// Runs stremio-core on the console: starts it with storage under /download0, loads the
// board from the installed add-ons, and shows what arrived. The same text is written to
// /download0/core.log.

#include "demo_renderer.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

extern "C"
{
    std::int32_t stremio_core_init(const char *storage_dir);
    std::int32_t stremio_core_load_board(std::uint32_t rows);
    std::size_t stremio_core_poll_event(char *out, std::size_t capacity);
    std::size_t stremio_core_board_summary(char *out, std::size_t capacity);
    std::size_t stremio_core_last_error(char *out, std::size_t capacity);
    void app_heap_stats(std::size_t *in_use, std::size_t *peak, std::size_t *mapped);
}

namespace
{
constexpr unsigned kLineCount = 8;
constexpr unsigned kLineLength = 44;
char lines[kLineCount][kLineLength + 1];
unsigned line_count = 0;

char summary[16 * 1024];
char event[256 * 1024];

void log_line(const char *text) noexcept
{
    if (std::FILE *log = std::fopen("/download0/core.log", "a"))
    {
        std::fprintf(log, "%s\n", text);
        std::fclose(log);
    }
}

// The demo font has capitals only; anything else it lacks becomes a space.
void show(const char *text) noexcept
{
    if (line_count >= kLineCount)
        return;
    char *line = lines[line_count++];
    unsigned index = 0;
    for (; index < kLineLength && text[index] != 0 && text[index] != '\n'; ++index)
    {
        const unsigned char letter = static_cast<unsigned char>(text[index]);
        line[index] = letter < 128 && (std::isalnum(letter) || letter == ' ')
                          ? static_cast<char>(std::toupper(letter))
                          : ' ';
    }
    line[index] = 0;
}

double seconds_now() noexcept
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

void run_core() noexcept
{
    std::remove("/download0/core.log");
    log_line("BEGIN INIT");
    if (stremio_core_init("/download0/stremio") != 0)
    {
        stremio_core_last_error(summary, sizeof summary);
        log_line(summary);
        show("CORE INIT FAILED");
        show(summary);
        return;
    }
    log_line("BEGIN BOARD");
    stremio_core_load_board(6);

    const double start = seconds_now();
    unsigned events = 0;
    for (;;)
    {
        while (stremio_core_poll_event(event, sizeof event) != 0)
            ++events;
        stremio_core_board_summary(summary, sizeof summary);
        const bool settled =
            std::strstr(summary, " loading 0 ") != nullptr && std::strncmp(summary, "rows 0 ", 7) != 0;
        if (settled || seconds_now() - start > 45)
            break;
        timespec pause{0, 100 * 1000 * 1000};
        nanosleep(&pause, nullptr);
    }

    char header[96];
    std::size_t in_use = 0, peak = 0, mapped = 0;
    app_heap_stats(&in_use, &peak, &mapped);
    std::snprintf(header, sizeof header, "%u events in %.1f s, heap peak %zu KiB", events,
                  seconds_now() - start, peak >> 10);
    log_line(header);
    log_line(summary);
    log_line("DONE");

    show(header);
    for (const char *row = summary; row != nullptr && *row != 0;)
    {
        show(row);
        row = std::strchr(row, '\n');
        if (row != nullptr)
            ++row;
    }
}

void draw_scene(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;

    canvas.clear(Color::background);
    canvas.text(120, 90, "STREMIO CORE", 10, Color::white);
    for (unsigned index = 0; index < line_count; ++index)
        canvas.text(120, 290 + index * 90, lines[index], 5, Color::cyan);
}
} // namespace

int main()
{
    run_core();
    ps5::demo::run(draw_scene, "STREMIO CORE READY");
}
