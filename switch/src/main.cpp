// The app on the Switch: opens the display, starts stremio-core, and runs the UI.
//
// What it has to say goes to sdmc:/switch/StremiBrew/log.txt (and, if it dies, what it
// died of: see crash_log.cpp).

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
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
#include "core_link.hpp"
#include "play_control.hpp"

extern "C"
{
std::int32_t stremio_core_init(const char *storage_dir);
std::size_t stremio_core_last_error(char *out, std::size_t capacity);
std::int32_t stremio_core_fetch_file(const char *url, const char *path);
bool stremio_core_fetch_failed(const char *url);
std::size_t stremio_core_trim_folder(const char *folder, std::uint64_t limit);
void stremio_core_sign_in_start(void);
void stremio_core_sign_in_cancel(void);
void stremio_core_sign_out(void);
std::int32_t stremio_core_load_details(const char *type, const char *id, const char *video);
void stremio_core_set_languages(const char *audio, const char *subtitles);
std::int32_t stremio_core_library_set(const char *id, bool add);
void stremio_core_forget_progress(const char *id);
void stremio_core_calendar_load(std::int32_t year, std::uint32_t month);
void stremio_core_discover_choose(std::int32_t kind, std::uint32_t index);

// system_fixes.c
void clock_check_against_network(void);
// The bridge's lines for the log (see the bridge's horizon.rs).
void stremio_host_log(const char *text);
}

void watch_for_endings(); // crash_log.cpp

namespace
{
constexpr char kDataFolder[] = "sdmc:/switch/StremiBrew";
constexpr char kLogFile[] = "sdmc:/switch/StremiBrew/log.txt";
constexpr char kStorageFolder[] = "sdmc:/switch/StremiBrew/core";
constexpr char kImageFolder[] = "sdmc:/switch/StremiBrew/images";
constexpr char kDisplayFile[] = "sdmc:/switch/StremiBrew/display.txt";
constexpr char kSubtitleStyleFile[] = "sdmc:/switch/StremiBrew/subtitle-style.txt";
constexpr char kQualityFile[] = "sdmc:/switch/StremiBrew/quality.txt";
// The window is the size of the television's picture; in the hand, the top left of it
// that the console's own screen shows is drawn into instead.
constexpr int kDockedWidth = 1920, kDockedHeight = 1080;
constexpr int kHandheldWidth = 1280, kHandheldHeight = 720;
} // namespace

void log_line(const char *format, ...)
{
    // One line at a time, whichever thread it comes from.
    static std::mutex writing;
    char text[768];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    const std::lock_guard lock{writing};
    if (std::FILE *log = std::fopen(kLogFile, "a"))
    {
        std::fprintf(log, "%s\n", text);
        std::fclose(log);
    }
}

void stremio_host_log(const char *text)
{
    log_line("%s", text);
}

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
// while held; A accepts and B goes back, as on the console itself; L and R step back and
// forward; Plus is the options button.
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

double seconds_now()
{
    return static_cast<double>(armTicksToNs(armGetSystemTick())) / 1e9;
}

// The console's own keyboard, for typing a search. It takes the screen until it is
// closed. Returns 1 with what was typed, 0 when it was closed without, and -1 when the
// console would not open it at all.
int ask_keyboard(const std::string &text, std::string &typed)
{
    SwkbdConfig keyboard;
    const Result made = swkbdCreate(&keyboard, 0);
    if (R_FAILED(made))
    {
        log_line("keyboard: the console's keyboard is not to be had (0x%x)", made);
        return -1;
    }
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetGuideText(&keyboard, "Search");
    swkbdConfigSetOkButtonText(&keyboard, "Search");
    swkbdConfigSetInitialText(&keyboard, text.c_str());
    swkbdConfigSetStringLenMax(&keyboard, 60);
    char out[256] = {};
    const Result shown = swkbdShow(&keyboard, out, sizeof out);
    swkbdClose(&keyboard);
    if (R_FAILED(shown))
        return 0;
    typed = out;
    return 1;
}
} // namespace

int main()
{
    mkdir("sdmc:/switch", 0777);
    mkdir(kDataFolder, 0777);
    mkdir(kImageFolder, 0777);
    std::remove(kLogFile);
    log_line("start");
    watch_for_endings();
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

    {
        ui::App app{vg, "romfs:/fonts", kImageFolder};
        app.set_image_fetcher(
            [](const std::string &address, const std::string &file) {
                stremio_core_fetch_file(address.c_str(), file.c_str());
            },
            [](const std::string &address) { return stremio_core_fetch_failed(address.c_str()); });
        // Not on this console: trailers on the home screen (yet), and speech recognition.
        // Its own: a limit on how much picture a stream may have, which starts at what
        // the console's screen and its television output can show.
        app.set_features({false, false, true});
        {
            int limit = 1080;
            if (std::FILE *file = std::fopen(kQualityFile, "r"))
            {
                if (std::fscanf(file, "%d", &limit) != 1 || limit < 0 || limit > 4320)
                    limit = 1080;
                std::fclose(file);
            }
            app.set_quality_limit(limit);
        }
        app.set_quality_handler([](int limit) {
            if (std::FILE *file = std::fopen(kQualityFile, "w"))
            {
                std::fprintf(file, "%d\n", limit);
                std::fclose(file);
            }
        });

        // Everything asked of the core goes through the link's thread (see core_link.hpp).
        ps5::CoreLink core;
        app.set_intent_handler([&core](ui::Intent intent) {
            core.post([intent] {
                switch (intent)
                {
                case ui::Intent::SignIn:
                    stremio_core_sign_in_start();
                    break;
                case ui::Intent::CancelSignIn:
                    stremio_core_sign_in_cancel();
                    break;
                case ui::Intent::SignOut:
                    stremio_core_sign_out();
                    break;
                }
            });
        });

        nx::PlayControl playing{app, core, vg};
        app.set_title_handler({
            [&core](const std::string &type, const std::string &id) {
                core.post([type, id] { stremio_core_load_details(type.c_str(), id.c_str(), nullptr); });
            },
            [&core](const std::string &type, const std::string &id, const std::string &episode) {
                core.post([type, id, episode] {
                    stremio_core_load_details(type.c_str(), id.c_str(), episode.c_str());
                });
            },
            [] {},
            [&playing](const ui::Stream &stream, const std::string &title, const std::string &type,
                       const std::string &id, const std::string &video) {
                log_line("playing \"%s\" from %s", title.c_str(), stream.addon.c_str());
                playing.start(stream, type, id, video);
            },
        });
        app.set_player_handler(playing.handler());

        // How subtitles look is kept in a small file: four whole numbers.
        {
            ui::SubtitleStyle style;
            if (std::FILE *file = std::fopen(kSubtitleStyleFile, "r"))
            {
                ui::SubtitleStyle saved;
                int bold = 0;
                if (std::fscanf(file, "%d %d %d %d", &saved.size, &saved.background, &saved.colour, &bold) == 4 &&
                    saved.size >= 50 && saved.size <= 200 && saved.background >= 0 && saved.background <= 100 &&
                    saved.colour >= 0 && saved.colour < static_cast<int>(std::size(ui::kSubtitleColours)))
                {
                    saved.bold = bold != 0;
                    style = saved;
                }
                std::fclose(file);
            }
            app.set_subtitle_style(style);
        }
        app.set_subtitle_style_handler([](const ui::SubtitleStyle &style) {
            if (std::FILE *file = std::fopen(kSubtitleStyleFile, "w"))
            {
                std::fprintf(file, "%d %d %d %d\n", style.size, style.background, style.colour, style.bold ? 1 : 0);
                std::fclose(file);
            }
        });

        app.set_library_handler([&core](ui::LibraryAction action, const ui::BoardItem &item) {
            const std::string id = item.id;
            log_line("library: %s %s", action == ui::LibraryAction::Add      ? "add"
                                       : action == ui::LibraryAction::Remove ? "remove"
                                                                             : "forget progress of",
                     id.c_str());
            core.post([action, id] {
                if (action == ui::LibraryAction::Forget)
                    stremio_core_forget_progress(id.c_str());
                else
                    stremio_core_library_set(id.c_str(), action == ui::LibraryAction::Add);
            });
        });
        app.set_calendar_handler([&core](int year, int month) {
            core.post([year, month] { stremio_core_calendar_load(year, static_cast<std::uint32_t>(month)); });
        });
        app.set_discover_handler([&core](int kind, int index) {
            core.post([kind, index] { stremio_core_discover_choose(kind, static_cast<std::uint32_t>(index)); });
        });
        app.set_languages_handler([&core](const std::string &audio, const std::string &subtitles) {
            core.post([audio, subtitles] { stremio_core_set_languages(audio.c_str(), subtitles.c_str()); });
        });

        // Searches are typed on the console's keyboard. It is asked for between frames
        // (it takes the screen); if the console will not open it, the app's own is used.
        bool keyboard_wanted = false, keyboard_there = true;
        std::string keyboard_text;
        app.set_keyboard_handler([&](const std::string &text) {
            if (!keyboard_there)
                return false;
            keyboard_wanted = true;
            keyboard_text = text;
            return true;
        });

        // Which UI to use (the handheld one, and whether to choose by itself) is kept in
        // a small file: two whole numbers.
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

        // The network, the time and the core are brought up beside the first frames
        // rather than before them (asking the network the time can take seconds).
        std::thread starting{[&core] {
            // More room than the console's usual for what arrives (a video is read through
            // these sockets), and enough sessions for the downloads that run side by side.
            static const SocketInitConfig sockets = {
                .tcp_tx_buf_size = 0x8000,
                .tcp_rx_buf_size = 0x40000,
                .tcp_tx_buf_max_size = 0x40000,
                .tcp_rx_buf_max_size = 0x100000,
                .udp_tx_buf_size = 0x2400,
                .udp_rx_buf_size = 0xA500,
                .sb_efficiency = 4,
                .num_bsd_sessions = 8,
                .bsd_service_type = BsdServiceType_User,
            };
            const Result network = socketInitialize(&sockets);
            log_line("network %s (0x%x)", R_SUCCEEDED(network) ? "ready" : "not available", network);
            if (R_SUCCEEDED(network))
                clock_check_against_network();
            mkdir(kStorageFolder, 0777);
            if (stremio_core_init(kStorageFolder) != 0)
            {
                char error[256] = {};
                stremio_core_last_error(error, sizeof error);
                log_line("core did not start: %s", error);
                return;
            }
            log_line("core started");
            core.start();
            core.post([] {
                // Downloaded artwork is kept between runs up to this size; the oldest goes first.
                constexpr std::uint64_t kImageFolderLimit = std::uint64_t{300} << 20;
                const std::size_t trimmed = stremio_core_trim_folder(kImageFolder, kImageFolderLimit);
                log_line("image folder: removed %zu old files", trimmed);
            });
        }};

        Pad pad;
        double previous = seconds_now(), slow_logged = 0;
        unsigned long frames = 0;
        bool was_docked = false;
        while (appletMainLoop())
        {
            const double frame_start = seconds_now();
            // (A long gap, such as the keyboard leaves, is not animated through.)
            const float elapsed = std::min(static_cast<float>(frame_start - previous), 0.1f);
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

            // Whatever the core's thread has ready is applied between frames. Rows read
            // for a view the screen has since left are dropped.
            core.set_view(static_cast<int>(app.view()), app.search_query());
            std::vector<ui::BoardRow> rows;
            int rows_view = 0;
            std::string rows_query;
            if (core.take_board(rows, rows_view, rows_query) && rows_view == static_cast<int>(app.view()) &&
                rows_query == app.search_query())
                app.set_board(std::move(rows));
            core.set_page(static_cast<int>(app.page()));
            ui::CalendarMonth month;
            if (core.take_calendar(month))
                app.set_calendar(std::move(month));
            std::vector<ui::Addon> addons;
            if (core.take_addons(addons))
                app.set_addons(std::move(addons));
            ui::DiscoverData discover;
            if (core.take_discover(discover))
                app.set_discover(std::move(discover));
            ui::Details details;
            if (core.take_details(details))
                app.set_details(std::move(details));
            ui::Account account;
            if (core.take_account(account))
                app.set_account(std::move(account));
            core.set_focused_catalog(app.focused_catalog());
            core.set_title_open(app.title_open());

            pad.poll(elapsed, [&](ui::Button button) { app.press(button); });
            if (keyboard_wanted)
            {
                keyboard_wanted = false;
                std::string typed;
                const int outcome = ask_keyboard(keyboard_text, typed);
                if (outcome > 0)
                    app.submit_search(typed);
                else if (outcome < 0)
                    keyboard_there = false; // the app's own keyboard from the next press on
                previous = seconds_now();
            }
            const double applied = seconds_now();

            // The picture shown is the window's top left; OpenGL counts rows from the bottom.
            glViewport(0, kDockedHeight - height, width, height);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            // A playing video's picture goes under the UI, which then draws only its controls.
            playing.frame(width, height);
            app.update(elapsed);
            app.draw(width, height);
            const double drawn = seconds_now();
            eglSwapBuffers(display.display, display.surface);
            const double presented = seconds_now();

            if (presented - frame_start > 0.030 && frames > 120 && presented - slow_logged > 1.0)
            {
                slow_logged = presented;
                log_line("slow frame: %.0f ms (data and input %.1f, drawing %.1f, presenting %.1f)",
                         (presented - frame_start) * 1e3, (applied - frame_start) * 1e3, (drawn - applied) * 1e3,
                         (presented - drawn) * 1e3);
            }
            if (++frames == 1 || frames == 120 || frames % 3600 == 0)
                log_line("frame %lu, gl error 0x%x", frames, glGetError());
        }
        log_line("closing");
        starting.join();
    }
    nvgDeleteGL3(vg);
    display.close();
    romfsExit();
    return 0;
}
