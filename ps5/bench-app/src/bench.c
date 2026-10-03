/*
 * Phase 0.3 spike: software decode speed. Decodes the video stream of a clip as fast as
 * possible, once per thread count, and reports frames per second through `log`.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>

void app_heap_stats(size_t *in_use, size_t *peak, size_t *mapped);

static double seconds_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void logf_line(void (*log)(const char *), const char *format, ...)
{
    char line[160];
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    __builtin_va_end(arguments);
    log(line);
}

/* Decodes up to `frame_limit` frames with `threads` decoder threads. Returns frames per
 * second, or a negative FFmpeg error code. */
static double decode_pass(void (*log)(const char *), const char *path, int threads,
                          int64_t frame_limit, int describe)
{
    AVFormatContext *input = NULL;
    int status = avformat_open_input(&input, path, NULL, NULL);
    if (status < 0)
        return status;
    status = avformat_find_stream_info(input, NULL);
    if (status < 0)
    {
        avformat_close_input(&input);
        return status;
    }
    const AVCodec *codec = NULL;
    int index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (index < 0)
    {
        avformat_close_input(&input);
        return index;
    }
    AVStream *stream = input->streams[index];
    AVCodecContext *decoder = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(decoder, stream->codecpar);
    decoder->thread_count = threads;
    decoder->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    status = avcodec_open2(decoder, codec, NULL);
    if (status < 0)
    {
        avcodec_free_context(&decoder);
        avformat_close_input(&input);
        return status;
    }
    if (describe)
    {
        const char *format = av_get_pix_fmt_name(decoder->pix_fmt);
        logf_line(log, "CLIP %s %dx%d %s", codec->name, decoder->width, decoder->height,
                  format != NULL ? format : "unknown");
        logf_line(log, "CLIP %.2f fps, %lld kbit/s, transfer %d primaries %d",
                  av_q2d(stream->avg_frame_rate), (long long)(input->bit_rate / 1000),
                  (int)decoder->color_trc, (int)decoder->color_primaries);
    }

    logf_line(log, "BEGIN %d threads (decoder uses %d, type %d)", threads,
              decoder->thread_count, decoder->active_thread_type);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int64_t frames = 0;
    int64_t packets = 0;
    int64_t send_errors = 0;
    int first_send_error = 0;
    int read_error = 0;
    int draining = 0;
    double start = seconds_now();
    while (frames < frame_limit)
    {
        if (!draining)
        {
            status = av_read_frame(input, packet);
            if (status < 0)
            {
                read_error = status;
                draining = 1;
                avcodec_send_packet(decoder, NULL);
            }
            else
            {
                if (packet->stream_index == index)
                {
                    ++packets;
                    int sent = avcodec_send_packet(decoder, packet);
                    if (sent < 0)
                    {
                        if (send_errors++ == 0)
                            first_send_error = sent;
                    }
                }
                av_packet_unref(packet);
            }
        }
        for (;;)
        {
            status = avcodec_receive_frame(decoder, frame);
            if (status < 0)
                break;
            if (++frames % 300 == 0)
                logf_line(log, "  %lld frames at %.1f s", (long long)frames,
                          seconds_now() - start);
            av_frame_unref(frame);
        }
        if (draining && status == AVERROR_EOF)
            break;
    }
    double elapsed = seconds_now() - start;
    {
        char read_text[64] = "none";
        char send_text[64] = "none";
        if (read_error < 0)
            av_strerror(read_error, read_text, sizeof read_text);
        if (first_send_error < 0)
            av_strerror(first_send_error, send_text, sizeof send_text);
        logf_line(log, "  packets %lld, frames %lld, %.2f s", (long long)packets,
                  (long long)frames, elapsed);
        logf_line(log, "  read end: %s; send errors %lld, first: %s", read_text,
                  (long long)send_errors, send_text);
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
    if (frames == 0 || elapsed <= 0)
        return 0;
    logf_line(log, "THREADS %2d: %lld frames in %.2f s", threads, (long long)frames, elapsed);
    return (double)frames / elapsed;
}

void bench_run(const char *path, void (*log)(const char *), void (*result)(int, double))
{
    static const int thread_counts[] = {1, 4, 8, 12, 16};
    logf_line(log, "FILE %s", path);
    for (unsigned i = 0; i < sizeof thread_counts / sizeof thread_counts[0]; ++i)
    {
        int threads = thread_counts[i];
        /* The single-thread pass only needs enough frames for a stable figure. */
        double rate = decode_pass(log, path, threads, threads == 1 ? 240 : 1500, i == 0);
        if (rate < 0)
        {
            char text[96];
            av_strerror((int)rate, text, sizeof text);
            logf_line(log, "ERROR %s", text);
            result(threads, rate);
            return;
        }
        size_t in_use, peak, mapped;
        app_heap_stats(&in_use, &peak, &mapped);
        logf_line(log, "RESULT %2d threads: %.1f fps (heap peak %zu MiB, mapped %zu MiB)",
                  threads, rate, peak >> 20, mapped >> 20);
        result(threads, rate);
        if (rate == 0)
        {
            log("STOP: nothing was decoded");
            return;
        }
    }
    log("DONE");
}
