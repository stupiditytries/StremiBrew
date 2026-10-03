// Phase 0.3 spike: decodes the clip packaged in the app's assets folder and shows the
// frames per second for each thread count. Results are also written to
// /download0/bench.log.

#include "demo_renderer.hpp"

#include <cstdio>

extern "C" void bench_run(const char *path, void (*log)(const char *),
                          void (*result)(int, double));

namespace
{
constexpr unsigned kLineCount = 6;
char lines[kLineCount][64];
unsigned line_count = 0;

void log_line(const char *text) noexcept
{
    if (std::FILE *log = std::fopen("/download0/bench.log", "a"))
    {
        std::fprintf(log, "%s\n", text);
        std::fclose(log);
    }
}

void record_result(int threads, double rate) noexcept
{
    if (line_count >= kLineCount)
        return;
    if (rate < 0)
        std::snprintf(lines[line_count], sizeof lines[0], "ERROR %d", static_cast<int>(rate));
    else
        std::snprintf(lines[line_count], sizeof lines[0], "%d THREADS %d FPS", threads,
                      static_cast<int>(rate));
    ++line_count;
}

void draw_scene(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;

    canvas.clear(Color::background);
    canvas.text(120, 90, "DECODE BENCH", 10, Color::white);
    for (unsigned index = 0; index < line_count; ++index)
        canvas.text(120, 300 + index * 110, lines[index], 6, Color::cyan);
}
} // namespace

int main()
{
    std::remove("/download0/bench.log");
    bench_run("/app0/assets/clip.mp4", [](const char *text) { log_line(text); },
              [](int threads, double rate) { record_result(threads, rate); });
    ps5::demo::run(draw_scene, "DECODE BENCH READY");
}
