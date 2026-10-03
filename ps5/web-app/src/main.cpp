// Web-view spike: tries to show Stremio's web UI full screen inside this app through the
// console's built-in web engine. Each step's result is written to /download0/web.log and
// drawn behind the web view, so a failure is visible on screen.

#include "demo_renderer.hpp"

#include <cctype>
#include <cstdio>

extern "C"
{
    int webview_open(const char *url, void (*log)(const char *));
    void webview_update(void);
}

namespace
{
constexpr unsigned kLineCount = 9;
constexpr unsigned kLineLength = 46;
char lines[kLineCount][kLineLength + 1];
unsigned line_count = 0;

void log_line(const char *text) noexcept
{
    if (std::FILE *log = std::fopen("/download0/web.log", "a"))
    {
        std::fprintf(log, "%s\n", text);
        std::fclose(log);
    }
    if (line_count >= kLineCount)
        return;
    char *line = lines[line_count++];
    unsigned index = 0;
    for (; index < kLineLength && text[index] != 0; ++index)
    {
        const unsigned char letter = static_cast<unsigned char>(text[index]);
        line[index] = letter < 128 && (std::isalnum(letter) || letter == ' ')
                          ? static_cast<char>(std::toupper(letter))
                          : ' ';
    }
    line[index] = 0;
}

void draw_scene(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;

    webview_update();
    canvas.clear(Color::background);
    canvas.text(120, 90, "WEB VIEW SPIKE", 10, Color::white);
    for (unsigned index = 0; index < line_count; ++index)
        canvas.text(120, 280 + index * 85, lines[index], 5, Color::cyan);
}
} // namespace

int main()
{
    std::remove("/download0/web.log");
    webview_open("https://web.stremio.com/", [](const char *text) { log_line(text); });
    ps5::demo::run(draw_scene, "WEB VIEW SPIKE READY");
}
