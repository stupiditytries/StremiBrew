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

#include <switch.h>

#include <glad/glad.h>

#include "aside.hpp"
#include "languages.hpp"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>

struct HttpStream;
HttpStream *stremio_http_open(const char *url);
HttpStream *stremio_http_open_pieces(const char *url, std::uint64_t piece);
std::int64_t stremio_http_read(HttpStream *stream, std::uint8_t *buffer, std::size_t length);
void stremio_http_seek(HttpStream *stream, std::uint64_t position);
std::int64_t stremio_http_size(const HttpStream *stream);
void stremio_http_close(HttpStream *stream);
void stremio_http_stats(std::uint64_t *out);

}

void log_line(const char *format, ...);

namespace nx
{
// (Finding a picture's black bars is the PS5 app's code, used as it is.)
using ps5::BarSample;
using ps5::decide_bars;
using ps5::measure_bars;

namespace
{
constexpr int kAudioRate = 48000;
constexpr std::size_t kAudioGrain = 512; // frames the audio output is given at a time
// For a video the console's own decoder does not take: the cores an app's threads run on.
constexpr int kDecodeThreads = 3;
// Decoded pictures kept ready. Each 4K picture is some 25 MB, so the queue is short; the
// look-ahead is held as packets instead.
constexpr std::size_t kPictures = 5;
// How much is read ahead of what is playing. Reading ahead also fills the stream's cache,
// which is what makes a short skip forward immediate.
constexpr std::size_t kPacketsAhead = 1440;
constexpr std::size_t kBytesAhead = std::size_t{64} << 20;
constexpr int kIoBuffer = 1 << 19;
// The small pictures shown while scrubbing: their width, and how many seconds of video
// each stands for (more in a long video, so there are never more than kThumbsMost).
constexpr int kThumbWidth = 256;
constexpr double kThumbEvery = 10.0;
constexpr int kThumbsMost = 150;
// How many of them are fetched at once. Each one is a separate request to the server for
// a different part of the file, and it is those requests, not the decoding, that take
// the time; several at once cut it accordingly.
constexpr int kThumbWorkers = 2;
// The workers read the file a piece at a time (see the bridge's stream_io.rs).
constexpr std::uint64_t kThumbPiece = 384 * 1024;
constexpr int kThumbIoBuffer = 128 * 1024;
// Where in a trailer the pictures are taken that decide whether it has black bars (as
// shares of its length): clear of the cards it opens and closes on.
constexpr double kProbePlaces[] = {0.12, 0.25, 0.38, 0.50, 0.62, 0.75};

double now_seconds()
{
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) * 1e-9;
}

void note(const char *format, ...)
{
    char line[400];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    log_line("player: %s", line);
}

// FFmpeg's errors go to the app's log (a limited number of them: a bad stream can produce
// one a picture).
void ffmpeg_says(void *, int level, const char *format, va_list arguments)
{
    static std::atomic<int> lines{0};
    if (level > AV_LOG_ERROR || lines.fetch_add(1) > 150)
        return;
    char text[300];
    std::vsnprintf(text, sizeof text, format, arguments);
    for (char &letter : text)
        if (letter == '\n')
            letter = ' ';
    log_line("player: ffmpeg: %s", text);
}

// The console's audio output, made to look like a pipe that blocks of sound are pushed
// into: write() returns once the console has taken the block, which is what paces the
// thread feeding it. The console takes sound in pieces that it plays in turn; a few are
// with it at any time, and behind() says how far what is being heard is behind what was
// last written.
class AudioOutput
{
  public:
    bool open()
    {
        if (R_FAILED(audoutInitialize()))
            return false;
        if (R_FAILED(audoutStartAudioOut()))
        {
            audoutExit();
            return false;
        }
        for (Piece &piece : pieces_)
        {
            // (The console wants each piece's memory to start and end on a page.)
            piece.buffer.buffer = std::aligned_alloc(0x1000, 0x1000);
            piece.buffer.buffer_size = 0x1000;
            piece.out = false;
        }
        ready_ = true;
        return true;
    }

    bool ready() const
    {
        return ready_;
    }

    // One block: kAudioGrain samples for each of two channels.
    void write(const std::int16_t *block)
    {
        if (!ready_)
            return;
        collect(0);
        Piece *free = nullptr;
        for (int attempt = 0; attempt < 10 && free == nullptr; ++attempt)
        {
            for (Piece &piece : pieces_)
                if (!piece.out && free == nullptr)
                    free = &piece;
            if (free == nullptr)
                collect(100'000'000ull);
        }
        if (free == nullptr)
        {
            // The console has stopped taking sound: the block is let go rather than the
            // video brought to a stop behind it.
            std::this_thread::sleep_for(std::chrono::microseconds(kAudioGrain * 1'000'000 / kAudioRate));
            return;
        }
        std::memcpy(free->buffer.buffer, block, kAudioGrain * 2 * sizeof(std::int16_t));
        free->buffer.data_size = kAudioGrain * 2 * sizeof(std::int16_t);
        free->buffer.data_offset = 0;
        if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&free->buffer)))
        {
            free->out = true;
            ++out_;
        }
    }

    // Seconds between the start of the block last written and what is being heard.
    double behind() const
    {
        return out_ > 1 ? static_cast<double>(out_ - 1) * static_cast<double>(kAudioGrain) / kAudioRate : 0.0;
    }

    void close()
    {
        if (!ready_)
            return;
        audoutStopAudioOut();
        audoutExit();
        for (Piece &piece : pieces_)
            std::free(piece.buffer.buffer);
        ready_ = false;
    }

  private:
    struct Piece
    {
        AudioOutBuffer buffer{};
        bool out = false; // with the console, not yet played
    };

    // Takes back the pieces the console has finished playing, waiting up to `patience`
    // nanoseconds for one when given any.
    void collect(u64 patience)
    {
        AudioOutBuffer *released = nullptr;
        u32 count = 0;
        Result result = patience != 0 ? audoutWaitPlayFinish(&released, &count, patience)
                                      : audoutGetReleasedAudioOutBuffer(&released, &count);
        while (R_SUCCEEDED(result) && count > 0 && released != nullptr)
        {
            for (Piece &piece : pieces_)
            {
                if (&piece.buffer == released && piece.out)
                {
                    piece.out = false;
                    --out_;
                }
            }
            released = nullptr;
            count = 0;
            result = audoutGetReleasedAudioOutBuffer(&released, &count);
        }
    }

    static constexpr int kPieces = 6;
    Piece pieces_[kPieces];
    int out_ = 0;
    bool ready_ = false;
};

// Whether the console's video decoder can be put to a kind of video at all.
bool hardware_offered(const AVCodec *codec)
{
    for (int index = 0;; ++index)
    {
        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, index);
        if (config == nullptr)
            return false;
        if (config->device_type == AV_HWDEVICE_TYPE_NVTEGRA &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            return true;
    }
}

// ... and to this video in particular: it decodes 4:2:0 pictures, of 8 bits (or, for HEVC
// alone, 10). Anything else of a kind it knows would be refused later, after the choice
// of a software decoder with several threads can no longer be made.
bool hardware_takes(const AVCodecParameters *video)
{
    const AVPixFmtDescriptor *layout = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(video->format));
    if (layout == nullptr)
        return true; // not said; the decoder finds out
    if (layout->log2_chroma_w != 1 || layout->log2_chroma_h != 1)
        return false;
    const int depth = layout->comp[0].depth;
    return depth == 8 || (depth == 10 && video->codec_id == AV_CODEC_ID_HEVC);
}

// The decoder is offered the formats it can produce; the console's decoder's is taken
// when it is among them.
AVPixelFormat hardware_format(AVCodecContext *, const AVPixelFormat *offered)
{
    for (const AVPixelFormat *format = offered; *format != AV_PIX_FMT_NONE; ++format)
        if (*format == AV_PIX_FMT_NVTEGRA)
            return *format;
    note("the console's decoder would not take this video after all; decoding in software, on one thread");
    return offered[0];
}

// A copy, in ordinary memory, of a picture the console's decoder made (which is in the
// decoder's own). Null when it cannot be had.
AVFrame *plain_copy(const AVFrame *decoded)
{
    AVFrame *plain = av_frame_alloc();
    if (plain == nullptr)
        return nullptr;
    if (decoded->hw_frames_ctx != nullptr)
    {
        // Laid out the way that lets the console's video hardware make the copy itself:
        // lines that begin 256 bytes apart.
        const auto *frames = reinterpret_cast<const AVHWFramesContext *>(decoded->hw_frames_ctx->data);
        plain->format = frames->sw_format;
        plain->width = decoded->width;
        plain->height = decoded->height;
        if (av_frame_get_buffer(plain, 256) < 0)
        {
            av_frame_free(&plain);
            return nullptr;
        }
    }
    if (av_hwframe_transfer_data(plain, decoded, 0) < 0)
    {
        // Not into memory laid out here, then: into whatever FFmpeg itself provides.
        av_frame_unref(plain);
        if (av_hwframe_transfer_data(plain, decoded, 0) < 0)
        {
            av_frame_free(&plain);
            return nullptr;
        }
    }
    if (av_frame_copy_props(plain, decoded) < 0)
    {
        av_frame_free(&plain);
        return nullptr;
    }
    return plain;
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
    // went wrong. `browsing` is for a reader that only jumps about the file for single
    // pictures: it reads in pieces, and skips the probing of the streams that playing
    // needs (the file's header says enough to decode a picture).
    bool open(const std::string &url, std::atomic<bool> &stop_flag, std::string &why,
              bool browsing = false)
    {
        stop = &stop_flag;
        http = browsing ? stremio_http_open_pieces(url.c_str(), kThumbPiece)
                        : stremio_http_open(url.c_str());
        if (http == nullptr)
        {
            why = "The stream's address did not answer.";
            return false;
        }
        const int buffer_size = browsing ? kThumbIoBuffer : kIoBuffer;
        auto *buffer = static_cast<std::uint8_t *>(av_malloc(buffer_size));
        io = avio_alloc_context(buffer, buffer_size, 0, this, read, nullptr, seek);
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
        result = browsing ? 0 : avformat_find_stream_info(format, nullptr);
        if (result < 0)
        {
            why = "The video's streams could not be read (" + ffmpeg_error(result) + ").";
            return false;
        }
        return true;
    }

    AVCodecContext *decoder(int index, int threads, AVBufferRef *device = nullptr) const
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
        if (device != nullptr)
        {
            // The console's own video decoder does the work; this one hands it the video.
            context->hw_device_ctx = av_buffer_ref(device);
            context->get_format = hardware_format;
        }
        else if (threads > 1)
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
    bool preview = false; // a trailer (see Player::Options)
    float volume = 1.0f;
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
    // Running dry is not ended by the first scrap to arrive: playback then waits until
    // `refill_wanted` seconds have been read (more each time it runs dry again soon
    // after), so a connection slower than the video gives stretches of play with waits
    // between them rather than a stutter.
    bool refilling = false;
    double refill_wanted = 0, refill_began = 0, flowed_from = 0;
    int dry_runs = 0;
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
    double subtitle_delay = 0;
    // A seek being timed for the log: when it was asked for, when the reader took it up,
    // when the demuxer had moved, and the stream's counters at that point.
    bool seek_traced = false;
    double seek_asked = 0, seek_taken = 0, seek_moved = 0, seek_target = 0;
    std::uint64_t seek_stats[4] = {};
    // A copy being taken of the sound as it plays (see Player::capture_audio).
    bool capturing = false, capture_failed = false, capture_complete = false;
    std::size_t capture_wanted = 0;
    std::vector<float> captured;
    double capture_start = 0;
    int capture_serial = 0;
    float capture_sum = 0;
    int capture_count = 0;
    // The scrubbing pictures: one for each `thumb_every` seconds of the video, made in
    // the background from when the video opens (an empty one is not made yet).
    std::vector<std::vector<std::uint8_t>> thumbs;
    double thumb_every = 0;
    int thumb_width = 0, thumb_height = 0;

    // FFmpeg. After opening, each object is used by one thread only.
    Input input;
    AVCodecContext *video = nullptr, *audio = nullptr, *subtitle = nullptr;
    AVBufferRef *device = nullptr; // the console's video decoder, when it is the one decoding
    SwrContext *resampler = nullptr;
    int video_index = -1, audio_index = -1, subtitle_index = -1;
    double video_base = 0, audio_base = 0, subtitle_base = 0; // seconds per timestamp unit
    double origin = 0;                                        // the file's first timestamp, in seconds
    std::thread reader, video_thread, audio_thread;
    std::vector<std::thread> preview_threads;
    // A trailer's black bars, decided before it is shown (see run_probe).
    bool bars_decided = false;
    float bars_top = 0, bars_bottom = 0;
    std::vector<int> thumb_order;     // which picture to make first, second, ...
    std::atomic<int> thumb_next{0};   // the place in that order reached so far
    std::atomic<int> thumbs_made{0};

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
        av_buffer_unref(&device);
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
        // The console's own decoder when it takes this video; a software one otherwise.
        if (const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
            codec != nullptr && hardware_offered(codec) && hardware_takes(parameters))
        {
            const int made = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0);
            if (made >= 0)
                video = input.decoder(video_index, 1, device);
            if (video == nullptr)
            {
                note("the console's decoder did not open (%s); decoding in software", ffmpeg_error(made).c_str());
                av_buffer_unref(&device);
            }
        }
        if (video == nullptr)
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
        note("opened: %s %dx%d %s at %.2f pictures a second, %zu audio tracks (playing %d), %zu text subtitles, %.0f s",
             avcodec_get_name(parameters->codec_id), parameters->width, parameters->height,
             av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters->format)),
             av_q2d(format->streams[video_index]->avg_frame_rate), audios.size(), choice,
             subtitles.size(), length);
        note("decoding %s", device != nullptr ? "on the console's decoder" : "in software");
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

    // With `mutex` held: how many seconds of the video have been read and are waiting to
    // be decoded (of picture or of sound, whichever there is less of).
    double queued_seconds() const
    {
        const auto span = [](const std::deque<PacketItem> &packets, double base) {
            if (packets.size() < 2)
                return 0.0;
            const auto stamp = [](const AVPacket *packet) {
                return packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
            };
            const std::int64_t first = stamp(packets.front().packet), last = stamp(packets.back().packet);
            // (A file that does not time its packets is taken at 25 of them a second.)
            if (first == AV_NOPTS_VALUE || last == AV_NOPTS_VALUE)
                return static_cast<double>(packets.size()) / 25.0;
            return std::max(0.0, static_cast<double>(last - first) * base);
        };
        const double pictures_read = span(video_packets, video_base);
        return has_audio && audio != nullptr ? std::min(pictures_read, span(audio_packets, audio_base))
                                             : pictures_read;
    }

    // Reader thread. A jump forward to a moment that has already been read is made within
    // what is waiting to be decoded: everything before the key picture that moment hangs
    // on is dropped, and decoding starts afresh from there. Nothing is asked of the
    // network, so it is immediate. False when the moment is not within what has been read.
    bool jump_in_queue(double target)
    {
        std::lock_guard lock{mutex};
        if (!seek_wanted || seek_to != target || video_packets.empty() || target <= clock())
            return false;
        const auto time_of = [this](const AVPacket *packet, double base) {
            const std::int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            return stamp != AV_NOPTS_VALUE ? static_cast<double>(stamp) * base - origin : -1.0;
        };
        // The last key picture at or before the moment, and whether what has been read
        // goes on past the moment.
        std::size_t key = video_packets.size();
        bool reaches = false;
        for (std::size_t index = 0; index < video_packets.size(); ++index)
        {
            const AVPacket *packet = video_packets[index].packet;
            const double time = time_of(packet, video_base);
            if (time < 0)
                continue;
            if (time > target + 1.0)
            {
                reaches = true;
                break;
            }
            if ((packet->flags & AV_PKT_FLAG_KEY) && time <= target)
                key = index;
        }
        if (key == video_packets.size() || !(reaches || reader_done))
            return false;
        const double from = time_of(video_packets[key].packet, video_base);
        for (std::size_t index = 0; index < key; ++index)
        {
            queued_bytes -= static_cast<std::size_t>(video_packets[index].packet->size);
            av_packet_free(&video_packets[index].packet);
        }
        video_packets.erase(video_packets.begin(), video_packets.begin() + static_cast<std::ptrdiff_t>(key));
        // The sound from a little before that picture on (the first of it is left out
        // again when it is played, down to the moment itself).
        while (!audio_packets.empty())
        {
            const double time = time_of(audio_packets.front().packet, audio_base);
            if (time < 0 || time >= from - 0.5)
                break;
            queued_bytes -= static_cast<std::size_t>(audio_packets.front().packet->size);
            av_packet_free(&audio_packets.front().packet);
            audio_packets.pop_front();
        }
        // What is left is the new serial's: the decoders start afresh at it.
        ++serial;
        for (PacketItem &item : video_packets)
            item.serial = serial;
        for (PacketItem &item : audio_packets)
            item.serial = serial;
        start_from = target;
        video_done = audio_done = false;
        video_primed = false;
        starved = false;
        refilling = false;
        clock_pts = target;
        clock_running = false;
        seek_wanted = false;
        note("jumped to %.0f s within what had been read (%zu packets of picture and %zu of sound in hand)", target,
             video_packets.size(), audio_packets.size());
        wake.notify_all();
        return true;
    }

    // With `mutex` held: playback has run dry (at `now`); it waits to be refilled.
    void begin_refill(double now)
    {
        dry_runs = now - flowed_from < 45.0 ? dry_runs + 1 : 1;
        refill_wanted = dry_runs <= 1 ? 4.0 : dry_runs == 2 ? 10.0 : 20.0;
        refilling = true;
        refill_began = now;
        note("ran dry at %.0f s; waiting until %.0f s have been read", clock_pts, refill_wanted);
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
            if (preview)
                preview_threads.emplace_back([this] { run_probe(); });
            else
                start_previews();
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
            if (seeking && jump_in_queue(target))
                continue;
            if (seeking)
            {
                const double taken = now_seconds();
                std::uint64_t before[4] = {};
                stremio_http_stats(before);
                const auto stamp = static_cast<std::int64_t>((target + origin) * AV_TIME_BASE);
                const int result = avformat_seek_file(format, -1, INT64_MIN, stamp, stamp, 0);
                const double moved = now_seconds();
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
                seek_traced = true;
                seek_taken = taken;
                seek_moved = moved;
                seek_target = target;
                std::memcpy(seek_stats, before, sizeof before);
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
        for (std::thread &worker : preview_threads)
            if (worker.joinable())
                worker.join();
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
            if (packet != nullptr)
            {
                // After a seek the decoder has to work from the key frame before the
                // target up to it. The pictures on the way are not shown, so the ones no
                // other picture is built from are not decoded at all, which roughly
                // halves the wait in most videos.
                double from;
                {
                    std::lock_guard lock{mutex};
                    from = start_from;
                }
                const std::int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
                const bool early = stamp != AV_NOPTS_VALUE &&
                                   static_cast<double>(stamp) * video_base - origin < from - 0.25;
                video->skip_frame = early ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
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
                // A picture from the console's decoder is in that decoder's memory; what is
                // queued and drawn is a copy in ordinary memory (not made for the pictures
                // a seek passes over on its way).
                AVFrame *plain = nullptr;
                if (frame->format == AV_PIX_FMT_NVTEGRA)
                {
                    bool wanted;
                    {
                        std::lock_guard lock{mutex};
                        wanted = mine == serial && pts >= start_from - 0.02;
                    }
                    plain = wanted ? plain_copy(frame) : nullptr;
                    if (plain == nullptr)
                    {
                        av_frame_unref(frame);
                        continue;
                    }
                }
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
                        pictures.push_back({plain != nullptr ? plain : av_frame_clone(frame), pts, mine});
                        plain = nullptr;
                        video_primed = true;
                        wake.notify_all();
                        if (seek_traced)
                        {
                            // The seek's first picture: where its time went.
                            seek_traced = false;
                            std::uint64_t now_stats[4] = {};
                            stremio_http_stats(now_stats);
                            const double done = now_seconds();
                            const std::uint64_t connections = now_stats[2] - seek_stats[2];
                            note("seek to %.0f s: %.0f ms before the reader took it, %.0f ms moving in the file, "
                                 "%.0f ms to the first picture; %.1f MB from the cache, %.1f MB from the network, "
                                 "%llu connections (%llu ms answering)",
                                 seek_target, (seek_taken - seek_asked) * 1e3, (seek_moved - seek_taken) * 1e3,
                                 (done - seek_moved) * 1e3,
                                 static_cast<double>(now_stats[0] - seek_stats[0]) / (1 << 20),
                                 static_cast<double>(now_stats[1] - seek_stats[1]) / (1 << 20),
                                 static_cast<unsigned long long>(connections),
                                 static_cast<unsigned long long>(now_stats[3] - seek_stats[3]));
                        }
                    }
                }
                lock.unlock();
                av_frame_unref(frame);
                av_frame_free(&plain);
                if (stop)
                    break;
            }
        }
        av_frame_free(&frame);
    }

    void run_audio()
    {
        AudioOutput output;
        if (!output.open())
            note("audio output did not open");

        AVFrame *frame = av_frame_alloc();
        std::vector<std::int16_t> pcm; // stereo, waiting to be played
        std::size_t played = 0;        // samples of `pcm` already played
        double pcm_pts = 0;            // the time of pcm[played]
        int mine = -1;
        bool drained = false;
        bool flowing = false; // sound (or without it the clock) has run since the last seek
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
                    flowing = false;
                    refilling = false;
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
                // After running dry, playback waits for a fair amount to have been read
                // (or for all there is) before it starts again.
                if (refilling)
                {
                    if (reader_done || read_enough() || queued_seconds() >= refill_wanted)
                    {
                        refilling = false;
                        flowed_from = now;
                        note("playing on after %.1f s of waiting", now - refill_began);
                    }
                    else
                    {
                        starved = true;
                        wake.wait_for(lock, std::chrono::milliseconds(20));
                        last = now_seconds();
                        continue;
                    }
                }
                // Nothing to decode: the clock runs on its own.
                const bool silent = audio == nullptr || !output.ready() ||
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
                    if (starved && flowing && video_packets.empty() && !reader_done)
                    {
                        flowing = false;
                        begin_refill(now);
                    }
                    if (!starved)
                    {
                        clock_pts += std::min(elapsed, 0.05);
                        flowing = true;
                    }
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
                        // (Before anything has played, after opening or a seek, this is
                        // only the wait for the first of the sound, not a running dry.)
                        if (flowing)
                        {
                            flowing = false;
                            begin_refill(now);
                        }
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
            output.write(pcm.data() + played);
            {
                std::lock_guard lock{mutex};
                if (capturing)
                {
                    // The block just played joins the copy: its two channels as one, and
                    // every three samples as one (48,000 a second down to 16,000).
                    if (captured.empty() && capture_count == 0)
                    {
                        capture_start = pcm_pts;
                        capture_serial = serial;
                    }
                    if (capture_serial != serial)
                    {
                        capturing = false;
                        capture_failed = true;
                    }
                    const std::int16_t *block = pcm.data() + played;
                    for (std::size_t frame_index = 0; capturing && frame_index < kAudioGrain; ++frame_index)
                    {
                        capture_sum += static_cast<float>(block[frame_index * 2] + block[frame_index * 2 + 1]);
                        if (++capture_count == 3)
                        {
                            captured.push_back(capture_sum / (6.0f * 32768.0f));
                            capture_sum = 0;
                            capture_count = 0;
                            if (captured.size() >= capture_wanted)
                            {
                                capturing = false;
                                capture_complete = true;
                            }
                        }
                    }
                }
                if (mine == serial)
                {
                    // (What is being heard is a little behind what was just handed over.)
                    clock_pts = pcm_pts - output.behind();
                    clock_at = now_seconds();
                    clock_running = true;
                    starved = false;
                    flowing = true;
                }
            }
            played += kAudioGrain * 2;
            pcm_pts += static_cast<double>(kAudioGrain) / kAudioRate;
        }
        av_frame_free(&frame);
        output.close();
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
        if (volume < 0.999f)
            for (std::size_t index = before; index < pcm.size(); ++index)
                pcm[index] = static_cast<std::int16_t>(static_cast<float>(pcm[index]) * volume);
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

    // The scrubbing pictures: one for each stretch of the video, made in the background
    // from when it opens so they are on hand by the time the user scrubs. The order is
    // outwards from where playback starts; several workers go through it at once.
    void start_previews()
    {
        double length, from;
        {
            std::lock_guard lock{mutex};
            length = duration;
            from = seek_wanted ? seek_to : 0.0;
        }
        if (length <= 0)
            return;
        const double every = std::max(kThumbEvery, length / kThumbsMost);
        const int count = std::max(1, static_cast<int>(length / every));
        const int centre = std::clamp(static_cast<int>(from / every), 0, count - 1);
        for (int step = 0; step < 2 * count; ++step)
        {
            const int which = centre + ((step & 1) ? (step + 1) / 2 : -(step / 2));
            if (which >= 0 && which < count)
                thumb_order.push_back(which);
        }
        {
            std::lock_guard lock{mutex};
            thumbs.assign(static_cast<std::size_t>(count), {});
            thumb_every = every;
        }
        for (int worker = 0; worker < kThumbWorkers; ++worker)
            preview_threads.emplace_back([this] { run_preview(); });
    }

    // A trailer's black bars. The file is opened a second time and one key frame is
    // decoded at each of a few places across it; whether most of those pictures have bars
    // decides it, once, before the trailer is shown, so the picture never changes size
    // while it plays. (See black_bars.hpp.)
    void run_probe()
    {
        const auto decided = [&](float top, float bottom) {
            std::lock_guard lock{mutex};
            bars_top = top;
            bars_bottom = bottom;
            bars_decided = true;
        };
        double length;
        {
            std::lock_guard lock{mutex};
            length = duration;
        }
        Input second;
        std::string why;
        if (length <= 0 || !second.open(url, stop, why, true))
            return decided(0, 0);
        const int index = av_find_best_stream(second.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        AVCodecContext *decoder = index >= 0 ? second.decoder(index, 1) : nullptr;
        if (decoder == nullptr)
            return decided(0, 0);
        decoder->skip_frame = AVDISCARD_NONKEY;
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        const double started = now_seconds();
        BarSample samples[std::size(kProbePlaces)];
        int count = 0;
        for (const double place : kProbePlaces)
        {
            if (stop)
                break;
            const auto stamp = static_cast<std::int64_t>((length * place + origin) * AV_TIME_BASE);
            if (avformat_seek_file(second.format, -1, INT64_MIN, stamp, stamp, 0) < 0)
                continue;
            avcodec_flush_buffers(decoder);
            bool done = false;
            for (int tries = 0; tries < 400 && !done && !stop; ++tries)
            {
                if (av_read_frame(second.format, packet) < 0)
                    break;
                const bool key = packet->stream_index == index && (packet->flags & AV_PKT_FLAG_KEY);
                if (key && avcodec_send_packet(decoder, packet) >= 0)
                {
                    avcodec_send_packet(decoder, nullptr);
                    if (avcodec_receive_frame(decoder, frame) >= 0)
                    {
                        const AVPixFmtDescriptor *layout = planar_layout(frame);
                        if (layout != nullptr)
                        {
                            const int depth = layout->comp[0].depth;
                            samples[count++] = measure_bars(frame->data[0], frame->linesize[0], frame->width,
                                                            frame->height, depth > 8 ? 2 : 1, depth,
                                                            frame->color_range != AVCOL_RANGE_JPEG);
                        }
                        av_frame_unref(frame);
                    }
                    avcodec_flush_buffers(decoder);
                    done = true;
                }
                av_packet_unref(packet);
            }
        }
        float top = 0, bottom = 0;
        decide_bars(samples, count, top, bottom);
        note("trailer bars: %.1f%% top, %.1f%% bottom, from %d pictures in %.1f s", top * 100, bottom * 100,
             count, now_seconds() - started);
        decided(top, bottom);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
    }

    // One worker: the file opened again, read on its own, with a decoder that only does
    // key frames. For each picture it takes from the order it jumps to that stretch of the
    // video and makes a small copy of the key frame there.
    void run_preview()
    {
        Input second;
        std::string why;
        if (!second.open(url, stop, why, true))
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
        const double first = origin; // the same file as the one playing
        const double started = now_seconds();
        const int count = static_cast<int>(thumb_order.size());
        while (!stop)
        {
            const int place = thumb_next.fetch_add(1);
            if (place >= count)
                break;
            const int which = thumb_order[static_cast<std::size_t>(place)];
            {
                // Playback comes first: while it is waiting for data, this waits too.
                std::unique_lock lock{mutex};
                while (!stop && (starved || !video_primed) && !paused && !failed)
                    wake.wait_for(lock, std::chrono::milliseconds(100));
            }
            const double target = (which + 0.5) * thumb_every;
            const auto stamp = static_cast<std::int64_t>((target + first) * AV_TIME_BASE);
            if (avformat_seek_file(second.format, -1, INT64_MIN, stamp, stamp, 0) < 0)
                continue;
            avcodec_flush_buffers(decoder);
            bool done = false;
            for (int tries = 0; tries < 400 && !done && !stop; ++tries)
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
                        if (small_copy(frame, kThumbWidth, pixels, height))
                        {
                            std::lock_guard lock{mutex};
                            thumbs[static_cast<std::size_t>(which)] = std::move(pixels);
                            thumb_width = kThumbWidth;
                            thumb_height = height;
                            ++thumbs_made;
                        }
                        av_frame_unref(frame);
                    }
                    avcodec_flush_buffers(decoder);
                    done = true;
                }
                av_packet_unref(packet);
            }
        }
        if (!stop)
            note("scrubbing pictures: %d of %d made, %.0f s after opening", thumbs_made.load(), count,
                 now_seconds() - started);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
    }
};

namespace
{
std::atomic<int> live_sessions{0};
}

int Player::sessions()
{
    return live_sessions;
}

Player::Player() = default;

Player::~Player()
{
    close();
}

void Player::open(const std::string &url, double start, const std::string &audio_language,
                  const Options &options)
{
    close();
    av_log_set_level(AV_LOG_ERROR);
    av_log_set_callback(ffmpeg_says);
    appletSetMediaPlaybackState(true); // the screen stays on while a video plays
    session_ = std::make_shared<Session>();
    session_->url = url;
    session_->audio_language = audio_language;
    session_->preview = options.preview;
    format_ = -1; // the next picture is described afresh, for this video
    session_->volume = options.volume;
    session_->paused = options.paused;
    if (start > 1.0)
    {
        session_->seek_wanted = true;
        session_->seek_to = start;
        session_->clock_pts = start;
    }
    has_picture_ = false;
    shown_serial_ = -1;
    skipped_ = 0;
    ++live_sessions;
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
    appletSetMediaPlaybackState(false);
    // The threads may be inside a network read; they are waited for out of the way.
    run_aside([session] {
        if (session->reader.joinable())
            session->reader.join();
        --live_sessions;
    });
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
    session_->seek_asked = now_seconds();
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
    const double reading = playback.position - session.subtitle_delay;
    for (const Cue &cue : session.cues)
    {
        if (cue.start > reading)
            break;
        if (reading < cue.end)
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

std::vector<Cue> Player::subtitles() const
{
    if (session_ == nullptr)
        return {};
    std::lock_guard lock{session_->mutex};
    return session_->cues;
}

std::string Player::audio_language() const
{
    if (session_ == nullptr)
        return {};
    std::lock_guard lock{session_->mutex};
    const int choice = session_->audio_choice;
    return choice >= 0 && choice < static_cast<int>(session_->audio_infos.size())
               ? session_->audio_infos[static_cast<std::size_t>(choice)].language
               : std::string{};
}

void Player::capture_audio(double seconds)
{
    if (session_ == nullptr)
        return;
    std::lock_guard lock{session_->mutex};
    Session &session = *session_;
    session.captured.clear();
    session.capture_sum = 0;
    session.capture_count = 0;
    session.capture_wanted = static_cast<std::size_t>(seconds * 16000.0);
    session.capture_complete = false;
    // Without sound being played there is nothing to copy.
    session.capture_failed = !session.has_audio || session.paused;
    session.capturing = !session.capture_failed;
}

float Player::capture_state() const
{
    if (session_ == nullptr)
        return -1.0f;
    std::lock_guard lock{session_->mutex};
    const Session &session = *session_;
    if (session.capture_complete)
        return 2.0f;
    if (session.capture_failed || !session.capturing || session.paused)
        return -1.0f;
    return static_cast<float>(session.captured.size()) /
           static_cast<float>(std::max<std::size_t>(1, session.capture_wanted));
}

bool Player::take_capture(std::vector<float> &samples, double &start)
{
    if (session_ == nullptr)
        return false;
    std::lock_guard lock{session_->mutex};
    if (!session_->capture_complete)
        return false;
    session_->capture_complete = false;
    samples = std::move(session_->captured);
    session_->captured.clear();
    start = session_->capture_start;
    return true;
}

void Player::set_subtitle_delay(double seconds)
{
    if (session_ == nullptr)
        return;
    std::lock_guard lock{session_->mutex};
    session_->subtitle_delay = seconds;
}

bool Player::preview(double seconds, std::vector<std::uint8_t> &pixels, int &width, int &height,
                     int &index, double &time)
{
    if (session_ == nullptr)
        return false;
    std::lock_guard lock{session_->mutex};
    const auto &thumbs = session_->thumbs;
    if (thumbs.empty() || session_->thumb_every <= 0)
        return false;
    const int count = static_cast<int>(thumbs.size());
    const int wanted = std::clamp(static_cast<int>(seconds / session_->thumb_every), 0, count - 1);
    // The picture for that stretch of the video, or failing that a neighbour's.
    for (const int offset : {0, -1, 1})
    {
        const int candidate = wanted + offset;
        if (candidate < 0 || candidate >= count || thumbs[static_cast<std::size_t>(candidate)].empty())
            continue;
        time = (candidate + 0.5) * session_->thumb_every;
        width = session_->thumb_width;
        height = session_->thumb_height;
        // The pixels are only copied out when it is a different picture from last time.
        if (candidate != index)
            pixels = thumbs[static_cast<std::size_t>(candidate)];
        else
            pixels.clear();
        index = candidate;
        return true;
    }
    return false;
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
    // The picture's brightness is one texture; its colour is one more (its two samples
    // side by side, as the console's decoder gives them) or two (as software decoders do).
    //
    // Then Y'CbCr to R'G'B', and for HDR video on to SDR: to light (PQ or HLG), to the
    // BT.709 primaries, the brightest channel brought under SDR white along the BT.2390
    // curve (dark and mid tones are left alone, highlights are rolled off), and back to a
    // gamma signal.
    static const char kFragment[] = R"(#version 330 core
in vec2 uv;
out vec4 color;
uniform sampler2D plane_y, plane_u, plane_v;
uniform int paired;   // the colour samples are pairs in plane_u
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
    vec2 c = paired != 0 ? texture(plane_u, uv).rg : vec2(texture(plane_u, uv).r, texture(plane_v, uv).r);
    float cb = c.x * chroma.x + chroma.y;
    float cr = c.y * chroma.x + chroma.y;
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

// Takes note of a picture's layout. When it differs from the last one's (a new video),
// the textures are sized afresh and the program told the picture's colours. False for a
// layout this does not handle.
bool Player::describe(const void *picture)
{
    const auto *frame = static_cast<const AVFrame *>(picture);
    const auto format = static_cast<AVPixelFormat>(frame->format);
    // The console's decoder's pictures (brightness, then colour in pairs), or a software
    // decoder's three planes.
    const bool paired = format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_P010;
    const AVPixFmtDescriptor *layout = paired ? av_pix_fmt_desc_get(format) : planar_layout(frame);
    if (layout == nullptr)
        return false;
    if (format_ == frame->format && width_ == frame->width && height_ == frame->height)
        return true;
    format_ = frame->format;
    width_ = frame->width;
    height_ = frame->height;
    paired_ = paired;
    const int depth = layout->comp[0].depth;
    bytes_ = depth > 8 ? 2 : 1;
    const int chroma_width = AV_CEIL_RSHIFT(frame->width, layout->log2_chroma_w);
    const int chroma_height = AV_CEIL_RSHIFT(frame->height, layout->log2_chroma_h);
    for (int plane = 0; plane < 3; ++plane)
    {
        plane_width_[plane] = plane == 0 ? frame->width : chroma_width;
        plane_height_[plane] = plane == 0 ? frame->height : chroma_height;
    }
    planes_sized_ = false;

    // (Ten-bit pictures from the console's decoder keep their bits at the top of sixteen:
    // for the arithmetic they are sixteen-bit pictures.)
    const Colours colours = colours_of(frame, paired && bytes_ == 2 ? 16 : depth, bytes_);
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "paired"), paired ? 1 : 0);
    glUniform2f(glGetUniformLocation(program_, "luma"), colours.luma[0], colours.luma[1]);
    glUniform2f(glGetUniformLocation(program_, "chroma"), colours.chroma[0], colours.chroma[1]);
    glUniform2f(glGetUniformLocation(program_, "weights"), colours.kr, colours.kb);
    glUniform1i(glGetUniformLocation(program_, "transfer"), colours.pq ? 1 : colours.hlg ? 2 : 0);
    glUniform1i(glGetUniformLocation(program_, "wide"), colours.wide ? 1 : 0);
    glUniform1f(glGetUniformLocation(program_, "peak"), colours.peak);
    glUseProgram(0);
    const AVRational shape = frame->sample_aspect_ratio;
    picture_aspect_ = static_cast<float>(frame->width) / static_cast<float>(frame->height) *
                      (shape.num > 0 && shape.den > 0 ? static_cast<float>(av_q2d(shape)) : 1.0f);
    note("picture: %dx%d %s, transfer %d, range %d, gl error 0x%x", frame->width, frame->height,
         av_get_pix_fmt_name(format), frame->color_trc, frame->color_range, glGetError());
    return true;
}

bool Player::bars_decided(float &top, float &bottom) const
{
    top = bottom = 0;
    if (session_ == nullptr)
        return false;
    std::lock_guard lock{session_->mutex};
    top = session_->bars_top;
    bottom = session_->bars_bottom;
    return session_->bars_decided;
}

// Sends a picture to the graphics card: each of its planes is a texture of its own, of 8
// or 16 bits a sample, and of two samples a place where the colour comes in pairs.
void Player::upload(const void *picture)
{
    const auto *frame = static_cast<const AVFrame *>(picture);
    const int count = paired_ ? 2 : 3;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int plane = 0; plane < count; ++plane)
    {
        const bool pairs = paired_ && plane == 1;
        const int sample = bytes_ * (pairs ? 2 : 1);
        const GLenum inside = pairs ? (bytes_ == 2 ? GL_RG16 : GL_RG8) : (bytes_ == 2 ? GL_R16 : GL_R8);
        const GLenum given = pairs ? GL_RG : GL_RED;
        const GLenum type = bytes_ == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
        glActiveTexture(GL_TEXTURE0 + plane);
        glBindTexture(GL_TEXTURE_2D, planes_[plane]);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[plane] / sample);
        if (!planes_sized_)
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(inside), plane_width_[plane], plane_height_[plane], 0,
                         given, type, frame->data[plane]);
        else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, plane_width_[plane], plane_height_[plane], given, type,
                            frame->data[plane]);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    planes_sized_ = true;
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
        if (describe(due))
        {
            const double before = now_seconds();
            upload(due);
            const double after = now_seconds();
            has_picture_ = true;
            ++shown_;
            // Every so often the log gets how sending pictures is going.
            longest_ = std::max(longest_, after - before);
            if (after - reported_ > 20.0)
            {
                note("pictures: %lu shown, %lu skipped, longest upload %.1f ms", shown_, skipped_,
                     longest_ * 1e3);
                reported_ = after;
                longest_ = 0;
            }
        }
        else if (!has_picture_)
        {
            session_->fail(std::string{"Pictures in the "} +
                           av_get_pix_fmt_name(static_cast<AVPixelFormat>(due->format)) +
                           " layout are not supported yet.");
        }
        av_frame_free(&due);
    }
    if (!has_picture_)
        return;

    // Fitted to the screen with its own shape kept: bars above and below, or at the sides.
    // (The screen is whatever part of the framebuffer is being drawn into: on this
    // console that is not always the whole of it.)
    GLint screen[4] = {};
    glGetIntegerv(GL_VIEWPORT, screen);
    const float screen_aspect = static_cast<float>(width) / static_cast<float>(height);
    int shown_width = width, shown_height = height;
    if (picture_aspect_ > screen_aspect)
        shown_height = static_cast<int>(static_cast<float>(width) / picture_aspect_ + 0.5f);
    else
        shown_width = static_cast<int>(static_cast<float>(height) * picture_aspect_ + 0.5f);
    glViewport(screen[0] + (width - shown_width) / 2, screen[1] + (height - shown_height) / 2, shown_width,
               shown_height);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(program_);
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
    glViewport(screen[0], screen[1], screen[2], screen[3]);
}
} // namespace nx
