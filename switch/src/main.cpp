// The app on the Switch. For now: the UI alone, on sample data from the app's own
// files, to prove the display, the drawing and the controller. The core, the network and
// the player follow.
//
// What it has to say goes to sdmc:/switch/StremiBrew/log.txt.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

#include <switch.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>

#define NANOVG_GL3_IMPLEMENTATION
#include "nanovg.h"
#include "nanovg_gl.h"

#include "app.hpp"
#include "board_data.hpp"

namespace
{
constexpr char kDataFolder[] = "sdmc:/switch/StremiBrew";
constexpr char kLogFile[] = "sdmc:/switch/StremiBrew/log.txt";
// The window is the size of the television's picture; in the hand, the top left of it
// that the console's own screen shows is drawn into instead.
constexpr int kDockedWidth = 1920, kDockedHeight = 1080;
constexpr int kHandheldWidth = 1280, kHandheldHeight = 720;
} // namespace

void log_line(const char *format, ...)
{
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    if (std::FILE *log = std::fopen(kLogFile, "a"))
    {
        std::fprintf(log, "%s\n", text);
        std::fclose(log);
    }
}

#ifdef WITH_CORE
extern "C"
{
std::int32_t stremio_core_init(const char *storage_dir);
std::int32_t stremio_core_load_board(std::uint32_t rows);
std::size_t stremio_core_poll_event(char *out, std::size_t capacity);
std::size_t stremio_core_board_summary(char *out, std::size_t capacity);
std::size_t stremio_core_last_error(char *out, std::size_t capacity);
}

// A first trial of the core on this console: start it, ask for the board over the
// network, and write what came back to the log.
static void try_core()
{
    mkdir("sdmc:/switch/StremiBrew/core", 0777);
    const Result network = socketInitializeDefault();
    log_line("core trial: network %s (0x%x)", R_SUCCEEDED(network) ? "ready" : "not available", network);
    if (stremio_core_init("sdmc:/switch/StremiBrew/core") != 0)
    {
        char error[256] = {};
        stremio_core_last_error(error, sizeof error);
        log_line("core trial: the core did not start: %s", error);
        return;
    }
    log_line("core trial: core started");
    stremio_core_load_board(6);
    static char text[1 << 16];
    for (int second = 1; second <= 20; ++second)
    {
        svcSleepThread(1'000'000'000ull);
        int events = 0;
        while (stremio_core_poll_event(text, sizeof text) != 0)
            ++events;
        const std::size_t length = stremio_core_board_summary(text, sizeof text);
        text[length < sizeof text ? length : 0] = 0;
        // The first line is the totals, the rest one line per row: all of it now and
        // then, the totals alone in between.
        if (char *end = std::strchr(text, 10); end != nullptr && second % 5 != 0)
            *end = 0;
        log_line("core trial, %d s, %d events: %s", second, events, text);
    }
}
#endif

namespace
{
struct Display
{
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;

    bool open(NWindow *window)
    {
        static const EGLint config_attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
                                                   EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                                   EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
        static const EGLint context_attributes[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                                                    EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                                                    EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
                                                    EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_NONE};
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_API) ||
            !eglChooseConfig(display, config_attributes, &config, 1, &count) || count != 1)
        {
            log_line("display: EGL did not start (0x%x)", eglGetError());
            return false;
        }
        surface = eglCreateWindowSurface(display, config, window, nullptr);
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
            !eglMakeCurrent(display, surface, surface, context))
        {
            log_line("display: no surface or context (0x%x)", eglGetError());
            return false;
        }
        gladLoadGL();
        eglSwapInterval(display, 1);
        log_line("display: %s, %s", reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
        return true;
    }

    void close()
    {
        if (display == EGL_NO_DISPLAY)
            return;
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT)
            eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE)
            eglDestroySurface(display, surface);
        eglTerminate(display);
    }
};

// The controller as UI button presses: directions (the pad's and either stick's) repeat
// while held; A accepts and B goes back, as on the console itself.
class Pad
{
  public:
    Pad()
    {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&state_);
    }

    template <typename Press> void poll(float seconds, Press press)
    {
        padUpdate(&state_);
        const u64 down = padGetButtonsDown(&state_), held = padGetButtons(&state_);
        const auto send = [&](u64 bits) {
            if (bits & HidNpadButton_AnyUp)
                press(ui::Button::Up);
            if (bits & HidNpadButton_AnyDown)
                press(ui::Button::Down);
            if (bits & HidNpadButton_AnyLeft)
                press(ui::Button::Left);
            if (bits & HidNpadButton_AnyRight)
                press(ui::Button::Right);
            if (bits & HidNpadButton_A)
                press(ui::Button::Accept);
            if (bits & HidNpadButton_B)
                press(ui::Button::Back);
            if (bits & HidNpadButton_L)
                press(ui::Button::SkipBack);
            if (bits & HidNpadButton_R)
                press(ui::Button::SkipForward);
            if (bits & HidNpadButton_Plus)
                press(ui::Button::Options);
        };
        constexpr u64 kDirections =
            HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
        send(down);
        if (down & kDirections)
        {
            repeating_ = down & kDirections;
            repeat_in_ = 0.40f;
        }
        else if ((held & repeating_) != repeating_)
            repeating_ = 0;
        else if (repeating_ != 0 && (repeat_in_ -= seconds) <= 0)
        {
            send(repeating_);
            repeat_in_ = 0.11f;
        }
    }

  private:
    PadState state_{};
    u64 repeating_ = 0;
    float repeat_in_ = 0;
};

std::string read_file(const char *path)
{
    std::ifstream file{path, std::ios::binary};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

double seconds_now()
{
    return static_cast<double>(armTicksToNs(armGetSystemTick())) / 1e9;
}
} // namespace

int main()
{
    mkdir("sdmc:/switch", 0777);
    mkdir(kDataFolder, 0777);
    std::remove(kLogFile);
    log_line("start");
    const Result files = romfsInit();
    if (R_FAILED(files))
        log_line("the app's own files did not open (0x%x)", files);

    NWindow *window = nwindowGetDefault();
    nwindowSetDimensions(window, kDockedWidth, kDockedHeight);
    Display display;
    if (!display.open(window))
        return 1;
    NVGcontext *vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (vg == nullptr)
    {
        log_line("drawing library did not start (0x%x)", glGetError());
        return 1;
    }
    log_line("drawing ready");
#ifdef WITH_CORE
    std::thread{try_core}.detach();
#endif

    {
        ui::App app{vg, "romfs:/fonts", "romfs:/images"};
        const std::string board = read_file("romfs:/board.json");
        std::vector<ui::BoardRow> rows;
        log_line("sample board: %zu bytes, %s", board.size(), ui::parse_board(board, rows) ? "read" : "not read");
        app.set_board(rows);
        ui::Account account;
        account.signed_in = true;
        account.email = "sample data";
        account.addons = 0;
        account.audio_language = "eng";
        account.subtitles_language = "eng";
        app.set_account(account);
        // Which UI to use (the handheld one, and whether to choose by itself) is kept in
        // a small file: two whole numbers.
        static constexpr char kDisplayFile[] = "sdmc:/switch/StremiBrew/display.txt";
        {
            int handheld = 0, automatic = 1;
            if (std::FILE *file = std::fopen(kDisplayFile, "r"))
            {
                if (std::fscanf(file, "%d %d", &handheld, &automatic) != 2)
                    handheld = 0, automatic = 1;
                std::fclose(file);
            }
            app.set_display_options(handheld != 0, automatic != 0);
        }
        app.set_display_handler([](bool handheld, bool automatic) {
            if (std::FILE *file = std::fopen(kDisplayFile, "w"))
            {
                std::fprintf(file, "%d %d\n", handheld ? 1 : 0, automatic ? 1 : 0);
                std::fclose(file);
            }
        });
        // Discover is given the board's titles, so that screen has something too.
        ui::DiscoverData discover;
        discover.types = {{"Movie", true}, {"Series", false}};
        discover.catalogs = {{"Popular", true}, {"Featured", false}};
        for (const ui::BoardRow &row : rows)
            for (const ui::BoardItem &item : row.items)
                discover.items.push_back(item);
        app.set_discover(discover);
        app.set_discover_handler([&app, discover](int, int) { app.set_discover(discover); });

        Pad pad;
        double previous = seconds_now(), slow_logged = 0;
        unsigned long frames = 0;
        bool was_docked = false;
        while (appletMainLoop())
        {
            const double frame_start = seconds_now();
            const float elapsed = static_cast<float>(frame_start - previous);
            previous = frame_start;

            const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
            const int width = docked ? kDockedWidth : kHandheldWidth;
            const int height = docked ? kDockedHeight : kHandheldHeight;
            if (frames == 0 || docked != was_docked)
            {
                nwindowSetCrop(window, 0, 0, width, height);
                log_line("%s: drawing at %dx%d", docked ? "docked" : "in the hand", width, height);
                was_docked = docked;
            }

            app.set_docked(docked);
            pad.poll(elapsed, [&](ui::Button button) { app.press(button); });

            // The picture shown is the window's top left; OpenGL counts rows from the bottom.
            glViewport(0, kDockedHeight - height, width, height);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            app.update(elapsed);
            app.draw(width, height);
            const double drawn = seconds_now();
            eglSwapBuffers(display.display, display.surface);
            const double presented = seconds_now();

            if (presented - frame_start > 0.030 && frames > 120 && presented - slow_logged > 1.0)
            {
                slow_logged = presented;
                log_line("slow frame: %.0f ms (drawing %.1f, presenting %.1f)", (presented - frame_start) * 1e3,
                         (drawn - frame_start) * 1e3, (presented - drawn) * 1e3);
            }
            if (++frames == 1 || frames == 120 || frames % 3600 == 0)
                log_line("frame %lu, gl error 0x%x", frames, glGetError());
        }
    }
    log_line("closing");
    nvgDeleteGL3(vg);
    display.close();
    romfsExit();
    return 0;
}
