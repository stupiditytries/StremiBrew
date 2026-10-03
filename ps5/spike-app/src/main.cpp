// Phase 0.1 spike: runs the Rust standard-library checks on the console, shows the
// results on screen and writes them to /download0/spike.log.

#include "demo_renderer.hpp"

#include <cstdint>
#include <cstdio>

extern "C"
{
    std::uint32_t spike_threads(std::uint32_t count);
    std::int32_t spike_files(const char *dir);
    std::uint64_t spike_wall_clock_ms();
    std::uint64_t spike_monotonic_ms(std::uint32_t ms);
    std::int32_t spike_http_status(const char *host);
}

namespace
{
constexpr unsigned kLineCount = 6;
char lines[kLineCount][64];

void record_results() noexcept
{
    std::snprintf(lines[0], sizeof lines[0], "THREADS %u OF 8", spike_threads(8));
    std::snprintf(lines[1], sizeof lines[1], "FILES STEP %d", spike_files("/download0/spike"));
    std::snprintf(lines[2], sizeof lines[2], "WALL CLOCK %llu",
                  static_cast<unsigned long long>(spike_wall_clock_ms()));
    std::snprintf(lines[3], sizeof lines[3], "SLEEP 250 TOOK %llu",
                  static_cast<unsigned long long>(spike_monotonic_ms(250)));
    std::snprintf(lines[4], sizeof lines[4], "HTTP EXAMPLE.COM %d",
                  spike_http_status("example.com"));
    std::snprintf(lines[5], sizeof lines[5], "DONE");

    if (std::FILE *log = std::fopen("/download0/spike.log", "w"))
    {
        for (const auto &line : lines)
            std::fprintf(log, "%s\n", line);
        std::fclose(log);
    }
}

void draw_scene(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;

    canvas.clear(Color::background);
    canvas.text(120, 90, "RUST STD SPIKE", 10, Color::white);
    for (unsigned index = 0; index < kLineCount; ++index)
        canvas.text(120, 300 + index * 110, lines[index], 6, Color::cyan);
}
} // namespace

int main()
{
    record_results();
    ps5::demo::run(draw_scene, "RUST SPIKE READY");
}
