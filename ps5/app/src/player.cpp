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

#include "languages.hpp"

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
// How much is read ahead of what is playing. Reading ahead also fills the stream's cache,
// which is what makes a short skip forward immediate.
constexpr std::size_t kPacketsAhead = 1440;
constexpr std::size_t kBytesAhead = std::size_t{192} << 20;
constexpr int kIoBuffer = 1 << 19;
constexpr int kPreviewWidth = 384;

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

// A video file read from a web address through FFmpeg's demuxers.
struct Input
{
    std::atomic<bool> *stop = nullptr;
    HttpStream *http = nullptr;
    std::int64_t position = 0;
    AVIOContext *io = nullptr;
    AVFormatContext *format = nullptr;

    ~Input()
    {
        if (format != nullptr)
            avformat_close_input(&format);
        if (io != nullptr)
        {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        stremio_http_close(http);
    }

    static int read(void *opaque, std::uint8_t *buffer, int size)
    {
        auto *self = static_cast<Input *>(opaque);
        if (*self->stop)
            return AVERROR_EXIT;
        const std::int64_t count =
            stremio_http_read(self->http, buffer, static_cast<std::size_t>(size));
        if (count < 0)
            return AVERROR(EIO);
        if (count == 0)
            return AVERROR_EOF;
        self->position += count;
        return static_cast<int>(count);
    }

    static std::int64_t seek(void *opaque, std::int64_t offset, int whence)
    {
        auto *self = static_cast<Input *>(opaque);
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

    // Opens the address and reads what streams the file has. On failure `why` says what
    // went wrong.
    bool open(const std::string &url, std::atomic<bool> &stop_flag, std::string &why)
    {
        stop = &stop_flag;
        http = stremio_http_open(url.c_str());
        if (http == nullptr)
        {
            why = "The stream's address did not answer.";
            return false;
        }
        auto *buffer = static_cast<std::uint8_t *>(av_malloc(kIoBuffer));
        io = avio_alloc_context(buffer, kIoBuffer, 0, this, read, nullptr, seek);
        format = avformat_alloc_context();
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        format->interrupt_callback = {
            [](void *opaque) -> int { return *static_cast<Input *>(opaque)->stop ? 1 : 0; }, this};
        int result = avformat_open_input(&format, nullptr, nullptr, nullptr);
        if (result < 0)
        {
            format = nullptr; // freed by the failed call
            why = "The file is not a video this app can read (" + ffmpeg_error(result) + ").";
            return false;
        }
        result = avformat_find_stream_info(format, nullptr);
        if (result < 0)
        {
            why = "The video's streams could not be read (" + ffmpeg_error(result) + ").";
            return false;
        }
        return true;
    }

    AVCodecContext *decoder(int index, int threads) const
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

    double origin() const
    {
        return format->start_time != AV_NOPTS_VALUE
                   ? static_cast<double>(format->start_time) / AV_TIME_BASE
                   : 0.0;
    }
};

std::string tag(const AVStream *stream, const char *key)
{
    const AVDictionaryEntry *entry = av_dict_get(stream->metadata, key, nullptr, 0);
    return entry != nullptr && entry->value != nullptr ? entry->value : "";
}

// "Dolby Digital+ 5.1", and the track's own title when it has one.
TrackInfo describe_audio(const AVStream *stream)
{
    const AVCodecParameters *parameters = stream->codecpar;
    const char *format = avcodec_get_name(parameters->codec_id);
    switch (parameters->codec_id)
    {
    case AV_CODEC_ID_AC3:
        format = "Dolby Digital";
        break;
    case AV_CODEC_ID_EAC3:
        format = "Dolby Digital+";
        break;
    case AV_CODEC_ID_TRUEHD:
        format = "Dolby TrueHD";
        break;
    case AV_CODEC_ID_DTS:
        format = "DTS";
        break;
    case AV_CODEC_ID_AAC:
        format = "AAC";
        break;
    case AV_CODEC_ID_OPUS:
        format = "Opus";
        break;
    case AV_CODEC_ID_FLAC:
        format = "FLAC";
        break;
    case AV_CODEC_ID_MP3:
        format = "MP3";
        break;
    default:
        break;
    }
    const int channels = parameters->ch_layout.nb_channels;
    const char *layout = channels == 1   ? "mono"
                         : channels == 2 ? "stereo"
                         : channels == 6 ? "5.1"
                         : channels == 8 ? "7.1"
                                         : "";
    TrackInfo info;
    info.language = tag(stream, "language");
    info.detail = std::string{format} + (*layout != 0 ? std::string{" "} + layout : "");
    if (const std::string title = tag(stream, "title"); !title.empty())
        info.detail += "  \xC2\xB7  " + title;
    return info;
}

// The text of a decoded subtitle line. FFmpeg hands text subtitles over as an ASS event:
// nine comma-separated fields with the text last, styled with {...} codes.
std::string plain_text(const char *ass)
{
    const char *text = ass;
    for (int commas = 0; commas < 8 && text != nullptr; ++commas)
    {
        text = std::strchr(text, ',');
        if (text != nullptr)
            ++text;
    }
    if (text == nullptr)
        return {};
    std::string plain;
    for (const char *at = text; *at != 0; ++at)
    {
        if (*at == '{')
        {
            const char *close = std::strchr(at, '}');
            if (close == nullptr)
                break;
            at = close;
        }
        else if (*at == '\\' && (at[1] == 'N' || at[1] == 'n'))
        {
            plain += '\n';
            ++at;
        }
        else if (*at == '\\' && at[1] == 'h')
        {
            plain += ' ';
            ++at;
        }
        else if (*at != '\r')
        {
            plain += *at;
        }
    }
    while (!plain.empty() && (plain.back() == '\n' || plain.back() == ' '))
        plain.pop_back();
    return plain;
}

// PQ to light and back, and the BT.2390 roll-off of highlights down to SDR white: the same
// arithmetic the picture's shader does, for the small scrubbing pictures made on the CPU.
constexpr float kM1 = 0.1593017578125f, kM2 = 78.84375f;
constexpr float kC1 = 0.8359375f, kC2 = 18.8515625f, kC3 = 18.6875f;
constexpr float kWhite = 203.0f;

float pq_to_nits(float signal)
{
    const float p = std::pow(std::max(signal, 0.0f), 1.0f / kM2);
    return 10000.0f * std::pow(std::max(p - kC1, 0.0f) / (kC2 - kC3 * p), 1.0f / kM1);
}

float nits_to_pq(float nits)
{
    const float y = std::pow(std::max(nits, 0.0f) / 10000.0f, kM1);
    return std::pow((kC1 + kC2 * y) / (1.0f + kC3 * y), kM2);
}

// A PQ signal value (0..1) as an SDR one.
float pq_to_sdr(float signal)
{
    const float source = nits_to_pq(1000.0f);
    const float limit = nits_to_pq(kWhite) / source;
    const float knee = 1.5f * limit - 0.5f;
    float e = std::min(signal / source, 1.0f);
    if (e > knee)
    {
        const float t = (e - knee) / (1.0f - knee), t2 = t * t, t3 = t2 * t;
        e = (2 * t3 - 3 * t2 + 1) * knee + (t3 - 2 * t2 + t) * (1 - knee) + (-2 * t3 + 3 * t2) * limit;
    }
    return std::pow(std::clamp(pq_to_nits(e * source) / kWhite, 0.0f, 1.0f), 1.0f / 2.4f);
}

// The colour arithmetic a picture needs, from what the video says about itself.
struct Colours
{
    float luma[2], chroma[2]; // scale and offset, for samples as 0..1 of the stored type
    float kr, kb;
    bool pq, hlg, wide, full;
    float peak;
};

Colours colours_of(const AVFrame *frame, int depth, int bytes)
{
    Colours colours{};
    const auto format = static_cast<AVPixelFormat>(frame->format);
    colours.pq = frame->color_trc == AVCOL_TRC_SMPTE2084;
    colours.hlg = frame->color_trc == AVCOL_TRC_ARIB_STD_B67;
    colours.wide = frame->color_primaries == AVCOL_PRI_BT2020 || colours.pq || colours.hlg;
    colours.kr = 0.2126f, colours.kb = 0.0722f; // BT.709
    switch (frame->colorspace)
    {
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        colours.kr = 0.2627f, colours.kb = 0.0593f;
        break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        colours.kr = 0.299f, colours.kb = 0.114f;
        break;
    case AVCOL_SPC_BT709:
        break;
    default:
        // Not said: HDR is BT.2020, small pictures are BT.601, the rest BT.709.
        if (colours.wide)
            colours.kr = 0.2627f, colours.kb = 0.0593f;
        else if (frame->height < 720)
            colours.kr = 0.299f, colours.kb = 0.114f;
        break;
    }
    colours.full = frame->color_range == AVCOL_RANGE_JPEG || format == AV_PIX_FMT_YUVJ420P ||
                   format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P;
    // A sample is the stored number over the stored type's largest; the video's numbers
    // only use `depth` bits of it.
    const float largest = bytes == 2 ? 65535.0f : 255.0f;
    const float step = static_cast<float>(1 << (depth - 8));
    const float top = static_cast<float>((1 << depth) - 1);
    if (colours.full)
    {
        colours.luma[0] = largest / top, colours.luma[1] = 0.0f;
        colours.chroma[0] = largest / top, colours.chroma[1] = -128.0f * step / top;
    }
    else
    {
        colours.luma[0] = largest / (219.0f * step), colours.luma[1] = -16.0f / 219.0f;
        colours.chroma[0] = largest / (224.0f * step), colours.chroma[1] = -128.0f / 224.0f;
    }
    colours.peak = 1000.0f;
    if (const AVFrameSideData *side =
            av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA))
    {
        const auto *mastering = reinterpret_cast<const AVMasteringDisplayMetadata *>(side->data);
        if (mastering->has_luminance)
            colours.peak = static_cast<float>(av_q2d(mastering->max_luminance));
    }
    colours.peak = std::clamp(colours.peak, 400.0f, 4000.0f);
    return colours;
}

// Whether a picture is laid out as three planes of 8 to 16 bit samples, which is what the
// software decoders produce and all this player draws.
const AVPixFmtDescriptor *planar_layout(const AVFrame *frame)
{
    const AVPixFmtDescriptor *layout = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    if (layout == nullptr || !(layout->flags & AV_PIX_FMT_FLAG_PLANAR) ||
        (layout->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_BE)) || layout->nb_components < 3)
        return nullptr;
    return layout;
}

// A small RGBA copy of a picture, `width` pixels wide.
bool small_copy(const AVFrame *frame, int width, std::vector<std::uint8_t> &pixels, int &height)
{
    const AVPixFmtDescriptor *layout = planar_layout(frame);
    if (layout == nullptr || frame->width <= 0 || frame->height <= 0)
        return false;
    const int depth = layout->comp[0].depth, bytes = depth > 8 ? 2 : 1;
    const Colours colours = colours_of(frame, depth, bytes);
    const AVRational shape = frame->sample_aspect_ratio;
    const double aspect = static_cast<double>(frame->width) / frame->height *
                          (shape.num > 0 && shape.den > 0 ? av_q2d(shape) : 1.0);
    height = std::max(2, static_cast<int>(width / aspect + 0.5));
    pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    const float largest = bytes == 2 ? 65535.0f : 255.0f;
    const auto sample = [&](int plane, int x, int y) {
        const std::uint8_t *row = frame->data[plane] + static_cast<std::ptrdiff_t>(y) * frame->linesize[plane];
        if (bytes == 2)
        {
            std::uint16_t value;
            std::memcpy(&value, row + x * 2, 2);
            return static_cast<float>(value) / largest;
        }
        return static_cast<float>(row[x]) / largest;
    };
    for (int y = 0; y < height; ++y)
    {
        const int source_y = std::min(frame->height - 1, y * frame->height / height);
        for (int x = 0; x < width; ++x)
        {
            const int source_x = std::min(frame->width - 1, x * frame->width / width);
            const float luma = sample(0, source_x, source_y) * colours.luma[0] + colours.luma[1];
            const int chroma_x = source_x >> layout->log2_chroma_w, chroma_y = source_y >> layout->log2_chroma_h;
            const float cb = sample(1, chroma_x, chroma_y) * colours.chroma[0] + colours.chroma[1];
            const float cr = sample(2, chroma_x, chroma_y) * colours.chroma[0] + colours.chroma[1];
            float rgb[3];
            rgb[0] = luma + 2.0f * (1.0f - colours.kr) * cr;
            rgb[2] = luma + 2.0f * (1.0f - colours.kb) * cb;
            rgb[1] = (luma - colours.kr * rgb[0] - colours.kb * rgb[2]) / (1.0f - colours.kr - colours.kb);
            std::uint8_t *out = pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            for (int channel = 0; channel < 3; ++channel)
            {
                float value = std::clamp(rgb[channel], 0.0f, 1.0f);
                if (colours.pq)
                    value = pq_to_sdr(value);
                out[channel] = static_cast<std::uint8_t>(value * 255.0f + 0.5f);
            }
            out[3] = 255;
        }
    }
    return true;
}
} // namespace

struct Player::Session
{
    std::string url;
    std::string audio_language;
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
    bool has_audio = false;
    // The clock: the time in the video (`clock_pts`) that was reached at `clock_at`.
    double clock_pts = 0, clock_at = 0;
    bool clock_running = false;
    // Tracks: the file's stream number for each choice, what to show for it, the one in
    // use and the one asked for. The reader makes the change.
    std::vector<int> audio_streams, subtitle_streams;
    std::vector<TrackInfo> audio_infos, subtitle_infos;
    int audio_choice = -1, audio_wanted = -1;
    int subtitle_choice = -1, subtitle_wanted = -1;
    // While the reader swaps the audio decoder the audio thread stands aside.
    bool audio_swapping = false, audio_parked = false;
    std::vector<Cue> cues; // the subtitles on hand, in order of their start
    // The scrubbing picture: the time asked for, and the newest one made.
    bool preview_wanted = false;
    double preview_time = 0;
    std::vector<std::uint8_t> preview_pixels;
    int preview_width = 0, preview_height = 0;
    double preview_made_for = 0;
    bool preview_fresh = false;

    // FFmpeg. After opening, each object is used by one thread only.
    Input input;
    AVCodecContext *video = nullptr, *audio = nullptr, *subtitle = nullptr;
    SwrContext *resampler = nullptr;
    int video_index = -1, audio_index = -1, subtitle_index = -1;
    double video_base = 0, audio_base = 0, subtitle_base = 0; // seconds per timestamp unit
    double origin = 0;                                        // the file's first timestamp, in seconds
    std::thread reader, video_thread, audio_thread, preview_thread;
    bool preview_started = false; // guarded by `mutex`

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
        avcodec_free_context(&subtitle);
        swr_free(&resampler);
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

    bool open_file()
    {
        std::string why;
        if (!input.open(url, stop, why))
        {
            fail(why);
            return false;
        }
        AVFormatContext *format = input.format;
        video_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_index < 0)
        {
            fail("The file has no video.");
            return false;
        }
        const AVCodecParameters *parameters = format->streams[video_index]->codecpar;
        video = input.decoder(video_index, kDecodeThreads);
        if (video == nullptr)
        {
            fail(std::string{"Video in the "} + avcodec_get_name(parameters->codec_id) +
                 " format is not supported yet.");
            return false;
        }
        video_base = av_q2d(format->streams[video_index]->time_base);
        origin = input.origin();

        // The tracks: every audio stream there is a decoder for, and every text subtitle.
        const int best_audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, video_index, nullptr, 0);
        std::vector<int> audios, subtitles;
        std::vector<TrackInfo> audio_names, subtitle_names;
        int first_choice = -1, language_choice = -1;
        for (unsigned index = 0; index < format->nb_streams; ++index)
        {
            const AVStream *stream = format->streams[index];
            const AVCodecParameters *kind = stream->codecpar;
            if (avcodec_find_decoder(kind->codec_id) == nullptr)
                continue;
            if (kind->codec_type == AVMEDIA_TYPE_AUDIO)
            {
                if (static_cast<int>(index) == best_audio)
                    first_choice = static_cast<int>(audios.size());
                const TrackInfo info = describe_audio(stream);
                if (language_choice < 0 && !audio_language.empty() &&
                    ui::same_language(info.language, audio_language))
                    language_choice = static_cast<int>(audios.size());
                audios.push_back(static_cast<int>(index));
                audio_names.push_back(info);
            }
            else if (kind->codec_type == AVMEDIA_TYPE_SUBTITLE)
            {
                const AVCodecDescriptor *descriptor = avcodec_descriptor_get(kind->codec_id);
                if (descriptor == nullptr || !(descriptor->props & AV_CODEC_PROP_TEXT_SUB))
                    continue; // picture subtitles are not drawn yet
                TrackInfo info;
                info.language = tag(stream, "language");
                info.detail = tag(stream, "title");
                subtitles.push_back(static_cast<int>(index));
                subtitle_names.push_back(info);
            }
        }
        // The preferred language's track when there is one, else the file's own default.
        int choice = language_choice >= 0 ? language_choice : first_choice;
        if (choice < 0 && !audios.empty())
            choice = 0;
        if (choice >= 0)
        {
            audio_index = audios[static_cast<std::size_t>(choice)];
            audio = input.decoder(audio_index, 1);
            audio_base = av_q2d(format->streams[audio_index]->time_base);
        }
        const double length = format->duration != AV_NOPTS_VALUE
                                  ? static_cast<double>(format->duration) / AV_TIME_BASE
                                  : 0.0;
        note("opened: %s %dx%d %s, %zu audio tracks (playing %d), %zu text subtitles, %.0f s",
             avcodec_get_name(parameters->codec_id), parameters->width, parameters->height,
             av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters->format)), audios.size(),
             choice, subtitles.size(), length);
        std::lock_guard lock{mutex};
        has_audio = audio != nullptr;
        duration = length;
        audio_streams = std::move(audios);
        audio_infos = std::move(audio_names);
        subtitle_streams = std::move(subtitles);
        subtitle_infos = std::move(subtitle_names);
        audio_choice = audio_wanted = audio != nullptr ? choice : -1;
        // A start part-way in (resuming) is a seek made before anything is read; one too
        // near the end starts from the beginning instead.
        if (seek_wanted && length > 0 && seek_to > length - 20.0)
            seek_wanted = false;
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

    // Reader thread: changes the audio track. The audio thread is asked to stand aside
    // while its decoder is replaced, and playback then restarts from where it was.
    void swap_audio(int choice)
    {
        std::unique_lock lock{mutex};
        audio_swapping = true;
        wake.notify_all();
        wake.wait(lock, [&] { return audio_parked || stop; });
        if (stop)
            return;
        const double here = clock();
        lock.unlock();
        avcodec_free_context(&audio);
        swr_free(&resampler);
        audio_index = audio_streams[static_cast<std::size_t>(choice)];
        audio = input.decoder(audio_index, 1);
        audio_base = av_q2d(input.format->streams[audio_index]->time_base);
        lock.lock();
        has_audio = audio != nullptr;
        audio_choice = choice;
        audio_swapping = false;
        if (!seek_wanted)
        {
            seek_wanted = true;
            seek_to = here;
        }
        wake.notify_all();
    }

    // Reader thread: changes which of the file's subtitle tracks is decoded (-1 for none).
    void swap_subtitle(int choice)
    {
        avcodec_free_context(&subtitle);
        subtitle_index = -1;
        if (choice >= 0)
        {
            subtitle_index = subtitle_streams[static_cast<std::size_t>(choice)];
            subtitle = input.decoder(subtitle_index, 1);
            subtitle_base = av_q2d(input.format->streams[subtitle_index]->time_base);
            if (subtitle == nullptr)
                subtitle_index = -1;
        }
        std::lock_guard lock{mutex};
        subtitle_choice = choice;
        if (choice >= 0)
        {
            // The lines for the next few seconds have already been read past; reading
            // again from here picks them up.
            cues.clear();
            if (!seek_wanted)
            {
                seek_wanted = true;
                seek_to = clock();
            }
        }
        wake.notify_all();
    }

    // Reader thread: a packet of the chosen subtitle track becomes a line to show.
    void read_subtitle(AVPacket *packet)
    {
        AVSubtitle decoded{};
        int got = 0;
        if (avcodec_decode_subtitle2(subtitle, &decoded, &got, packet) < 0 || got == 0)
            return;
        std::string text;
        for (unsigned index = 0; index < decoded.num_rects; ++index)
        {
            const AVSubtitleRect *rect = decoded.rects[index];
            const std::string line = rect->ass != nullptr    ? plain_text(rect->ass)
                                     : rect->text != nullptr ? std::string{rect->text}
                                                             : std::string{};
            if (!line.empty())
                text += (text.empty() ? "" : "\n") + line;
        }
        const double start = (packet->pts != AV_NOPTS_VALUE ? packet->pts * subtitle_base - origin : 0.0) +
                             decoded.start_display_time / 1000.0;
        double length = decoded.end_display_time > decoded.start_display_time
                            ? (decoded.end_display_time - decoded.start_display_time) / 1000.0
                            : static_cast<double>(packet->duration) * subtitle_base;
        if (length <= 0)
            length = 4.0;
        avsubtitle_free(&decoded);
        if (text.empty())
            return;
        std::lock_guard lock{mutex};
        const auto place = std::lower_bound(cues.begin(), cues.end(), start,
                                            [](const Cue &cue, double time) { return cue.start < time; });
        if (place != cues.end() && std::abs(place->start - start) < 0.001 && place->text == text)
            return; // read again after a seek
        cues.insert(place, Cue{start, start + length, std::move(text)});
    }

    void run_reader()
    {
        if (open_file())
        {
            video_thread = std::thread{[this] { run_video(); }};
            audio_thread = std::thread{[this] { run_audio(); }};
        }
        AVFormatContext *format = input.format;
        AVPacket *packet = av_packet_alloc();
        while (!stop)
        {
            double target = 0;
            bool seeking = false;
            int new_audio = -2, new_subtitle = -2;
            {
                std::unique_lock lock{mutex};
                if (failed)
                {
                    wake.wait_for(lock, std::chrono::milliseconds(100));
                    continue;
                }
                if (audio_wanted != audio_choice && audio_wanted >= 0)
                    new_audio = audio_wanted;
                else if (subtitle_wanted != subtitle_choice)
                    new_subtitle = subtitle_wanted;
                else if (seek_wanted)
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
            if (new_audio != -2)
            {
                swap_audio(new_audio);
                continue;
            }
            if (new_subtitle != -2)
            {
                swap_subtitle(new_subtitle);
                continue;
            }
            if (seeking)
            {
                const auto stamp = static_cast<std::int64_t>((target + origin) * AV_TIME_BASE);
                const int result = avformat_seek_file(format, -1, INT64_MIN, stamp, stamp, 0);
                if (subtitle != nullptr)
                    avcodec_flush_buffers(subtitle);
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
            if (packet->stream_index == subtitle_index && subtitle != nullptr)
                read_subtitle(packet);
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
        int mine = -1; // the serial being decoded
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
            {
                std::unique_lock lock{mutex};
                if (audio_swapping)
                {
                    // The reader is replacing the decoder.
                    audio_parked = true;
                    wake.notify_all();
                    wake.wait(lock, [&] { return !audio_swapping || stop; });
                    audio_parked = false;
                    mine = -1;
                    last = now_seconds();
                    continue;
                }
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
                // Nothing to decode: the clock runs on its own.
                const bool silent = audio == nullptr || handle < 0 ||
                                    (drained && pcm.size() - played < kAudioGrain * 2);
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

    // The scrubbing pictures: the file opened a second time, read on its own, with a
    // decoder that only does key frames. Each request jumps to the key frame before the
    // time asked for and decodes that one picture.
    void run_preview()
    {
        Input second;
        std::string why;
        if (!second.open(url, stop, why))
        {
            note("scrubbing pictures not available: %s", why.c_str());
            return;
        }
        const int index = av_find_best_stream(second.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        AVCodecContext *decoder = index >= 0 ? second.decoder(index, 1) : nullptr;
        if (decoder == nullptr)
            return;
        decoder->skip_frame = AVDISCARD_NONKEY;
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        const double first = second.origin();
        while (!stop)
        {
            double target;
            {
                std::unique_lock lock{mutex};
                if (!preview_wanted)
                {
                    wake.wait_for(lock, std::chrono::milliseconds(50));
                    continue;
                }
                preview_wanted = false;
                target = preview_time;
            }
            const auto stamp = static_cast<std::int64_t>((target + first) * AV_TIME_BASE);
            if (avformat_seek_file(second.format, -1, INT64_MIN, stamp, stamp, 0) < 0)
                continue;
            avcodec_flush_buffers(decoder);
            bool made = false;
            for (int tries = 0; tries < 400 && !made && !stop; ++tries)
            {
                if (av_read_frame(second.format, packet) < 0)
                    break;
                const bool key = packet->stream_index == index && (packet->flags & AV_PKT_FLAG_KEY);
                if (key && avcodec_send_packet(decoder, packet) >= 0)
                {
                    // The decoder would hold the picture back until later ones arrive;
                    // telling it there are none makes it hand the picture over now.
                    avcodec_send_packet(decoder, nullptr);
                    if (avcodec_receive_frame(decoder, frame) >= 0)
                    {
                        std::vector<std::uint8_t> pixels;
                        int height = 0;
                        if (small_copy(frame, kPreviewWidth, pixels, height))
                        {
                            std::lock_guard lock{mutex};
                            preview_pixels = std::move(pixels);
                            preview_width = kPreviewWidth;
                            preview_height = height;
                            preview_made_for = target;
                            preview_fresh = true;
                        }
                        made = true;
                        av_frame_unref(frame);
                    }
                    avcodec_flush_buffers(decoder);
                }
                av_packet_unref(packet);
            }
        }
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
    }
};

Player::Player() = default;

Player::~Player()
{
    close();
}

void Player::open(const std::string &url, double start, const std::string &audio_language)
{
    close();
    av_log_set_level(AV_LOG_ERROR);
    session_ = std::make_shared<Session>();
    session_->url = url;
    session_->audio_language = audio_language;
    if (start > 1.0)
    {
        session_->seek_wanted = true;
        session_->seek_to = start;
        session_->clock_pts = start;
    }
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
        if (session->preview_thread.joinable())
            session->preview_thread.join();
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
    // The subtitle lines on screen at this moment.
    for (const Cue &cue : session.cues)
    {
        if (cue.start > playback.position)
            break;
        if (playback.position < cue.end)
            playback.subtitle += (playback.subtitle.empty() ? "" : "\n") + cue.text;
    }
    return playback;
}

bool Player::tracks_ready() const
{
    if (session_ == nullptr)
        return false;
    std::lock_guard lock{session_->mutex};
    return session_->opened;
}

std::vector<TrackInfo> Player::audio_tracks() const
{
    if (session_ == nullptr)
        return {};
    std::lock_guard lock{session_->mutex};
    return session_->audio_infos;
}

int Player::audio_track() const
{
    if (session_ == nullptr)
        return -1;
    std::lock_guard lock{session_->mutex};
    return session_->audio_wanted;
}

void Player::set_audio_track(int index)
{
    if (session_ == nullptr)
        return;
    std::lock_guard lock{session_->mutex};
    if (index >= 0 && index < static_cast<int>(session_->audio_streams.size()))
        session_->audio_wanted = index;
    session_->wake.notify_all();
}

std::vector<TrackInfo> Player::subtitle_tracks() const
{
    if (session_ == nullptr)
        return {};
    std::lock_guard lock{session_->mutex};
    return session_->subtitle_infos;
}

void Player::set_subtitle_track(int index)
{
    if (session_ == nullptr)
        return;
    std::lock_guard lock{session_->mutex};
    if (index < static_cast<int>(session_->subtitle_streams.size()))
        session_->subtitle_wanted = std::max(index, -1);
    // Choosing the track already in use reads its lines afresh (they may have been
    // replaced by subtitles from elsewhere).
    if (index >= 0 && session_->subtitle_wanted == session_->subtitle_choice)
        session_->subtitle_choice = -2;
    session_->cues.clear();
    session_->wake.notify_all();
}

void Player::set_external_subtitles(std::vector<Cue> cues)
{
    if (session_ == nullptr)
        return;
    std::sort(cues.begin(), cues.end(), [](const Cue &left, const Cue &right) { return left.start < right.start; });
    std::lock_guard lock{session_->mutex};
    session_->subtitle_wanted = -1;
    session_->cues = std::move(cues);
    session_->wake.notify_all();
}

void Player::request_preview(double seconds)
{
    if (session_ == nullptr)
        return;
    Session *session = session_.get();
    std::lock_guard lock{session->mutex};
    if (!session->opened || session->failed)
        return;
    session->preview_wanted = true;
    session->preview_time = seconds;
    if (!session->preview_started)
    {
        session->preview_started = true;
        session->preview_thread = std::thread{[session] { session->run_preview(); }};
    }
    session->wake.notify_all();
}

bool Player::take_preview(std::vector<std::uint8_t> &pixels, int &width, int &height, double &seconds)
{
    if (session_ == nullptr)
        return false;
    std::lock_guard lock{session_->mutex};
    if (!session_->preview_fresh)
        return false;
    session_->preview_fresh = false;
    pixels = std::move(session_->preview_pixels);
    width = session_->preview_width;
    height = session_->preview_height;
    seconds = session_->preview_made_for;
    return true;
}

bool Player::create_programs()
{
    static const char kVertex[] = R"(#version 330 core
out vec2 uv;
void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = vec2(corner.x, 1.0 - corner.y);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";
    // The planes as ordinary textures, which the card filters itself.
    static const char kFromTextures[] = R"(#version 330 core
uniform sampler2D plane_y;
uniform sampler2D plane_u;
uniform sampler2D plane_v;
#define SAMPLE_Y texture(plane_y, uv).r
#define SAMPLE_U texture(plane_u, uv).r
#define SAMPLE_V texture(plane_v, uv).r
)";
    // The planes as plain buffers of samples: each is read at the four samples round the
    // point wanted and blended, which is what a filtered texture read does.
    static const char kFromBuffers[] = R"(#version 330 core
uniform samplerBuffer plane_y;
uniform samplerBuffer plane_u;
uniform samplerBuffer plane_v;
uniform ivec2 size_y;
uniform ivec2 size_c;
float tap(samplerBuffer plane, ivec2 size, ivec2 at) {
    at = clamp(at, ivec2(0), size - 1);
    return texelFetch(plane, at.y * size.x + at.x).r;
}
float blend(samplerBuffer plane, ivec2 size, vec2 where) {
    vec2 at = where * vec2(size) - 0.5;
    vec2 base = floor(at);
    vec2 part = at - base;
    ivec2 corner = ivec2(base);
    return mix(mix(tap(plane, size, corner), tap(plane, size, corner + ivec2(1, 0)), part.x),
               mix(tap(plane, size, corner + ivec2(0, 1)), tap(plane, size, corner + ivec2(1, 1)), part.x),
               part.y);
}
#define SAMPLE_Y blend(plane_y, size_y, uv)
#define SAMPLE_U blend(plane_u, size_c, uv)
#define SAMPLE_V blend(plane_v, size_c, uv)
)";
    // Y'CbCr to R'G'B', and for HDR video on to SDR: to light (PQ or HLG), to the BT.709
    // primaries, the brightest channel brought under SDR white along the BT.2390 curve
    // (dark and mid tones are left alone, highlights are rolled off), and back to a
    // gamma signal.
    static const char kColour[] = R"(
in vec2 uv;
out vec4 color;
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
    float y = SAMPLE_Y * luma.x + luma.y;
    float cb = SAMPLE_U * chroma.x + chroma.y;
    float cr = SAMPLE_V * chroma.x + chroma.y;
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
    const auto compile = [](GLenum type, const char *first, const char *second) {
        const GLuint shader = glCreateShader(type);
        const char *sources[2] = {first, second};
        glShaderSource(shader, second != nullptr ? 2 : 1, sources, nullptr);
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
    const auto link = [&](const char *sampling) -> GLuint {
        const GLuint vertex = compile(GL_VERTEX_SHADER, kVertex, nullptr);
        const GLuint fragment = compile(GL_FRAGMENT_SHADER, sampling, kColour);
        const GLuint program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE)
        {
            glDeleteProgram(program);
            return 0;
        }
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "plane_y"), 0);
        glUniform1i(glGetUniformLocation(program, "plane_u"), 1);
        glUniform1i(glGetUniformLocation(program, "plane_v"), 2);
        glUseProgram(0);
        return program;
    };
    texture_program_ = link(kFromTextures);
    if (texture_program_ == 0)
    {
        note("picture program did not link");
        return false;
    }
    buffer_program_ = link(kFromBuffers);
    if (buffer_program_ == 0)
    {
        note("buffer picture program did not link; pictures go as textures");
        route_[0] = route_[1] = Route::Textures;
    }
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

void Player::set_colours(unsigned program, const void *picture)
{
    if (program == 0)
        return;
    const auto *frame = static_cast<const AVFrame *>(picture);
    const AVPixFmtDescriptor *layout = planar_layout(frame);
    const Colours colours = colours_of(frame, layout->comp[0].depth, bytes_);
    glUseProgram(program);
    glUniform2f(glGetUniformLocation(program, "luma"), colours.luma[0], colours.luma[1]);
    glUniform2f(glGetUniformLocation(program, "chroma"), colours.chroma[0], colours.chroma[1]);
    glUniform2f(glGetUniformLocation(program, "weights"), colours.kr, colours.kb);
    glUniform1i(glGetUniformLocation(program, "transfer"), colours.pq ? 1 : colours.hlg ? 2 : 0);
    glUniform1i(glGetUniformLocation(program, "wide"), colours.wide ? 1 : 0);
    glUniform1f(glGetUniformLocation(program, "peak"), colours.peak);
    if (program == buffer_program_)
    {
        glUniform2i(glGetUniformLocation(program, "size_y"), plane_width_[0], plane_height_[0]);
        glUniform2i(glGetUniformLocation(program, "size_c"), plane_width_[1], plane_height_[1]);
    }
    glUseProgram(0);
}

// Takes note of a picture's layout. When it differs from the last one's (a new video),
// the textures and buffers are made afresh and the programs told its colours. False for
// a layout this does not handle.
bool Player::describe(const void *picture)
{
    const auto *frame = static_cast<const AVFrame *>(picture);
    const AVPixFmtDescriptor *layout = planar_layout(frame);
    if (layout == nullptr)
        return false;
    if (format_ == frame->format && width_ == frame->width && height_ == frame->height)
        return true;
    format_ = frame->format;
    width_ = frame->width;
    height_ = frame->height;
    bytes_ = layout->comp[0].depth > 8 ? 2 : 1;
    for (int plane = 0; plane < 3; ++plane)
    {
        plane_width_[plane] = plane == 0 ? frame->width : AV_CEIL_RSHIFT(frame->width, layout->log2_chroma_w);
        plane_height_[plane] = plane == 0 ? frame->height : AV_CEIL_RSHIFT(frame->height, layout->log2_chroma_h);
    }
    textures_sized_ = false;
    buffers_sized_ = false;
    set_colours(texture_program_, frame);
    set_colours(buffer_program_, frame);
    const AVRational shape = frame->sample_aspect_ratio;
    picture_aspect_ = static_cast<float>(frame->width) / static_cast<float>(frame->height) *
                      (shape.num > 0 && shape.den > 0 ? static_cast<float>(av_q2d(shape)) : 1.0f);
    note("picture: %dx%d %s, transfer %d, range %d, gl error 0x%x", frame->width, frame->height,
         av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)), frame->color_trc,
         frame->color_range, glGetError());
    return true;
}

// The planes always go up as 16-bit textures. This console's driver takes some 40 ms over
// any 8-bit one, whatever its size (which no video's frame rate survives), and a few
// milliseconds over a 16-bit one, so 8-bit video is widened first. A sample v becomes
// v * 257, which is the same fraction of the 16-bit range as v is of the 8-bit one, so the
// colour arithmetic does not change.
bool Player::upload_textures(const void *picture)
{
    const auto *frame = static_cast<const AVFrame *>(picture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int plane = 0; plane < 3; ++plane)
    {
        const int width = plane_width_[plane], height = plane_height_[plane];
        const void *samples = frame->data[plane];
        if (bytes_ == 1)
        {
            widened_.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
            std::uint16_t *to = widened_.data();
            const std::uint8_t *from = frame->data[plane];
            for (int y = 0; y < height; ++y, to += width, from += frame->linesize[plane])
                for (int x = 0; x < width; ++x)
                    to[x] = static_cast<std::uint16_t>(from[x] * 257);
            samples = widened_.data();
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
        else
        {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[plane] / 2);
        }
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(plane));
        glBindTexture(GL_TEXTURE_2D, planes_[plane]);
        if (!textures_sized_)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R16, width, height, 0, GL_RED, GL_UNSIGNED_SHORT, samples);
        else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_SHORT, samples);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glActiveTexture(GL_TEXTURE0);
    textures_sized_ = true;
    return true;
}

// Makes the buffer route's storage for the current layout: for each slot one buffer the
// app can write to at any time, and a buffer texture per plane looking into it.
bool Player::create_buffers()
{
    for (Slot &slot : slots_)
    {
        if (slot.buffer != 0)
        {
            glBindBuffer(GL_TEXTURE_BUFFER, slot.buffer);
            glUnmapBuffer(GL_TEXTURE_BUFFER);
            glDeleteBuffers(1, &slot.buffer);
            glDeleteTextures(3, slot.textures);
        }
        slot = Slot{};
    }
    GLint alignment = 1, most = 0;
    glGetIntegerv(GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT, &alignment);
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &most);
    alignment = std::max(alignment, 64);
    std::size_t total = 0, sizes[3];
    for (int plane = 0; plane < 3; ++plane)
    {
        plane_offset_[plane] = total;
        sizes[plane] = static_cast<std::size_t>(plane_width_[plane]) * plane_height_[plane] * bytes_;
        total += (sizes[plane] + alignment - 1) / alignment * alignment;
    }
    if (static_cast<std::size_t>(most) < static_cast<std::size_t>(plane_width_[0]) * plane_height_[0])
    {
        note("buffer textures are too small here (%d)", most);
        return false;
    }
    while (glGetError() != GL_NO_ERROR)
    {
    }
    const GLbitfield access = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    for (Slot &slot : slots_)
    {
        glGenBuffers(1, &slot.buffer);
        glBindBuffer(GL_TEXTURE_BUFFER, slot.buffer);
        glBufferStorage(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(total), nullptr, access);
        slot.memory = static_cast<unsigned char *>(
            glMapBufferRange(GL_TEXTURE_BUFFER, 0, static_cast<GLsizeiptr>(total), access));
        glGenTextures(3, slot.textures);
        for (int plane = 0; plane < 3; ++plane)
        {
            glBindTexture(GL_TEXTURE_BUFFER, slot.textures[plane]);
            glTexBufferRange(GL_TEXTURE_BUFFER, bytes_ == 2 ? GL_R16 : GL_R8, slot.buffer,
                             static_cast<GLintptr>(plane_offset_[plane]),
                             static_cast<GLsizeiptr>(sizes[plane]));
        }
        glBindTexture(GL_TEXTURE_BUFFER, 0);
        if (slot.memory == nullptr)
        {
            note("buffer storage could not be mapped (gl error 0x%x)", glGetError());
            glBindBuffer(GL_TEXTURE_BUFFER, 0);
            return false;
        }
    }
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR)
    {
        note("buffer route set-up failed (gl error 0x%x)", error);
        return false;
    }
    buffers_sized_ = true;
    return true;
}

void Player::copy_to_buffers(const void *picture)
{
    const auto *frame = static_cast<const AVFrame *>(picture);
    slot_ = (slot_ + 1) % kSlots;
    for (int plane = 0; plane < 3; ++plane)
    {
        unsigned char *to = slots_[slot_].memory + plane_offset_[plane];
        const std::size_t row = static_cast<std::size_t>(plane_width_[plane]) * bytes_;
        const unsigned char *from = frame->data[plane];
        if (static_cast<std::size_t>(frame->linesize[plane]) == row)
        {
            std::memcpy(to, from, row * plane_height_[plane]);
            continue;
        }
        for (int y = 0; y < plane_height_[plane]; ++y, to += row, from += frame->linesize[plane])
            std::memcpy(to, from, row);
    }
}

// Draws the picture last uploaded by one route, fitted to the screen with its own shape
// kept: bars above and below, or at the sides.
void Player::draw_picture(bool buffers, int width, int height)
{
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
    glUseProgram(buffers ? buffer_program_ : texture_program_);
    const GLenum kind = buffers ? GL_TEXTURE_BUFFER : GL_TEXTURE_2D;
    for (int plane = 2; plane >= 0; --plane)
    {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(plane));
        glBindTexture(kind, buffers ? slots_[slot_].textures[plane] : planes_[plane]);
    }
    glBindVertexArray(vertex_array_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    for (int plane = 2; plane >= 0; --plane)
    {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(plane));
        glBindTexture(kind, 0);
    }
    glUseProgram(0);
    glViewport(0, 0, width, height);
}

// Gets a new picture to the graphics card. The first bright picture of each kind (8 bit,
// more than 8 bit) is sent by both routes and what each draws is compared: the buffer
// route is used from then on if it draws the same, since it costs no driver call.
void Player::show(const void *picture, int width, int height)
{
    const auto *frame = static_cast<const AVFrame *>(picture);
    if (!describe(frame))
    {
        if (!has_picture_)
            session_->fail(std::string{"Pictures in the "} +
                           av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)) +
                           " layout are not supported yet.");
        return;
    }
    Route &route = route_[bytes_ - 1];
    // Buffer textures stop at about a million samples on this console, far short of a
    // picture, so the buffer route is only tried where a picture fits.
    if (route != Route::Textures && !buffers_sized_ && !create_buffers())
        route = Route::Textures;
    const double before = now_seconds();
    if (route == Route::Buffers)
    {
        copy_to_buffers(frame);
        shown_from_buffers_ = true;
    }
    else
    {
        upload_textures(frame);
        shown_from_buffers_ = false;
    }
    const double after = now_seconds();
    if (after - before > 0.008 && after - slow_logged_ > 5.0)
    {
        slow_logged_ = after;
        note("sending a picture took %.1f ms by %s (%lu skipped so far)", (after - before) * 1e3,
             route == Route::Buffers ? "buffers" : "textures", skipped_);
    }
    has_picture_ = true;
    if (route != Route::Untested)
        return;

    // The comparison: a small block from the middle of what each route draws.
    constexpr int kBlock = 16;
    unsigned char by_textures[kBlock * kBlock * 4] = {}, by_buffers[kBlock * kBlock * 4] = {};
    draw_picture(false, width, height);
    glReadPixels(width / 2 - kBlock / 2, height / 2 - kBlock / 2, kBlock, kBlock, GL_RGBA,
                 GL_UNSIGNED_BYTE, by_textures);
    long brightness = 0;
    for (int index = 0; index < kBlock * kBlock * 4; index += 4)
        brightness += by_textures[index] + by_textures[index + 1] + by_textures[index + 2];
    if (brightness / (kBlock * kBlock * 3) < 24)
        return; // too dark to tell anything; a later picture decides
    const double copy_start = now_seconds();
    copy_to_buffers(frame);
    const double copy_time = now_seconds() - copy_start;
    draw_picture(true, width, height);
    glReadPixels(width / 2 - kBlock / 2, height / 2 - kBlock / 2, kBlock, kBlock, GL_RGBA,
                 GL_UNSIGNED_BYTE, by_buffers);
    long difference = 0;
    for (int index = 0; index < kBlock * kBlock * 4; ++index)
        difference += std::abs(static_cast<int>(by_textures[index]) - static_cast<int>(by_buffers[index]));
    const long average = difference / (kBlock * kBlock * 4);
    route = average < 10 && glGetError() == GL_NO_ERROR ? Route::Buffers : Route::Textures;
    shown_from_buffers_ = route == Route::Buffers;
    note("%d-byte pictures go by %s (routes differ by %ld; a buffer copy took %.1f ms, the upload %.1f ms)",
         bytes_, route == Route::Buffers ? "buffers" : "textures", average, copy_time * 1e3,
         (after - before) * 1e3);
}

void Player::draw(int width, int height)
{
    if (session_ == nullptr)
        return;
    if (texture_program_ == 0 && !create_programs())
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
        show(due, width, height);
        av_frame_free(&due);
    }
    if (has_picture_)
        draw_picture(shown_from_buffers_, width, height);
}
} // namespace ps5
