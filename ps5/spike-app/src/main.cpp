// Phase 0.1 spike: runs the Rust standard-library checks on the console, shows the
// results on screen and writes them to /download0/spike.log.

#include "demo_renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

extern "C"
{
    std::uint32_t spike_threads(std::uint32_t count);
    std::int32_t spike_files(const char *dir);
    std::uint64_t spike_wall_clock_ms();
    std::uint64_t spike_monotonic_ms(std::uint32_t ms);
    std::int32_t spike_http_status(const char *host);
    void spike_detail(char *out, std::size_t capacity);
    void net_diag(void (*log)(const char *));
}

namespace
{
constexpr unsigned kLineCount = 6;
char lines[kLineCount][64];

// Appends one line to the log and closes it, so a crash in the next step still leaves the
// earlier lines on disk.
void log_line(const char *text) noexcept
{
    if (std::FILE *log = std::fopen("/download0/spike.log", "a"))
    {
        std::fprintf(log, "%s\n", text);
        std::fclose(log);
    }
}

// Logs the detail text the last check left behind, if any.
void log_detail() noexcept
{
    char detail[256];
    spike_detail(detail, sizeof detail);
    if (detail[0] != 0)
        log_line(detail);
}

void record_results() noexcept
{
    std::remove("/download0/spike.log");
    log_line("BEGIN THREADS");
    std::snprintf(lines[0], sizeof lines[0], "THREADS %u OF 8", spike_threads(8));
    log_line(lines[0]);
    log_detail();
    log_line("BEGIN FILES");
    std::snprintf(lines[1], sizeof lines[1], "FILES STEP %d", spike_files("/download0/spike"));
    log_line(lines[1]);
    log_detail();
    log_line("BEGIN CLOCKS");
    std::snprintf(lines[2], sizeof lines[2], "WALL CLOCK %llu",
                  static_cast<unsigned long long>(spike_wall_clock_ms()));
    log_line(lines[2]);
    std::snprintf(lines[3], sizeof lines[3], "SLEEP 250 TOOK %llu",
                  static_cast<unsigned long long>(spike_monotonic_ms(250)));
    log_line(lines[3]);
    log_line("BEGIN HTTP");
    std::snprintf(lines[4], sizeof lines[4], "HTTP EXAMPLE.COM %d",
                  spike_http_status("example.com"));
    log_line(lines[4]);
    log_detail();
    net_diag([](const char *text) { log_line(text); });
    std::snprintf(lines[5], sizeof lines[5], "DONE");
    log_line(lines[5]);
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
