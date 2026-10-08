#include "video_test.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <switch.h>

#include <glad/glad.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>

// The bridge's reader of a file at a web address (see the bridge's stream_io.rs).
struct HttpStream;
HttpStream *stremio_http_open(const char *url);
std::int64_t stremio_http_read(HttpStream *stream, std::uint8_t *buffer, std::size_t length);
void stremio_http_seek(HttpStream *stream, std::uint64_t position);
std::int64_t stremio_http_size(const HttpStream *stream);
void stremio_http_close(HttpStream *stream);
}

void log_line(const char *format, ...);

namespace nx
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr int kIoBuffer = 256 << 10;
// The sound goes to the console 1,024 samples (a 47th of a second) at a time, four such
// pieces ahead.
constexpr int kAudioRate = 48000, kAudioPiece = 1024, kAudioPieces = 4;
// Pictures decoded ahead of the one on screen.
constexpr std::size_t kPicturesAhead = 4;

double seconds_since(Clock::time_point then)
{
    return std::chrono::duration<double>(Clock::now() - then).count();
}

// FFmpeg's warnings and errors go to the app's log (a limited number of them: a bad
// stream can produce one per picture).
void ffmpeg_says(void *, int level, const char *format, va_list arguments)
{
    static std::atomic<int> lines{0};
    if (level > AV_LOG_WARNING || lines.fetch_add(1) > 200)
        return;
    char text[400];
    std::vsnprintf(text, sizeof text, format, arguments);
    for (char &letter : text)
        if (letter == '\n')
            letter = ' ';
    log_line("video: ffmpeg: %s", text);
}

struct PacketQueue
{
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<AVPacket *> packets;
    std::size_t bytes = 0;
    bool finished = false; // the file has no more

    void push(AVPacket *packet)
    {
        {
            const std::lock_guard lock{mutex};
            bytes += static_cast<std::size_t>(packet->size);
            packets.push_back(packet);
        }
        changed.notify_all();
    }
    // The next packet; null when the file is finished or the player is stopping.
    AVPacket *pop(const std::atomic<bool> &stop)
    {
        std::unique_lock lock{mutex};
        changed.wait(lock, [&] { return stop || finished || !packets.empty(); });
        if (stop || packets.empty())
            return nullptr;
        AVPacket *packet = packets.front();
        packets.pop_front();
        bytes -= static_cast<std::size_t>(packet->size);
        lock.unlock();
        changed.notify_all();
        return packet;
    }
    void clear()
    {
        const std::lock_guard lock{mutex};
        for (AVPacket *packet : packets)
            av_packet_free(&packet);
        packets.clear();
        bytes = 0;
    }
};

struct Picture
{
    AVFrame *frame = nullptr;
    double time = 0;
};
} // namespace

struct VideoTest::Session
{
    std::string url;
    std::atomic<bool> stop{false}, paused{false};

    // Reading.
    HttpStream *http = nullptr;
    std::int64_t position = 0;
    AVIOContext *io = nullptr;
    AVFormatContext *format = nullptr;
    int video_index = -1, audio_index = -1;
    AVCodecContext *video = nullptr, *audio = nullptr;
    AVBufferRef *device = nullptr;
    bool hardware = false;
    PacketQueue video_packets, audio_packets;

    // Pictures ready to show, in order.
    std::mutex picture_mutex;
    std::condition_variable picture_changed;
    std::deque<Picture> pictures;
    bool pictures_finished = false;

    // The clock: the time of the sound last heard, and when that was.
    std::mutex clock_mutex;
    double clock_time = 0;
    Clock::time_point clock_at = Clock::now();
    bool clock_running = false;
    bool sound_finished = false;

    std::mutex state_mutex;
    ui::Playback::State state = ui::Playback::State::Opening;
    std::string error;
    double duration = 0;

    std::thread reader, video_thread, audio_thread;

    // What gets logged.
    std::atomic<unsigned> decoded{0}, shown{0}, dropped{0};
    std::atomic<std::uint64_t> decode_microseconds{0}, draw_microseconds{0};
    Clock::time_point reported = Clock::now();

    ~Session()
    {
        for (Picture &picture : pictures)
            av_frame_free(&picture.frame);
        video_packets.clear();
        audio_packets.clear();
        if (video != nullptr)
            avcodec_free_context(&video);
        if (audio != nullptr)
            avcodec_free_context(&audio);
        if (format != nullptr)
            avformat_close_input(&format);
        if (io != nullptr)
        {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        if (device != nullptr)
            av_buffer_unref(&device);
        if (http != nullptr)
            stremio_http_close(http);
    }

    void fail(const std::string &why)
    {
        log_line("video: failed: %s", why.c_str());
        const std::lock_guard lock{state_mutex};
        state = ui::Playback::State::Failed;
        error = why;
    }

    double now()
    {
        const std::lock_guard lock{clock_mutex};
        if (!clock_running || paused)
            return clock_time;
        // Between pieces of sound the clock runs on by itself, a little way.
        return clock_time + std::min(seconds_since(clock_at), 0.1);
    }

    static int read(void *opaque, std::uint8_t *buffer, int size)
    {
        auto *self = static_cast<Session *>(opaque);
        if (self->stop)
            return AVERROR_EXIT;
        const std::int64_t count = stremio_http_read(self->http, buffer, static_cast<std::size_t>(size));
        if (count < 0)
            return AVERROR(EIO);
        if (count == 0)
            return AVERROR_EOF;
        self->position += count;
        return static_cast<int>(count);
    }

    static std::int64_t seek(void *opaque, std::int64_t offset, int whence)
    {
        auto *self = static_cast<Session *>(opaque);
        const std::int64_t size = stremio_http_size(self->http);
        if (whence & AVSEEK_SIZE)
            return size >= 0 ? size : AVERROR(ENOSYS);
        std::int64_t target = offset;
        switch (whence & ~AVSEEK_FORCE)
        {
        case SEEK_SET:
            break;
        case SEEK_CUR:
            target += self->position;
            break;
        case SEEK_END:
            if (size < 0)
                return AVERROR(ENOSYS);
            target += size;
            break;
        default:
            return AVERROR(EINVAL);
        }
        if (target < 0)
            return AVERROR(EINVAL);
        stremio_http_seek(self->http, static_cast<std::uint64_t>(target));
        self->position = target;
        return target;
    }

    // The hardware decoder's picture format, when the decoder offers it.
    static AVPixelFormat choose_format(AVCodecContext *, const AVPixelFormat *offered)
    {
        for (const AVPixelFormat *format = offered; *format != AV_PIX_FMT_NONE; ++format)
            if (*format == AV_PIX_FMT_NVTEGRA)
                return *format;
        log_line("video: the decoder does not offer the hardware format for this video");
        return offered[0];
    }

    bool open_video()
    {
        AVStream *stream = format->streams[video_index];
        const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr)
            return false;
        video = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(video, stream->codecpar);
        video->pkt_timebase = stream->time_base;
        // The console's decoder, when this kind of video has one.
        bool offered = false;
        for (int index = 0;; ++index)
        {
            const AVCodecHWConfig *config = avcodec_get_hw_config(codec, index);
            if (config == nullptr)
                break;
            if (config->device_type == AV_HWDEVICE_TYPE_NVTEGRA &&
                (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                offered = true;
        }
        if (offered)
        {
            const int made = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0);
            if (made >= 0)
            {
                video->hw_device_ctx = av_buffer_ref(device);
                video->get_format = choose_format;
                hardware = true;
            }
            else
            {
                char why[128] = {};
                av_strerror(made, why, sizeof why);
                log_line("video: the hardware decoder did not open (%s)", why);
            }
        }
        if (!hardware)
            video->thread_count = 3; // in software, on the cores an app has
        const int opened = avcodec_open2(video, codec, nullptr);
        if (opened < 0 && hardware)
        {
            // Once more, in software.
            log_line("video: the decoder did not open with the hardware decoder (%d); trying without", opened);
            avcodec_free_context(&video);
            video = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(video, stream->codecpar);
            video->pkt_timebase = stream->time_base;
            video->thread_count = 3;
            hardware = false;
            return avcodec_open2(video, codec, nullptr) >= 0;
        }
        return opened >= 0;
    }

    void read_file()
    {
        http = stremio_http_open(url.c_str());
        if (http == nullptr)
            return fail("The stream's address did not answer.");
        auto *buffer = static_cast<std::uint8_t *>(av_malloc(kIoBuffer));
        io = avio_alloc_context(buffer, kIoBuffer, 0, this, read, nullptr, seek);
        format = avformat_alloc_context();
        format->pb = io;
        const auto began = Clock::now();
        if (avformat_open_input(&format, nullptr, nullptr, nullptr) < 0)
            return fail("The file is not a video this app can read.");
        if (avformat_find_stream_info(format, nullptr) < 0)
            return fail("The video's streams could not be read.");
        video_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        audio_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, video_index, nullptr, 0);
        if (video_index < 0)
            return fail("The file has no video in it.");
        if (!open_video())
            return fail("This kind of video cannot be decoded.");
        const AVCodecParameters *shape = format->streams[video_index]->codecpar;
        const AVRational rate = av_guess_frame_rate(format, format->streams[video_index], nullptr);
        log_line("video: %s %dx%d, %s, %.2f pictures a second, %s decoding; opening took %.1f s",
                 avcodec_get_name(shape->codec_id), shape->width, shape->height,
                 av_get_pix_fmt_name(static_cast<AVPixelFormat>(shape->format)),
                 rate.den != 0 ? av_q2d(rate) : 0.0, hardware ? "hardware" : "software", seconds_since(began));
        if (audio_index >= 0)
        {
            AVStream *stream = format->streams[audio_index];
            const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
            audio = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
            if (audio != nullptr)
            {
                avcodec_parameters_to_context(audio, stream->codecpar);
                audio->pkt_timebase = stream->time_base;
                if (avcodec_open2(audio, codec, nullptr) < 0)
                    avcodec_free_context(&audio);
            }
            if (audio == nullptr)
            {
                log_line("video: the sound (%s) cannot be decoded; playing without",
                         avcodec_get_name(stream->codecpar->codec_id));
                audio_index = -1;
            }
            else
                log_line("video: sound is %s, %d channels at %d Hz", avcodec_get_name(stream->codecpar->codec_id),
                         stream->codecpar->ch_layout.nb_channels, stream->codecpar->sample_rate);
        }
        {
            const std::lock_guard lock{state_mutex};
            duration = format->duration > 0 ? static_cast<double>(format->duration) / AV_TIME_BASE : 0.0;
            state = ui::Playback::State::Playing;
        }
        video_thread = std::thread{[this] { decode_video(); }};
        audio_thread = std::thread{[this] { play_sound(); }};

        // The file's packets, each to its decoder's queue. Reading waits while the
        // decoders have plenty.
        while (!stop)
        {
            std::size_t video_waiting = 0, video_bytes = 0, sound_waiting = 0;
            {
                const std::lock_guard lock{video_packets.mutex};
                video_waiting = video_packets.packets.size();
                video_bytes = video_packets.bytes;
            }
            {
                const std::lock_guard lock{audio_packets.mutex};
                sound_waiting = audio_packets.packets.size();
            }
            // Plenty of both (or of the one there is): wait. Plenty of one and little of
            // the other: read on, the other's packets are further along the file (up to
            // a point: a file whose sound and picture lie far apart is not chased).
            const bool video_plenty = video_waiting >= 90 || video_bytes >= (24u << 20);
            const bool sound_plenty = audio_index < 0 || sound_waiting >= 60;
            if ((video_plenty && sound_plenty) || video_bytes >= (48u << 20) || sound_waiting >= 600)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            AVPacket *packet = av_packet_alloc();
            const int got = av_read_frame(format, packet);
            if (got < 0)
            {
                av_packet_free(&packet);
                if (got != AVERROR_EOF && !stop)
                    log_line("video: reading stopped (%d)", got);
                break;
            }
            if (packet->stream_index == video_index)
                video_packets.push(packet);
            else if (packet->stream_index == audio_index)
                audio_packets.push(packet);
            else
                av_packet_free(&packet);
        }
        for (PacketQueue *queue : {&video_packets, &audio_packets})
        {
            {
                const std::lock_guard lock{queue->mutex};
                queue->finished = true;
            }
            queue->changed.notify_all();
        }
    }

    // Turns a decoded picture into one whose samples are in ordinary memory, and queues it.
    void queue_picture(AVFrame *decoded)
    {
        AVFrame *plain = decoded;
        if (decoded->format == AV_PIX_FMT_NVTEGRA)
        {
            plain = av_frame_alloc();
            const int copied = av_hwframe_transfer_data(plain, decoded, 0);
            if (copied < 0)
            {
                static std::atomic<int> said{0};
                if (said.fetch_add(1) < 5)
                    log_line("video: a picture could not be taken from the hardware decoder (%d)", copied);
                av_frame_free(&plain);
                av_frame_free(&decoded);
                return;
            }
            plain->best_effort_timestamp = decoded->best_effort_timestamp;
            plain->pts = decoded->pts;
            av_frame_free(&decoded);
        }
        Picture picture;
        picture.frame = plain;
        const AVRational base = format->streams[video_index]->time_base;
        const std::int64_t stamp =
            plain->best_effort_timestamp != AV_NOPTS_VALUE ? plain->best_effort_timestamp : plain->pts;
        const std::int64_t start = format->streams[video_index]->start_time;
        picture.time = stamp != AV_NOPTS_VALUE
                           ? static_cast<double>(stamp - (start != AV_NOPTS_VALUE ? start : 0)) * av_q2d(base)
                           : 0.0;
        std::unique_lock lock{picture_mutex};
        picture_changed.wait(lock, [&] { return stop || pictures.size() < kPicturesAhead; });
        if (stop)
        {
            lock.unlock();
            av_frame_free(&picture.frame);
            return;
        }
        pictures.push_back(picture);
    }

    void decode_video()
    {
        const auto take = [this] {
            for (;;)
            {
                AVFrame *frame = av_frame_alloc();
                if (avcodec_receive_frame(video, frame) < 0)
                {
                    av_frame_free(&frame);
                    return;
                }
                ++decoded;
                queue_picture(frame);
            }
        };
        while (!stop)
        {
            AVPacket *packet = video_packets.pop(stop);
            if (packet == nullptr)
                break;
            const auto began = Clock::now();
            const int sent = avcodec_send_packet(video, packet);
            av_packet_free(&packet);
            if (sent < 0 && sent != AVERROR(EAGAIN))
            {
                static std::atomic<int> said{0};
                if (said.fetch_add(1) < 5)
                    log_line("video: a packet was refused by the decoder (%d)", sent);
            }
            decode_microseconds += static_cast<std::uint64_t>(seconds_since(began) * 1e6);
            take();
        }
        if (!stop)
        {
            avcodec_send_packet(video, nullptr);
            take();
        }
        {
            const std::lock_guard lock{picture_mutex};
            pictures_finished = true;
        }
    }

    // Decodes the sound, makes it what the console plays (two channels of 16-bit samples,
    // 48,000 a second) and feeds the console's output. The time of the sound last played
    // is the clock the pictures are shown by; without sound, the clock simply runs.
    void play_sound()
    {
        if (audio_index < 0 || audio == nullptr)
        {
            const auto began = Clock::now();
            double paused_for = 0;
            auto paused_at = began;
            bool was_paused = false;
            while (!stop)
            {
                const bool is_paused = paused;
                if (is_paused && !was_paused)
                    paused_at = Clock::now();
                if (!is_paused && was_paused)
                    paused_for += seconds_since(paused_at);
                was_paused = is_paused;
                if (!is_paused)
                {
                    const std::lock_guard lock{clock_mutex};
                    clock_time = seconds_since(began) - paused_for;
                    clock_at = Clock::now();
                    clock_running = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return;
        }
        const Result started = audoutInitialize();
        const Result running = R_SUCCEEDED(started) ? audoutStartAudioOut() : started;
        if (R_FAILED(running))
            log_line("video: the sound output did not start (0x%x)", running);
        SwrContext *resampler = nullptr;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, kAudioRate, &audio->ch_layout, audio->sample_fmt,
                            audio->sample_rate, 0, nullptr);
        if (resampler == nullptr || swr_init(resampler) < 0)
        {
            log_line("video: the sound could not be converted for the console");
            swr_free(&resampler);
        }
        // What is waiting to be played, and the time of its first sample.
        std::vector<std::int16_t> waiting;
        double waiting_time = 0;
        bool timed = false;
        // The pieces handed to the console, each with the time its last sample ends at.
        struct Piece
        {
            AudioOutBuffer buffer{};
            double ends = 0;
            bool out = false;
        };
        Piece pieces[kAudioPieces];
        for (Piece &piece : pieces)
        {
            piece.buffer.buffer = std::aligned_alloc(0x1000, 0x1000);
            piece.buffer.buffer_size = 0x1000;
            std::memset(piece.buffer.buffer, 0, 0x1000);
        }
        AVFrame *frame = av_frame_alloc();
        const AVRational base = format->streams[audio_index]->time_base;
        const std::int64_t first = format->streams[audio_index]->start_time;
        bool drained = false;
        int outstanding = 0;
        while (!stop)
        {
            // Decode until there is a piece's worth waiting.
            while (!stop && !drained && waiting.size() < static_cast<std::size_t>(kAudioPiece) * 2 * 2)
            {
                AVPacket *packet = audio_packets.pop(stop);
                if (packet == nullptr)
                {
                    drained = true;
                    break;
                }
                if (avcodec_send_packet(audio, packet) >= 0)
                {
                    while (avcodec_receive_frame(audio, frame) >= 0)
                    {
                        if (!timed && frame->best_effort_timestamp != AV_NOPTS_VALUE)
                        {
                            waiting_time = static_cast<double>(frame->best_effort_timestamp -
                                                               (first != AV_NOPTS_VALUE ? first : 0)) *
                                           av_q2d(base);
                            timed = true;
                        }
                        if (resampler != nullptr)
                        {
                            const int room = swr_get_out_samples(resampler, frame->nb_samples);
                            const std::size_t before = waiting.size();
                            waiting.resize(before + static_cast<std::size_t>(room) * 2);
                            auto *out = reinterpret_cast<std::uint8_t *>(waiting.data() + before);
                            const int made =
                                swr_convert(resampler, &out, room,
                                            const_cast<const std::uint8_t **>(frame->extended_data), frame->nb_samples);
                            waiting.resize(before + static_cast<std::size_t>(std::max(made, 0)) * 2);
                        }
                        av_frame_unref(frame);
                    }
                }
                av_packet_free(&packet);
            }
            if (paused || R_FAILED(running))
            {
                // Held: what is with the console plays out, and the clock stays where it is.
                if (R_FAILED(running) && !paused && !waiting.empty())
                {
                    // No output: let the clock run at the sound's pace regardless.
                    const std::size_t samples = std::min(waiting.size() / 2, static_cast<std::size_t>(kAudioPiece));
                    waiting.erase(waiting.begin(), waiting.begin() + static_cast<std::ptrdiff_t>(samples * 2));
                    waiting_time += static_cast<double>(samples) / kAudioRate;
                    const std::lock_guard lock{clock_mutex};
                    clock_time = waiting_time;
                    clock_at = Clock::now();
                    clock_running = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kAudioPiece * 1000 / kAudioRate));
                if (drained && waiting.empty())
                    break;
                continue;
            }
            // Hand over as many pieces as the console has room for.
            for (Piece &piece : pieces)
            {
                if (piece.out)
                    continue;
                if (waiting.size() < static_cast<std::size_t>(kAudioPiece) * 2)
                {
                    if (!drained || waiting.empty())
                        break;
                    waiting.resize(static_cast<std::size_t>(kAudioPiece) * 2, 0); // the last, padded out
                }
                std::memcpy(piece.buffer.buffer, waiting.data(), static_cast<std::size_t>(kAudioPiece) * 4);
                waiting.erase(waiting.begin(), waiting.begin() + kAudioPiece * 2);
                waiting_time += static_cast<double>(kAudioPiece) / kAudioRate;
                piece.ends = waiting_time;
                piece.buffer.data_size = static_cast<u64>(kAudioPiece) * 4;
                piece.buffer.data_offset = 0;
                if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&piece.buffer)))
                {
                    piece.out = true;
                    ++outstanding;
                }
            }
            if (outstanding == 0)
            {
                if (drained)
                    break;
                continue;
            }
            // Wait for the console to finish one; its end is the time now. (Each call
            // reports one piece, so any others that have finished are asked after too.)
            AudioOutBuffer *released = nullptr;
            u32 count = 0;
            Result waited = audoutWaitPlayFinish(&released, &count, 100'000'000ull);
            while (R_SUCCEEDED(waited) && count > 0 && released != nullptr)
            {
                for (Piece &piece : pieces)
                {
                    if (&piece.buffer != released || !piece.out)
                        continue;
                    piece.out = false;
                    --outstanding;
                    const std::lock_guard lock{clock_mutex};
                    clock_time = piece.ends;
                    clock_at = Clock::now();
                    clock_running = true;
                }
                released = nullptr;
                count = 0;
                waited = audoutGetReleasedAudioOutBuffer(&released, &count);
            }
        }
        {
            const std::lock_guard lock{clock_mutex};
            sound_finished = true;
        }
        av_frame_free(&frame);
        swr_free(&resampler);
        if (R_SUCCEEDED(running))
            audoutStopAudioOut();
        if (R_SUCCEEDED(started))
            audoutExit();
        for (Piece &piece : pieces)
            std::free(piece.buffer.buffer);
        // With the sound over, the clock runs on by itself to the end of the pictures.
        const double from = now();
        const auto began = Clock::now();
        while (!stop)
        {
            if (!paused)
            {
                const std::lock_guard lock{clock_mutex};
                clock_time = from + seconds_since(began);
                clock_at = Clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

VideoTest::VideoTest() = default;

VideoTest::~VideoTest()
{
    close();
}

void VideoTest::open(const std::string &url)
{
    close();
    av_log_set_callback(ffmpeg_says);
    session_ = std::make_unique<Session>();
    session_->url = url;
    has_picture_ = false;
    appletSetMediaPlaybackState(true); // the screen stays on while a video plays
    log_line("video: opening");
    session_->reader = std::thread{[session = session_.get()] { session->read_file(); }};
}

void VideoTest::close()
{
    if (session_ == nullptr)
        return;
    Session &session = *session_;
    session.stop = true;
    session.video_packets.changed.notify_all();
    session.audio_packets.changed.notify_all();
    session.picture_changed.notify_all();
    for (std::thread *thread : {&session.reader, &session.video_thread, &session.audio_thread})
        if (thread->joinable())
            thread->join();
    log_line("video: closed after %u pictures decoded, %u shown, %u dropped", session.decoded.load(),
             session.shown.load(), session.dropped.load());
    session_.reset();
    has_picture_ = false;
    appletSetMediaPlaybackState(false);
}

void VideoTest::set_paused(bool paused)
{
    if (session_ != nullptr)
        session_->paused = paused;
}

ui::Playback VideoTest::status() const
{
    ui::Playback playback;
    if (session_ == nullptr)
        return playback;
    Session &session = *session_;
    {
        const std::lock_guard lock{session.state_mutex};
        playback.state = session.state;
        playback.error = session.error;
        playback.duration = session.duration;
    }
    playback.position = session.now();
    if (playback.state == ui::Playback::State::Playing)
    {
        if (session.paused)
            playback.state = ui::Playback::State::Paused;
        playback.buffering = !has_picture_;
        // Over when the last picture has been shown.
        const std::lock_guard lock{session.picture_mutex};
        if (session.pictures_finished && session.pictures.empty() && has_picture_)
            playback.state = ui::Playback::State::Ended;
    }
    return playback;
}

bool VideoTest::create_program()
{
    static const char kVertex[] = "#version 330 core\n"
                                  "out vec2 uv;\n"
                                  "void main() {\n"
                                  "  vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
                                  "  uv = vec2(corner.x, 1.0 - corner.y);\n"
                                  "  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
                                  "}\n";
    // The picture's samples (brightness, and two of colour at half the size) made into
    // red, green and blue: the usual arrangement for HD video (BT.709, limited range).
    static const char kFragment[] =
        "#version 330 core\n"
        "in vec2 uv;\n"
        "out vec4 color;\n"
        "uniform sampler2D plane_y, plane_u, plane_v;\n"
        "uniform int planar;\n"
        "uniform float scale;\n"
        "void main() {\n"
        "  float y = texture(plane_y, uv).r * scale;\n"
        "  vec2 c = planar == 1 ? vec2(texture(plane_u, uv).r, texture(plane_v, uv).r) * scale\n"
        "                       : texture(plane_u, uv).rg * scale;\n"
        "  y = (y - 16.0 / 255.0) * (255.0 / 219.0);\n"
        "  c = (c - 128.0 / 255.0) * (255.0 / 224.0);\n"
        "  color = vec4(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x, 1.0);\n"
        "}\n";
    const auto compile = [](GLenum type, const char *source) {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE)
        {
            char text[400] = {};
            glGetShaderInfoLog(shader, sizeof text - 1, nullptr, text);
            log_line("video: shader failed: %s", text);
        }
        return shader;
    };
    const GLuint vertex = compile(GL_VERTEX_SHADER, kVertex), fragment = compile(GL_FRAGMENT_SHADER, kFragment);
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
    {
        log_line("video: the drawing program did not link");
        return false;
    }
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "plane_y"), 0);
    glUniform1i(glGetUniformLocation(program_, "plane_u"), 1);
    glUniform1i(glGetUniformLocation(program_, "plane_v"), 2);
    glUseProgram(0);
    glGenVertexArrays(1, &vertex_array_);
    glGenTextures(3, planes_);
    for (const GLuint plane : planes_)
    {
        glBindTexture(GL_TEXTURE_2D, plane);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

void VideoTest::draw(int left, int bottom, int width, int height)
{
    if (session_ == nullptr)
        return;
    Session &session = *session_;
    if (program_ == 0 && !create_program())
        return;

    // The picture that is due: the last of those whose time has come. Earlier ones that
    // were never shown are dropped.
    const double now = session.now();
    AVFrame *due = nullptr;
    {
        const std::lock_guard lock{session.picture_mutex};
        while (!session.pictures.empty() && (session.pictures.front().time <= now + 0.005 || !has_picture_))
        {
            if (due != nullptr)
            {
                av_frame_free(&due);
                ++session.dropped;
            }
            due = session.pictures.front().frame;
            session.pictures.pop_front();
            if (!has_picture_)
                break; // the very first picture is shown as soon as there is one
        }
    }
    session.picture_changed.notify_all();

    const auto began = Clock::now();
    if (due != nullptr)
    {
        // Its samples go into textures: one for brightness and one (two bytes a sample)
        // or two for colour, of 8 or of 16 bits.
        const auto format = static_cast<AVPixelFormat>(due->format);
        const AVPixFmtDescriptor *layout = av_pix_fmt_desc_get(format);
        const bool known = format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_P010 || format == AV_PIX_FMT_YUV420P ||
                           format == AV_PIX_FMT_YUV420P10 || format == AV_PIX_FMT_YUVJ420P;
        if (!known || layout == nullptr)
        {
            static std::atomic<int> said{0};
            if (said.fetch_add(1) < 3)
                log_line("video: pictures are %s, which this player does not draw", av_get_pix_fmt_name(format));
        }
        else
        {
            const bool wide = layout->comp[0].depth > 8;
            planar_ = format != AV_PIX_FMT_NV12 && format != AV_PIX_FMT_P010;
            // Ten-bit samples sit at the top of their 16 bits in one layout and at the
            // bottom in the other.
            scale_ = format == AV_PIX_FMT_YUV420P10 ? 65535.0f / 1023.0f : 1.0f;
            if (!has_picture_ || width_ != due->width || height_ != due->height)
                log_line("video: first picture, %dx%d %s", due->width, due->height, av_get_pix_fmt_name(format));
            width_ = due->width;
            height_ = due->height;
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            const int count = planar_ ? 3 : 2;
            for (int plane = 0; plane < count; ++plane)
            {
                const bool pairs = !planar_ && plane == 1;
                const int plane_width = plane == 0 ? due->width : (due->width + 1) / 2;
                const int plane_height = plane == 0 ? due->height : (due->height + 1) / 2;
                const int bytes = (wide ? 2 : 1) * (pairs ? 2 : 1);
                const GLint inside = pairs ? (wide ? GL_RG16 : GL_RG8) : (wide ? GL_R16 : GL_R8);
                glActiveTexture(GL_TEXTURE0 + plane);
                glBindTexture(GL_TEXTURE_2D, planes_[plane]);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, due->linesize[plane] / bytes);
                if (plane_width_[plane] != plane_width || plane_height_[plane] != plane_height ||
                    plane_format_[plane] != inside)
                {
                    glTexImage2D(GL_TEXTURE_2D, 0, inside, plane_width, plane_height, 0, pairs ? GL_RG : GL_RED,
                                 wide ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, due->data[plane]);
                    plane_width_[plane] = plane_width;
                    plane_height_[plane] = plane_height;
                    plane_format_[plane] = inside;
                }
                else
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, plane_width, plane_height, pairs ? GL_RG : GL_RED,
                                    wide ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, due->data[plane]);
            }
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            has_picture_ = true;
            ++session.shown;
        }
        av_frame_free(&due);
    }
    if (!has_picture_)
        return;

    // Fitted to the area, keeping its shape.
    const float shape = static_cast<float>(width_) / static_cast<float>(std::max(1, height_));
    int shown_width = width, shown_height = static_cast<int>(static_cast<float>(width) / shape);
    if (shown_height > height)
    {
        shown_height = height;
        shown_width = static_cast<int>(static_cast<float>(height) * shape);
    }
    GLint before[4] = {};
    glGetIntegerv(GL_VIEWPORT, before);
    glViewport(left + (width - shown_width) / 2, bottom + (height - shown_height) / 2, shown_width, shown_height);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "planar"), planar_ ? 1 : 0);
    glUniform1f(glGetUniformLocation(program_, "scale"), scale_);
    for (int plane = 0; plane < 3; ++plane)
    {
        glActiveTexture(GL_TEXTURE0 + plane);
        glBindTexture(GL_TEXTURE_2D, planes_[plane]);
    }
    glBindVertexArray(vertex_array_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    for (int plane = 2; plane >= 0; --plane)
    {
        glActiveTexture(GL_TEXTURE0 + plane);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glUseProgram(0);
    glViewport(before[0], before[1], before[2], before[3]);
    session.draw_microseconds += static_cast<std::uint64_t>(seconds_since(began) * 1e6);

    // Every few seconds, how it is going.
    if (seconds_since(session.reported) > 5.0)
    {
        session.reported = Clock::now();
        const unsigned decoded = session.decoded, shown = session.shown;
        std::size_t waiting_video = 0, waiting_sound = 0;
        {
            const std::lock_guard lock{session.video_packets.mutex};
            waiting_video = session.video_packets.packets.size();
        }
        {
            const std::lock_guard lock{session.audio_packets.mutex};
            waiting_sound = session.audio_packets.packets.size();
        }
        log_line("video: at %.1f s: %u decoded (%.1f ms each), %u shown (%.1f ms each to draw), %u dropped; "
                 "%zu video and %zu sound packets waiting",
                 now, decoded,
                 decoded != 0 ? static_cast<double>(session.decode_microseconds) / 1e3 / decoded : 0.0, shown,
                 shown != 0 ? static_cast<double>(session.draw_microseconds) / 1e3 / shown : 0.0,
                 session.dropped.load(), waiting_video, waiting_sound);
    }
}
} // namespace nx
