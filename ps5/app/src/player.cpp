#include "player.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <GL/gl.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>

struct HttpStream;
HttpStream *stremio_http_open(const char *url);
std::int64_t stremio_http_read(HttpStream *stream, std::uint8_t *buffer, std::size_t length);
void stremio_http_seek(HttpStream *stream, std::uint64_t position);
std::int64_t stremio_http_size(const HttpStream *stream);
void stremio_http_close(HttpStream *stream);

int sceAudioOutInit(void);
int sceAudioOutOpen(int user, int type, int index, std::uint32_t length, std::uint32_t frequency,
                    std::uint32_t format);
int sceAudioOutClose(int handle);
int sceAudioOutOutput(int handle, const void *samples);
int sceKernelDebugOutText(int channel, const char *text);
}

namespace ps5
{
namespace
{
constexpr int kAudioRate = 48000;
constexpr std::size_t kAudioGrain = 256; // frames the audio output takes at a time
constexpr int kDecodeThreads = 12;
// Decoded pictures kept ready. Each 4K picture is some 25 MB, so the queue is short; the
// look-ahead is held as packets instead.
constexpr std::size_t kPictures = 5;
// How much is read ahead of what is playing.
constexpr std::size_t kPacketsAhead = 150;
constexpr std::size_t kBytesAhead = std::size_t{96} << 20;
constexpr int kIoBuffer = 1 << 19;

double now_seconds()
{
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) * 1e-9;
}

void note(const char *format, ...)
{
    char line[400] = "[stremio] player: ";
    const std::size_t lead = std::strlen(line);
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line + lead, sizeof line - lead - 2, format, arguments);
    va_end(arguments);
    std::strcat(line, "\n");
    sceKernelDebugOutText(0, line);
}

std::string ffmpeg_error(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return text;
}

struct PacketItem
{
    AVPacket *packet;
    int serial;
};

struct Picture
{
    AVFrame *frame;
    double pts;
    int serial;
};
} // namespace

struct Player::Session
{
    std::string url;
    std::atomic<bool> stop{false};

    // Everything from here to the FFmpeg objects is guarded by `mutex`; `wake` is
    // signalled whenever any of it changes.
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool opened = false; // the file is open and its streams are known
    bool failed = false;
    std::string error;
    double duration = 0;
    bool paused = false;
    bool seek_wanted = false;
    double seek_to = 0;
    // Each seek starts a new serial; packets and pictures carry the serial they belong
    // to, so what was in flight when a seek happened is recognised and dropped.
    int serial = 0;
    double start_from = 0; // where the current serial starts playing
    std::deque<PacketItem> video_packets, audio_packets;
    std::size_t queued_bytes = 0;
    std::deque<Picture> pictures;
    bool reader_done = false; // the end of the file has been read
    bool video_done = false;  // ... and decoded
    bool audio_done = false;
    bool video_primed = false; // the current serial's first picture is ready
    bool starved = false;      // playing, but there is nothing to play
    bool has_video = false, has_audio = false;
    // The clock: the time in the video (`clock_pts`) that was reached at `clock_at`.
    double clock_pts = 0, clock_at = 0;
    bool clock_running = false;

    // FFmpeg. After opening, each object is used by one thread only.
    HttpStream *http = nullptr;
    std::int64_t io_position = 0;
    AVIOContext *io = nullptr;
    AVFormatContext *format = nullptr;
    AVCodecContext *video = nullptr, *audio = nullptr;
    SwrContext *resampler = nullptr;
    int video_index = -1, audio_index = -1;
    double video_base = 0, audio_base = 0; // seconds per timestamp unit
    double origin = 0;                     // the file's first timestamp, in seconds
    std::thread reader, video_thread, audio_thread;

    ~Session()
    {
        for (PacketItem &item : video_packets)
            av_packet_free(&item.packet);
        for (PacketItem &item : audio_packets)
            av_packet_free(&item.packet);
        for (Picture &picture : pictures)
            av_frame_free(&picture.frame);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        swr_free(&resampler);
        if (format != nullptr)
            avformat_close_input(&format);
        if (io != nullptr)
        {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        stremio_http_close(http);
    }

    void fail(const std::string &why)
    {
        note("failed: %s", why.c_str());
        std::lock_guard lock{mutex};
        failed = true;
        error = why;
        wake.notify_all();
    }

    // With `mutex` held.
    double clock() const
    {
        if (!clock_running || paused)
            return clock_pts;
        // The clock runs on between updates, but not far: when the updates stop (nothing
        // left to play), so does it.
        return clock_pts + std::min(now_seconds() - clock_at, 0.1);
    }

    static int read_io(void *opaque, std::uint8_t *buffer, int size)
    {
        auto *self = static_cast<Session *>(opaque);
        if (self->stop)
            return AVERROR_EXIT;
        const std::int64_t count =
            stremio_http_read(self->http, buffer, static_cast<std::size_t>(size));
        if (count < 0)
            return AVERROR(EIO);
        if (count == 0)
            return AVERROR_EOF;
        self->io_position += count;
        return static_cast<int>(count);
    }

    static std::int64_t seek_io(void *opaque, std::int64_t offset, int whence)
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
            target += self->io_position;
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
        self->io_position = target;
        return target;
    }

    AVCodecContext *open_decoder(int index, int threads)
    {
        const AVStream *stream = format->streams[index];
        const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr)
            return nullptr;
        AVCodecContext *context = avcodec_alloc_context3(codec);
        if (context == nullptr || avcodec_parameters_to_context(context, stream->codecpar) < 0)
        {
            avcodec_free_context(&context);
            return nullptr;
        }
        context->pkt_timebase = stream->time_base;
        if (threads > 1)
        {
            context->thread_count = threads;
            context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        if (avcodec_open2(context, codec, nullptr) < 0)
            avcodec_free_context(&context);
        return context;
    }

    bool open_file()
    {
        http = stremio_http_open(url.c_str());
        if (http == nullptr)
        {
            fail("The stream's address did not answer.");
            return false;
        }
        auto *buffer = static_cast<std::uint8_t *>(av_malloc(kIoBuffer));
        io = avio_alloc_context(buffer, kIoBuffer, 0, this, read_io, nullptr, seek_io);
        format = avformat_alloc_context();
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        format->interrupt_callback = {[](void *opaque) -> int {
                                          return static_cast<Session *>(opaque)->stop ? 1 : 0;
                                      },
                                      this};
        int result = avformat_open_input(&format, nullptr, nullptr, nullptr);
        if (result < 0)
        {
            format = nullptr; // freed by the failed call
            fail("The file is not a video this app can read (" + ffmpeg_error(result) + ").");
            return false;
        }
        result = avformat_find_stream_info(format, nullptr);
        if (result < 0)
        {
            fail("The video's streams could not be read (" + ffmpeg_error(result) + ").");
            return false;
        }
        video_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        audio_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, video_index, nullptr, 0);
        if (video_index < 0)
        {
            fail("The file has no video.");
            return false;
        }
        const AVCodecParameters *parameters = format->streams[video_index]->codecpar;
        video = open_decoder(video_index, kDecodeThreads);
        if (video == nullptr)
        {
            fail(std::string{"Video in the "} + avcodec_get_name(parameters->codec_id) +
                 " format is not supported yet.");
            return false;
        }
        video_base = av_q2d(format->streams[video_index]->time_base);
        if (audio_index >= 0)
        {
            audio = open_decoder(audio_index, 1);
            audio_base = av_q2d(format->streams[audio_index]->time_base);
            if (audio == nullptr)
                note("no decoder for the audio (%s); playing without sound",
                     avcodec_get_name(format->streams[audio_index]->codecpar->codec_id));
        }
        origin = format->start_time != AV_NOPTS_VALUE
                     ? static_cast<double>(format->start_time) / AV_TIME_BASE
                     : 0.0;
        note("opened: %s %dx%d %s, audio %s, %.0f s", avcodec_get_name(parameters->codec_id),
             parameters->width, parameters->height,
             av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters->format)),
             audio != nullptr ? avcodec_get_name(audio->codec_id) : "none",
             format->duration != AV_NOPTS_VALUE ? static_cast<double>(format->duration) / AV_TIME_BASE
                                                : 0.0);
        std::lock_guard lock{mutex};
        has_video = true;
        has_audio = audio != nullptr;
        duration = format->duration != AV_NOPTS_VALUE
                       ? static_cast<double>(format->duration) / AV_TIME_BASE
                       : 0.0;
        opened = true;
        return true;
    }

    // With `mutex` held: whether enough has been read ahead for now.
    bool read_enough() const
    {
        if (queued_bytes > kBytesAhead)
            return true;
        return video_packets.size() > kPacketsAhead &&
               (!has_audio || audio_packets.size() > kPacketsAhead);
    }

    void run_reader()
    {
        if (open_file())
        {
            video_thread = std::thread{[this] { run_video(); }};
            audio_thread = std::thread{[this] { run_audio(); }};
        }
        AVPacket *packet = av_packet_alloc();
        while (!stop)
        {
            double target = 0;
            bool seeking = false;
            {
                std::unique_lock lock{mutex};
                if (failed)
                {
                    wake.wait_for(lock, std::chrono::milliseconds(100));
                    continue;
                }
                if (seek_wanted)
                {
                    seeking = true;
                    target = seek_to;
                }
                else if (reader_done || read_enough())
                {
                    wake.wait_for(lock, std::chrono::milliseconds(20));
                    continue;
                }
            }
            if (seeking)
            {
                const auto stamp = static_cast<std::int64_t>((target + origin) * AV_TIME_BASE);
                const int result = avformat_seek_file(format, -1, INT64_MIN, stamp, stamp, 0);
                std::lock_guard lock{mutex};
                // A newer request may have come in meanwhile; it is carried out next.
                if (seek_to == target)
                    seek_wanted = false;
                if (result < 0)
                {
                    // Playback carries on from where it was.
                    note("seek to %.1f failed: %s", target, ffmpeg_error(result).c_str());
                    continue;
                }
                for (PacketItem &item : video_packets)
                    av_packet_free(&item.packet);
                for (PacketItem &item : audio_packets)
                    av_packet_free(&item.packet);
                video_packets.clear();
                audio_packets.clear();
                queued_bytes = 0;
                ++serial;
                start_from = target;
                reader_done = video_done = audio_done = false;
                video_primed = false;
                starved = false;
                clock_pts = target;
                clock_running = false;
                wake.notify_all();
                continue;
            }
            const int result = av_read_frame(format, packet);
            if (result < 0)
            {
                if (result != AVERROR_EOF && !stop)
                    note("reading stopped: %s", ffmpeg_error(result).c_str());
                std::lock_guard lock{mutex};
                reader_done = true;
                wake.notify_all();
                continue;
            }
            const bool is_video = packet->stream_index == video_index;
            const bool is_audio = packet->stream_index == audio_index && audio != nullptr;
            if (is_video || is_audio)
            {
                AVPacket *kept = av_packet_alloc();
                av_packet_move_ref(kept, packet);
                std::lock_guard lock{mutex};
                (is_video ? video_packets : audio_packets).push_back({kept, serial});
                queued_bytes += static_cast<std::size_t>(kept->size);
                wake.notify_all();
            }
            else
            {
                av_packet_unref(packet);
            }
        }
        av_packet_free(&packet);
        {
            std::lock_guard lock{mutex};
            wake.notify_all();
        }
        if (video_thread.joinable())
            video_thread.join();
        if (audio_thread.joinable())
            audio_thread.join();
    }

    void run_video()
    {
        AVFrame *frame = av_frame_alloc();
        int mine = -1;      // the serial being decoded
        bool drained = false;
        while (!stop)
        {
            AVPacket *packet = nullptr;
            int packet_serial = 0;
            {
                std::unique_lock lock{mutex};
                if (!video_packets.empty())
                {
                    packet = video_packets.front().packet;
                    packet_serial = video_packets.front().serial;
                    video_packets.pop_front();
                    queued_bytes -= static_cast<std::size_t>(packet->size);
                    wake.notify_all();
                }
                else if (!(reader_done && !drained && mine == serial))
                {
                    wake.wait_for(lock, std::chrono::milliseconds(20));
                    continue;
                }
            }
            if (packet != nullptr && packet_serial != mine)
            {
                avcodec_flush_buffers(video);
                mine = packet_serial;
                drained = false;
            }
            // A null packet tells the decoder the file has ended, so it hands over the
            // pictures it was still holding.
            int result = avcodec_send_packet(video, packet);
            av_packet_free(&packet);
            if (result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF)
                continue; // a damaged packet; the next ones may be fine
            for (;;)
            {
                result = avcodec_receive_frame(video, frame);
                if (result == AVERROR_EOF)
                {
                    drained = true;
                    avcodec_flush_buffers(video);
                    std::lock_guard lock{mutex};
                    if (mine == serial)
                    {
                        video_done = true;
                        video_primed = true;
                        wake.notify_all();
                    }
                    break;
                }
                if (result < 0)
                    break;
                const std::int64_t stamp = frame->best_effort_timestamp;
                const double pts =
                    stamp != AV_NOPTS_VALUE ? static_cast<double>(stamp) * video_base - origin : 0.0;
                std::unique_lock lock{mutex};
                // After a seek the decoder starts from the key frame before the target;
                // the pictures in between are not shown.
                if (mine == serial && pts >= start_from - 0.02)
                {
                    wake.wait(lock, [&] {
                        return stop || mine != serial || pictures.size() < kPictures;
                    });
                    if (!stop && mine == serial)
                    {
                        pictures.push_back({av_frame_clone(frame), pts, mine});
                        video_primed = true;
                        wake.notify_all();
                    }
                }
                lock.unlock();
                av_frame_unref(frame);
                if (stop)
                    break;
            }
        }
        av_frame_free(&frame);
    }

    void run_audio()
    {
        int handle = -1;
        const int started = sceAudioOutInit();
        if (started >= 0 || started == static_cast<int>(0x8026000e))
            handle = sceAudioOutOpen(0xff, 0, 0, kAudioGrain, kAudioRate, 1);
        if (handle < 0)
            note("audio output did not open (0x%x)", handle);

        AVFrame *frame = av_frame_alloc();
        std::vector<std::int16_t> pcm; // stereo, waiting to be played
        std::size_t played = 0;        // samples of `pcm` already played
        double pcm_pts = 0;            // the time of pcm[played]
        int mine = -1;
        bool drained = false;
        double last = now_seconds();
        while (!stop)
        {
            AVPacket *packet = nullptr;
            bool silent = false; // nothing to decode: the clock runs on its own
            {
                std::unique_lock lock{mutex};
                const double now = now_seconds();
                const double elapsed = now - last;
                last = now;
                if (mine != serial)
                {
                    mine = serial;
                    drained = false;
                    pcm.clear();
                    played = 0;
                    if (audio != nullptr)
                        avcodec_flush_buffers(audio);
                }
                if (!opened || failed || paused || !video_primed)
                {
                    wake.wait_for(lock, std::chrono::milliseconds(10));
                    last = now_seconds();
                    continue;
                }
                silent = audio == nullptr || handle < 0 || (drained && pcm.size() - played < kAudioGrain * 2);
                if (silent)
                {
                    if (audio != nullptr)
                        audio_done = true;
                    // Sound that cannot be played is not left to pile up.
                    for (PacketItem &item : audio_packets)
                    {
                        queued_bytes -= static_cast<std::size_t>(item.packet->size);
                        av_packet_free(&item.packet);
                    }
                    audio_packets.clear();
                    // Without sound the clock follows real time, and waits when the
                    // pictures run out before the file does.
                    starved = pictures.empty() && !video_done;
                    if (!starved)
                        clock_pts += std::min(elapsed, 0.05);
                    clock_at = now;
                    clock_running = true;
                    wake.wait_for(lock, std::chrono::milliseconds(4));
                    continue;
                }
                if (pcm.size() - played < kAudioGrain * 2)
                {
                    if (!audio_packets.empty())
                    {
                        packet = audio_packets.front().packet;
                        audio_packets.pop_front();
                        queued_bytes -= static_cast<std::size_t>(packet->size);
                        wake.notify_all();
                    }
                    else if (!reader_done)
                    {
                        starved = true;
                        wake.wait_for(lock, std::chrono::milliseconds(10));
                        continue;
                    }
                }
            }

            if (pcm.size() - played < kAudioGrain * 2)
            {
                // Decode some more: the next packet, or at the end of the file whatever
                // the decoder was still holding.
                if (played > 0)
                {
                    pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(played));
                    played = 0;
                }
                const int sent = avcodec_send_packet(audio, packet);
                av_packet_free(&packet);
                if (sent < 0 && sent != AVERROR(EAGAIN) && sent != AVERROR_EOF)
                    continue;
                for (;;)
                {
                    const int result = avcodec_receive_frame(audio, frame);
                    if (result == AVERROR_EOF)
                        drained = true;
                    if (result < 0)
                        break;
                    append(frame, pcm, pcm_pts);
                    av_frame_unref(frame);
                }
                continue;
            }

            // Hands one block to the output, which waits until it has room for it; that
            // wait is what paces this thread.
            sceAudioOutOutput(handle, pcm.data() + played);
            {
                std::lock_guard lock{mutex};
                if (mine == serial)
                {
                    clock_pts = pcm_pts;
                    clock_at = now_seconds();
                    clock_running = true;
                    starved = false;
                }
            }
            played += kAudioGrain * 2;
            pcm_pts += static_cast<double>(kAudioGrain) / kAudioRate;
        }
        av_frame_free(&frame);
        if (handle >= 0)
            sceAudioOutClose(handle);
    }

    // Converts a decoded frame to 48 kHz stereo and adds it to `pcm`, leaving out what
    // comes before the point playback starts from (after a seek).
    void append(const AVFrame *frame, std::vector<std::int16_t> &pcm, double &pcm_pts)
    {
        if (resampler == nullptr)
        {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, kAudioRate,
                                    &frame->ch_layout, static_cast<AVSampleFormat>(frame->format),
                                    frame->sample_rate, 0, nullptr) < 0 ||
                swr_init(resampler) < 0)
            {
                swr_free(&resampler);
                return;
            }
        }
        const int room = swr_get_out_samples(resampler, frame->nb_samples);
        if (room <= 0)
            return;
        const std::size_t before = pcm.size();
        pcm.resize(before + static_cast<std::size_t>(room) * 2);
        auto *out = reinterpret_cast<std::uint8_t *>(pcm.data() + before);
        const int made = swr_convert(resampler, &out, room,
                                     const_cast<const std::uint8_t **>(frame->extended_data),
                                     frame->nb_samples);
        pcm.resize(before + static_cast<std::size_t>(std::max(made, 0)) * 2);
        if (made <= 0)
            return;
        double from;
        {
            std::lock_guard lock{mutex};
            from = start_from;
        }
        if (before == 0)
        {
            pcm_pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                          ? static_cast<double>(frame->best_effort_timestamp) * audio_base - origin
                          : from;
            if (pcm_pts < from)
            {
                const auto early = static_cast<std::size_t>((from - pcm_pts) * kAudioRate);
                const std::size_t drop = std::min(early, pcm.size() / 2);
                pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(drop * 2));
                pcm_pts += static_cast<double>(drop) / kAudioRate;
            }
        }
    }
};

Player::Player() = default;

Player::~Player()
{
    close();
}

void Player::open(const std::string &url)
{
    close();
    av_log_set_level(AV_LOG_ERROR);
    session_ = std::make_shared<Session>();
    session_->url = url;
    has_picture_ = false;
    shown_serial_ = -1;
    skipped_ = 0;
    Session *session = session_.get();
    session->reader = std::thread{[session] { session->run_reader(); }};
}

void Player::close()
{
    if (session_ == nullptr)
        return;
    std::shared_ptr<Session> session = std::move(session_);
    session_.reset();
    session->stop = true;
    {
        std::lock_guard lock{session->mutex};
        session->wake.notify_all();
    }
    has_picture_ = false;
    // The threads may be inside a network read; they are waited for out of the way.
    std::thread{[session] {
        if (session->reader.joinable())
            session->reader.join();
    }}.detach();
}

void Player::set_paused(bool paused)
{
    if (session_ == nullptr)
        return;
    std::lock_guard lock{session_->mutex};
    session_->paused = paused;
    session_->clock_at = now_seconds();
    session_->wake.notify_all();
}

void Player::seek(double seconds)
{
    if (session_ == nullptr)
        return;
    std::lock_guard lock{session_->mutex};
    session_->seek_to = std::max(0.0, seconds);
    session_->seek_wanted = true;
    session_->wake.notify_all();
}

ui::Playback Player::status() const
{
    ui::Playback playback;
    if (session_ == nullptr)
        return playback;
    const Session &session = *session_;
    std::lock_guard lock{session.mutex};
    playback.duration = session.duration;
    playback.position = session.seek_wanted ? session.seek_to : session.clock();
    if (session.duration > 0)
        playback.position = std::min(playback.position, session.duration);
    if (session.failed)
    {
        playback.state = ui::Playback::State::Failed;
        playback.error = session.error;
    }
    else if (!session.opened || (!has_picture_ && !session.video_primed))
    {
        playback.state = ui::Playback::State::Opening;
    }
    else if (session.reader_done && session.video_done && session.pictures.empty() &&
             (!session.has_audio || session.audio_done) && !session.seek_wanted)
    {
        playback.state = ui::Playback::State::Ended;
    }
    else
    {
        playback.state =
            session.paused ? ui::Playback::State::Paused : ui::Playback::State::Playing;
        playback.buffering =
            !session.paused && (session.seek_wanted || !session.video_primed || session.starved);
    }
    return playback;
}

bool Player::create_program()
{
    static const char kVertex[] = R"(#version 330 core
out vec2 uv;
void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = vec2(corner.x, 1.0 - corner.y);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";
    // Y'CbCr to R'G'B', and for HDR video on to SDR: to light (PQ or HLG), to the BT.709
    // primaries, the brightest channel brought under SDR white along the BT.2390 curve
    // (dark and mid tones are left alone, highlights are rolled off), and back to a
    // gamma signal.
    static const char kFragment[] = R"(#version 330 core
in vec2 uv;
out vec4 color;
uniform sampler2D plane_y;
uniform sampler2D plane_u;
uniform sampler2D plane_v;
uniform vec2 luma;    // scale and offset
uniform vec2 chroma;
uniform vec2 weights; // the red and blue luma weights
uniform int transfer; // 0 SDR, 1 PQ, 2 HLG
uniform int wide;     // BT.2020 primaries
uniform float peak;   // the video's brightest white, in nits
const float m1 = 0.1593017578125, m2 = 78.84375;
const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
const float white = 203.0;
vec3 pq_to_nits(vec3 signal) {
    vec3 p = pow(max(signal, 0.0), vec3(1.0 / m2));
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1));
}
float nits_to_pq(float nits) {
    float y = pow(max(nits, 0.0) / 10000.0, m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
vec3 hlg_to_nits(vec3 signal) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    vec3 scene = mix(signal * signal / 3.0, (exp((signal - c) / a) + b) / 12.0,
                     step(0.5, signal));
    float y = dot(scene, vec3(0.2627, 0.6780, 0.0593));
    return 1000.0 * pow(max(y, 1e-6), 0.2) * scene;
}
float roll_off(float nits) {
    float source = nits_to_pq(peak);
    float limit = nits_to_pq(white) / source;
    float knee = 1.5 * limit - 0.5;
    float e = min(nits_to_pq(nits) / source, 1.0);
    if (e > knee) {
        float t = (e - knee) / (1.0 - knee);
        float t2 = t * t, t3 = t2 * t;
        e = (2.0 * t3 - 3.0 * t2 + 1.0) * knee + (t3 - 2.0 * t2 + t) * (1.0 - knee) +
            (-2.0 * t3 + 3.0 * t2) * limit;
    }
    return pq_to_nits(vec3(e * source)).x;
}
void main() {
    float y = texture(plane_y, uv).r * luma.x + luma.y;
    float cb = texture(plane_u, uv).r * chroma.x + chroma.y;
    float cr = texture(plane_v, uv).r * chroma.x + chroma.y;
    float kr = weights.x, kb = weights.y;
    float r = y + 2.0 * (1.0 - kr) * cr;
    float b = y + 2.0 * (1.0 - kb) * cb;
    float g = (y - kr * r - kb * b) / (1.0 - kr - kb);
    vec3 rgb = clamp(vec3(r, g, b), 0.0, 1.0);
    if (transfer != 0) {
        vec3 nits = transfer == 1 ? pq_to_nits(rgb) : hlg_to_nits(rgb);
        if (wide != 0)
            nits = max(mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006,
                            -0.0728, -0.0083, 1.1187) * nits, 0.0);
        float top = max(max(nits.r, nits.g), max(nits.b, 1e-4));
        nits *= roll_off(top) / top;
        rgb = pow(clamp(nits / white, 0.0, 1.0), vec3(1.0 / 2.4));
    }
    color = vec4(rgb, 1.0);
}
)";
    const auto compile = [](GLenum type, const char *source) {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE)
        {
            char text[300] = {};
            glGetShaderInfoLog(shader, sizeof text - 1, nullptr, text);
            note("shader failed: %s", text);
        }
        return shader;
    };
    const GLuint vertex = compile(GL_VERTEX_SHADER, kVertex);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, kFragment);
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
        note("picture program did not link");
        glDeleteProgram(program_);
        program_ = 0;
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

// Copies a decoded picture's three planes into textures and sets the program up for its
// colours. False for a picture layout this does not handle.
bool Player::upload(const void *picture)
{
    const auto *frame = static_cast<const AVFrame *>(picture);
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor *layout = av_pix_fmt_desc_get(format);
    if (layout == nullptr || !(layout->flags & AV_PIX_FMT_FLAG_PLANAR) ||
        (layout->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_BE)) || layout->nb_components < 3)
        return false;
    const int depth = layout->comp[0].depth;
    const int bytes = depth > 8 ? 2 : 1;
    const int widths[3] = {frame->width, AV_CEIL_RSHIFT(frame->width, layout->log2_chroma_w),
                           AV_CEIL_RSHIFT(frame->width, layout->log2_chroma_w)};
    const int heights[3] = {frame->height, AV_CEIL_RSHIFT(frame->height, layout->log2_chroma_h),
                            AV_CEIL_RSHIFT(frame->height, layout->log2_chroma_h)};
    const bool fresh =
        plane_format_ != frame->format || plane_width_ != frame->width || plane_height_ != frame->height;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int plane = 0; plane < 3; ++plane)
    {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(plane));
        glBindTexture(GL_TEXTURE_2D, planes_[plane]);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[plane] / bytes);
        const GLenum type = bytes == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
        if (fresh)
            glTexImage2D(GL_TEXTURE_2D, 0, bytes == 2 ? GL_R16 : GL_R8, widths[plane],
                         heights[plane], 0, GL_RED, type, frame->data[plane]);
        else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, widths[plane], heights[plane], GL_RED, type,
                            frame->data[plane]);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glActiveTexture(GL_TEXTURE0);
    if (!fresh)
        return true;
    plane_format_ = frame->format;
    plane_width_ = frame->width;
    plane_height_ = frame->height;

    // The colours are the same for every picture of a video, so they are set up once.
    const bool pq = frame->color_trc == AVCOL_TRC_SMPTE2084;
    const bool hlg = frame->color_trc == AVCOL_TRC_ARIB_STD_B67;
    const bool wide = frame->color_primaries == AVCOL_PRI_BT2020 || pq || hlg;
    float kr = 0.2126f, kb = 0.0722f; // BT.709
    switch (frame->colorspace)
    {
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        kr = 0.2627f, kb = 0.0593f;
        break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        kr = 0.299f, kb = 0.114f;
        break;
    case AVCOL_SPC_BT709:
        break;
    default:
        // Not said: HDR is BT.2020, small pictures are BT.601, the rest BT.709.
        if (wide)
            kr = 0.2627f, kb = 0.0593f;
        else if (frame->height < 720)
            kr = 0.299f, kb = 0.114f;
        break;
    }
    const bool full = frame->color_range == AVCOL_RANGE_JPEG || format == AV_PIX_FMT_YUVJ420P ||
                      format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P;
    // A texture sample is the stored number over the texture type's largest; the video's
    // numbers only use `depth` bits of it.
    const float largest = bytes == 2 ? 65535.0f : 255.0f;
    const float step = static_cast<float>(1 << (depth - 8));
    const float top = static_cast<float>((1 << depth) - 1);
    float luma[2], chroma[2];
    if (full)
    {
        luma[0] = largest / top, luma[1] = 0.0f;
        chroma[0] = largest / top, chroma[1] = -128.0f * step / top;
    }
    else
    {
        luma[0] = largest / (219.0f * step), luma[1] = -16.0f / 219.0f;
        chroma[0] = largest / (224.0f * step), chroma[1] = -128.0f / 224.0f;
    }
    float peak = 1000.0f;
    if (const AVFrameSideData *side =
            av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA))
    {
        const auto *mastering = reinterpret_cast<const AVMasteringDisplayMetadata *>(side->data);
        if (mastering->has_luminance)
            peak = static_cast<float>(av_q2d(mastering->max_luminance));
    }
    peak = std::clamp(peak, 400.0f, 4000.0f);
    glUseProgram(program_);
    glUniform2f(glGetUniformLocation(program_, "luma"), luma[0], luma[1]);
    glUniform2f(glGetUniformLocation(program_, "chroma"), chroma[0], chroma[1]);
    glUniform2f(glGetUniformLocation(program_, "weights"), kr, kb);
    glUniform1i(glGetUniformLocation(program_, "transfer"), pq ? 1 : hlg ? 2 : 0);
    glUniform1i(glGetUniformLocation(program_, "wide"), wide ? 1 : 0);
    glUniform1f(glGetUniformLocation(program_, "peak"), peak);
    glUseProgram(0);
    const AVRational shape = frame->sample_aspect_ratio;
    picture_aspect_ = static_cast<float>(frame->width) / static_cast<float>(frame->height) *
                      (shape.num > 0 && shape.den > 0 ? static_cast<float>(av_q2d(shape)) : 1.0f);
    note("picture: %dx%d %s, %s, %s range, peak %.0f nits, gl error 0x%x", frame->width,
         frame->height, av_get_pix_fmt_name(format), pq ? "PQ" : hlg ? "HLG" : "SDR",
         full ? "full" : "limited", peak, glGetError());
    return true;
}

void Player::draw(int width, int height)
{
    if (session_ == nullptr)
        return;
    if (program_ == 0 && !create_program())
        return;

    // The newest picture whose time has come; older ones that were never shown are skipped.
    AVFrame *due = nullptr;
    {
        Session &session = *session_;
        std::lock_guard lock{session.mutex};
        const double clock = session.clock();
        // After a seek (and at the start) the first picture is shown as soon as it is
        // there, whatever the clock says, so a paused video shows where it is.
        const bool first = shown_serial_ != session.serial;
        while (!session.pictures.empty())
        {
            Picture &next = session.pictures.front();
            if (next.serial == session.serial && !(next.pts <= clock + 0.004 || (first && due == nullptr)))
                break;
            if (due != nullptr)
            {
                av_frame_free(&due);
                ++skipped_;
            }
            if (next.serial == session.serial)
                due = next.frame;
            else
                av_frame_free(&next.frame);
            session.pictures.pop_front();
        }
        if (due != nullptr)
        {
            shown_serial_ = session.serial;
            session.wake.notify_all();
        }
    }
    if (due != nullptr)
    {
        const double before = now_seconds();
        if (upload(due))
            has_picture_ = true;
        else if (!has_picture_)
            session_->fail(std::string{"Pictures in the "} +
                           av_get_pix_fmt_name(static_cast<AVPixelFormat>(due->format)) +
                           " layout are not supported yet.");
        av_frame_free(&due);
        const double after = now_seconds();
        if (after - before > 0.008 && after - slow_logged_ > 2.0)
        {
            slow_logged_ = after;
            note("picture upload took %.1f ms (%lu pictures skipped so far)",
                 (after - before) * 1e3, skipped_);
        }
    }
    if (!has_picture_)
        return;

    // Fitted to the screen with its own shape kept: bars above and below, or at the sides.
    const float screen_aspect = static_cast<float>(width) / static_cast<float>(height);
    int shown_width = width, shown_height = height;
    if (picture_aspect_ > screen_aspect)
        shown_height = static_cast<int>(static_cast<float>(width) / picture_aspect_ + 0.5f);
    else
        shown_width = static_cast<int>(static_cast<float>(height) * picture_aspect_ + 0.5f);
    glViewport((width - shown_width) / 2, (height - shown_height) / 2, shown_width, shown_height);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(program_);
    for (int plane = 2; plane >= 0; --plane)
    {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(plane));
        glBindTexture(GL_TEXTURE_2D, planes_[plane]);
    }
    glBindVertexArray(vertex_array_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    for (int plane = 2; plane >= 0; --plane)
    {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(plane));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glUseProgram(0);
    glViewport(0, 0, width, height);
}
} // namespace ps5
