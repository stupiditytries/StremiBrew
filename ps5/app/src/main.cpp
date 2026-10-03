// The app on the console: opens the display, starts stremio-core, and runs the UI.
//
// Progress and failures are written to /download0/app.log, one line at a time, so a
// start-up problem can be read back after the fact.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <sys/stat.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <ps5_opengl_display_modes.h>

#define NANOVG_GL3_IMPLEMENTATION
#include "nanovg.h"
#include "nanovg_gl.h"

#include "account_data.hpp"
#include "app.hpp"
#include "board_data.hpp"
#include "core_link.hpp"
#include "details_data.hpp"
#include "pad.hpp"
#include "theme.hpp"

extern "C"
{
    std::int32_t stremio_core_init(const char *storage_dir);
    std::int32_t stremio_core_load_board(std::uint32_t rows);
    std::size_t stremio_core_poll_event(char *out, std::size_t capacity);
    std::size_t stremio_core_board_rows(std::uint32_t items_per_row, char *out,
                                        std::size_t capacity);
    std::size_t stremio_core_last_error(char *out, std::size_t capacity);
    std::int32_t stremio_core_fetch_file(const char *url, const char *path);
    bool stremio_core_fetch_failed(const char *url);
    std::size_t stremio_core_trim_folder(const char *folder, std::uint64_t limit);
    void stremio_core_board_load_range(std::uint32_t start, std::uint32_t end);
    void stremio_core_sign_in_start(void);
    void stremio_core_sign_in_cancel(void);
    void stremio_core_sign_out(void);
    void stremio_core_account_advance(void);
    std::size_t stremio_core_account(char *out, std::size_t capacity);
    std::int32_t stremio_core_load_details(const char *type, const char *id, const char *video);
    std::size_t stremio_core_details(char *out, std::size_t capacity);
    void app_heap_stats(std::size_t *in_use, std::size_t *peak, std::size_t *mapped);
    int sceKernelDebugOutText(int channel, const char *text);
}

namespace
{
constexpr char kLogFile[] = "/download0/app.log";
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
    // Also to the console's kernel log, which can be read from a PC after a crash (the
    // file above cannot once the app has closed).
    char line[560];
    std::snprintf(line, sizeof line, "[stremio] %s\n", text);
    sceKernelDebugOutText(0, line);
}

namespace
{
constexpr char kStorageFolder[] = "/download0/stremio";
constexpr char kImageFolder[] = "/download0/images";
constexpr char kFontFolder[] = "/app0/assets/fonts";


double seconds_now()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

// The display: an OpenGL 3.3 context on the console's screen.
struct Display
{
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    int width = 0;
    int height = 0;

    bool open()
    {
        const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                            EGL_NONE};
        const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
                                             EGL_CONTEXT_MINOR_VERSION_KHR, 3,
                                             EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                                             EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        // The UI is drawn at 4K; the console scales it for a display that shows less. The
        // mode can only be chosen before EGL starts.
        if (!eglSetDisplayModePS5(display, 3840, 2160) || !eglSetDisplayRefreshPS5(display, 60))
            log_line("display: 4K mode refused (0x%x), staying at the default", eglGetError());
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr) ||
            !eglBindAPI(EGL_OPENGL_API) ||
            !eglChooseConfig(display, config_attributes, &config, 1, &count) || count != 1)
        {
            log_line("display: EGL initialisation failed (0x%x)", eglGetError());
            return false;
        }
        surface = eglCreateWindowSurface(display, config, static_cast<EGLNativeWindowType>(0),
                                         nullptr);
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
            !eglMakeCurrent(display, surface, surface, context))
        {
            log_line("display: no surface or context (0x%x)", eglGetError());
            return false;
        }
        EGLint surface_width = 0, surface_height = 0;
        eglQuerySurface(display, surface, EGL_WIDTH, &surface_width);
        eglQuerySurface(display, surface, EGL_HEIGHT, &surface_height);
        width = surface_width;
        height = surface_height;
        eglSwapInterval(display, 1);
        log_line("display: %dx%d, %s, %s", width, height,
                 reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
        return width > 0 && height > 0;
    }

    bool present() const
    {
        return eglSwapBuffers(display, surface) == EGL_TRUE;
    }
};

// The UI is drawn into a texture that has a stencil buffer (which the drawing library
// needs and the screen's own surface is not promised to have), and that texture is then
// drawn to the screen with one triangle. Copying it with glBlitFramebuffer instead would
// take the driver's slow path.
class Canvas
{
  public:
    bool create(int width, int height)
    {
        width_ = width;
        height_ = height;
        glGenFramebuffers(1, &framebuffer_);
        glGenTextures(1, &color_);
        glGenRenderbuffers(1, &stencil_);
        glBindTexture(GL_TEXTURE_2D, color_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindRenderbuffer(GL_RENDERBUFFER, stencil_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                  stencil_);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            log_line("canvas: framebuffer incomplete (0x%x)", status);
            return false;
        }
        return create_program();
    }

    void begin() const
    {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glViewport(0, 0, width_, height_);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    // Draws the canvas over the whole screen.
    void show(int screen_width, int screen_height) const
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, screen_width, screen_height);
        glDisable(GL_BLEND);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(program_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, color_);
        glBindVertexArray(vertex_array_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glUseProgram(0);
    }

  private:
    static GLuint compile(GLenum type, const char *source)
    {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE)
        {
            char text[512] = {};
            glGetShaderInfoLog(shader, sizeof text - 1, nullptr, text);
            log_line("canvas: shader failed: %s", text);
        }
        return shader;
    }

    bool create_program()
    {
        // One triangle that covers the screen; its corners are derived from the vertex
        // number, so no vertex data is needed.
        static const char kVertex[] = "#version 330 core\n"
                                      "out vec2 uv;\n"
                                      "void main() {\n"
                                      "  uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
                                      "  gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);\n"
                                      "}\n";
        static const char kFragment[] = "#version 330 core\n"
                                        "in vec2 uv;\n"
                                        "out vec4 color;\n"
                                        "uniform sampler2D canvas;\n"
                                        "void main() { color = vec4(texture(canvas, uv).rgb, 1.0); }\n";
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
            log_line("canvas: program did not link");
            return false;
        }
        glUseProgram(program_);
        glUniform1i(glGetUniformLocation(program_, "canvas"), 0);
        glUseProgram(0);
        glGenVertexArrays(1, &vertex_array_);
        return true;
    }

    int width_ = 0, height_ = 0;
    GLuint framebuffer_ = 0, color_ = 0, stencil_ = 0, program_ = 0, vertex_array_ = 0;
};

} // namespace

int main()
{
    std::remove(kLogFile);
    log_line("start");
    mkdir(kImageFolder, 0777);

    Display display;
    if (!display.open())
        return 1;
    // The canvas matches the screen; the UI's 1920x1080 layout is scaled up to it, so text
    // and shapes are drawn at the screen's full sharpness.
    Canvas canvas;
    const int width = display.width;
    const int height = display.height;
    if (!canvas.create(width, height))
        return 1;
    NVGcontext *vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (vg == nullptr)
    {
        log_line("drawing library did not start (0x%x)", glGetError());
        return 1;
    }
    log_line("drawing ready");

    ui::App app{vg, kFontFolder, kImageFolder};
    app.set_image_fetcher(
        [](const std::string &address, const std::string &file) {
            stremio_core_fetch_file(address.c_str(), file.c_str());
        },
        [](const std::string &address) { return stremio_core_fetch_failed(address.c_str()); });

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
    app.set_title_handler({
        [&core](const std::string &type, const std::string &id) {
            core.post([type, id] { stremio_core_load_details(type.c_str(), id.c_str(), nullptr); });
        },
        [&core](const std::string &type, const std::string &id, const std::string &video) {
            core.post([type, id, video] {
                stremio_core_load_details(type.c_str(), id.c_str(), video.c_str());
            });
        },
        [] {},
        [](const ui::Stream &stream, const std::string &title) {
            // The player is the next thing to be built; for now the choice is recorded.
            log_line("play requested: \"%s\" from %s", title.c_str(), stream.addon.c_str());
        },
    });

    if (stremio_core_init(kStorageFolder) != 0)
    {
        char error[256] = {};
        stremio_core_last_error(error, sizeof error);
        log_line("core did not start: %s", error);
    }
    else
    {
        log_line("core started");
        core.start();
        core.post([] {
            // Downloaded artwork is kept between runs up to this size; the oldest goes first.
            constexpr std::uint64_t kImageFolderLimit = std::uint64_t{400} << 20;
            const std::size_t trimmed = stremio_core_trim_folder(kImageFolder, kImageFolderLimit);
            log_line("image folder: removed %zu old files", trimmed);
        });
    }

    ps5::Pad pad;
    log_line("controller %s", pad.open() ? "ready" : "not available");

    double previous = seconds_now();
    double slow_logged = 0;
    unsigned long frames = 0;
    for (;;)
    {
        const double frame_start = seconds_now();

        // Whatever the core's thread has ready is applied between frames.
        std::vector<ui::BoardRow> rows;
        if (core.take_board(rows))
            app.set_board(std::move(rows));
        ui::Details details;
        if (core.take_details(details))
            app.set_details(std::move(details));
        ui::Account account;
        if (core.take_account(account))
            app.set_account(std::move(account));
        core.set_focused_catalog(app.focused_catalog());
        core.set_title_open(app.title_open());

        const double now = seconds_now();
        const float elapsed = static_cast<float>(now - previous);
        previous = now;
        pad.poll(elapsed, [&](ui::Button button) { app.press(button); });
        const double applied = seconds_now();

        canvas.begin();
        app.update(elapsed);
        app.draw(width, height);
        const double drawn = seconds_now();
        canvas.show(display.width, display.height);
        if (!display.present())
        {
            log_line("present failed (0x%x)", eglGetError());
            break;
        }
        const double presented = seconds_now();

        // A frame that takes much longer than the screen's 16.7 ms is a visible stutter;
        // each is reported (at most once a second) with where its time went.
        if (presented - frame_start > 0.030 && frames > 120 && presented - slow_logged > 1.0)
        {
            slow_logged = presented;
            log_line("slow frame: %.0f ms (data and input %.1f, drawing %.1f, presenting %.1f)",
                     (presented - frame_start) * 1e3, (applied - frame_start) * 1e3,
                     (drawn - applied) * 1e3, (presented - drawn) * 1e3);
        }

        if (++frames == 1 || frames == 120 || frames % 3600 == 0)
        {
            std::size_t in_use = 0, peak = 0, mapped = 0;
            app_heap_stats(&in_use, &peak, &mapped);
            log_line("frame %lu, gl error 0x%x, heap %zu MiB in use, %zu MiB peak", frames,
                     glGetError(), in_use >> 20, peak >> 20);
        }
    }
    return 0;
}
