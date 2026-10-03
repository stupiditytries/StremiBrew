// The app's screens on a PC, in a window or straight to a PNG, with sample data.
//
//   stremio_preview <board.json> <image cache folder> <font folder> [options]
//     --shot <file.png>   draw without showing a window, save the last frame and exit
//     --keys <letters>    button presses to apply first: u d l r (directions), a (accept),
//                         b (back), s (the sign-in code gets entered elsewhere);
//                         the screen settles between presses
//
// In a window the arrow keys, Enter and Backspace are the buttons, and S plays the part
// of the sign-in code being entered on another device.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "glad/glad.h"

#include <GLFW/glfw3.h>

#define NANOVG_GL3_IMPLEMENTATION
#include "nanovg.h"
#include "nanovg_gl.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "app.hpp"
#include "board_data.hpp"
#include "details_screen.hpp"
#include "theme.hpp"

namespace
{
ui::App *app = nullptr;
ui::Account account;

// What the console's host does with the core, acted out with canned data.
void on_intent(ui::Intent intent)
{
    switch (intent)
    {
    case ui::Intent::SignIn:
        account.link = ui::Account::Link::Waiting;
        account.code = "7K2P";
        account.link_page = "https://link.stremio.com/7K2P";
        break;
    case ui::Intent::CancelSignIn:
        account.link = ui::Account::Link::Idle;
        break;
    case ui::Intent::SignOut:
        account = ui::Account{};
        account.addons = 6;
        account.audio_language = "eng";
        account.subtitles_language = "eng";
        break;
    }
    app->set_account(account);
}

std::string data_folder; // where the sample files are

std::string read_text(const std::string &path)
{
    std::ifstream file{path, std::ios::binary};
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// A title's page shows the sample film or the sample series, whichever kind was opened.
void show_sample_details(const std::string &type, const std::string &id)
{
    ui::Details details;
    const std::string file = type == "series" ? "details-series.json" : "details-movie.json";
    if (ui::parse_details(read_text(data_folder + "/" + file), details))
    {
        // The sample is of one particular title; it is passed off as the one opened.
        details.id = id;
        app->set_details(std::move(details));
    }
}

// Plays the part of the code being entered on another device.
void complete_sign_in()
{
    if (account.link != ui::Account::Link::Waiting)
        return;
    account = ui::Account{};
    account.signed_in = true;
    account.email = "you@example.com";
    account.addons = 14;
    app->set_account(account);
}

void on_key(GLFWwindow *window, int key, int, int action, int)
{
    if (action != GLFW_PRESS && action != GLFW_REPEAT)
        return;
    switch (key)
    {
    case GLFW_KEY_UP:
        app->press(ui::Button::Up);
        break;
    case GLFW_KEY_DOWN:
        app->press(ui::Button::Down);
        break;
    case GLFW_KEY_LEFT:
        app->press(ui::Button::Left);
        break;
    case GLFW_KEY_RIGHT:
        app->press(ui::Button::Right);
        break;
    case GLFW_KEY_ENTER:
        app->press(ui::Button::Accept);
        break;
    case GLFW_KEY_BACKSPACE:
        app->press(ui::Button::Back);
        break;
    case GLFW_KEY_S:
        complete_sign_in();
        break;
    case GLFW_KEY_ESCAPE:
        glfwSetWindowShouldClose(window, GLFW_TRUE);
        break;
    default:
        break;
    }
}

bool button_for(char letter, ui::Button &button)
{
    switch (letter)
    {
    case 'u':
        button = ui::Button::Up;
        return true;
    case 'd':
        button = ui::Button::Down;
        return true;
    case 'l':
        button = ui::Button::Left;
        return true;
    case 'r':
        button = ui::Button::Right;
        return true;
    case 'a':
        button = ui::Button::Accept;
        return true;
    case 'b':
        button = ui::Button::Back;
        return true;
    default:
        return false;
    }
}

void save_png(const char *path, int width, int height)
{
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    // OpenGL's rows run bottom to top.
    stbi_flip_vertically_on_write(1);
    for (std::size_t index = 3; index < pixels.size(); index += 4)
        pixels[index] = 255;
    if (!stbi_write_png(path, width, height, 4, pixels.data(), width * 4))
        std::fprintf(stderr, "could not write %s\n", path);
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <board.json> <image cache> <fonts> [--shot f] [--keys k]\n",
                     argv[0]);
        return 2;
    }
    const char *shot = nullptr;
    std::string keys;
    for (int index = 4; index + 1 < argc; index += 2)
    {
        const std::string option = argv[index];
        if (option == "--shot")
            shot = argv[index + 1];
        else if (option == "--keys")
            keys = argv[index + 1];
        else if (option == "--backdrop")
        {
            // soft, light or sharp: how a title page treats its artwork.
            const std::string style = argv[index + 1];
            ui::set_backdrop(style == "soft"    ? ui::Backdrop::Soft
                             : style == "sharp" ? ui::Backdrop::Sharp
                                                : ui::Backdrop::Light);
        }
    }

    std::ifstream file{argv[1], std::ios::binary};
    std::stringstream json;
    json << file.rdbuf();
    std::vector<ui::BoardRow> rows;
    if (!ui::parse_board(json.str(), rows))
        std::fprintf(stderr, "%s is not a board; showing an empty one\n", argv[1]);

    if (!glfwInit())
        return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
    glfwWindowHint(GLFW_VISIBLE, shot != nullptr ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
    const int width = static_cast<int>(ui::theme::kScreenWidth);
    const int height = static_cast<int>(ui::theme::kScreenHeight);
    // A window shows the screen at half size; a screenshot is drawn at full size.
    GLFWwindow *window = glfwCreateWindow(shot != nullptr ? width : width / 2,
                                          shot != nullptr ? height : height / 2,
                                          "Stremio PS5 preview", nullptr, nullptr);
    if (window == nullptr)
        return 1;
    glfwMakeContextCurrent(window);
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
        return 1;
    glfwSwapInterval(1);

    NVGcontext *vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (vg == nullptr)
        return 1;
    {
        ui::App instance{vg, argv[3], argv[2]};
        app = &instance;
        instance.set_board(std::move(rows));
        account.addons = 6;
        account.audio_language = "eng";
        account.subtitles_language = "eng";
        instance.set_account(account);
        instance.set_intent_handler(on_intent);
        data_folder = std::string{argv[1]};
        data_folder.erase(data_folder.find_last_of("/\\") == std::string::npos
                              ? 0
                              : data_folder.find_last_of("/\\"));
        instance.set_title_handler({
            [](const std::string &type, const std::string &id) { show_sample_details(type, id); },
            [](const std::string &type, const std::string &id, const std::string &) {
                show_sample_details(type, id);
            },
            [] {},
            [](const ui::Stream &stream, const std::string &title, const std::string &,
               const std::string &, const std::string &video) {
                std::printf("play \"%s\" (%s): %s\n", title.c_str(), video.c_str(), stream.url.c_str());
            },
        });
        // A stand-in player: no picture, but the controls behave.
        static ui::Playback playback;
        playback = ui::Playback{};
        playback.state = ui::Playback::State::Playing;
        playback.position = 754.0;
        playback.duration = 2940.0;
        playback.subtitle = "You see, technically, chemistry\nis the study of matter.";
        static ui::PlayerTracks tracks;
        tracks.audio = {{"English", "E-AC-3 5.1"}, {"Spanish", "AAC stereo"}, {"French", "AC-3 5.1"}};
        tracks.audio_selected = 0;
        tracks.subtitles = {{"Off", ""},
                            {"English", "In the video"},
                            {"English", "OpenSubtitles 1"},
                            {"English", "OpenSubtitles 2"},
                            {"Spanish", "OpenSubtitles 1"},
                            {"French", "OpenSubtitles 1"}};
        tracks.subtitle_selected = 1;
        instance.set_player_handler({
            [](bool paused) {
                playback.state = paused ? ui::Playback::State::Paused : ui::Playback::State::Playing;
            },
            [](double seconds) { playback.position = seconds; },
            [] {},
            [](int index) { tracks.audio_selected = index; },
            [](int index) { tracks.subtitle_selected = index; },
            [](double seconds) { tracks.subtitle_delay = seconds; },
            [](double) {},
        });
        instance.set_languages_handler([](const std::string &audio, const std::string &subtitles) {
            std::printf("languages: %s / %s\n", audio.c_str(), subtitles.c_str());
        });
        glfwSetKeyCallback(window, on_key);

        const auto frame = [&](float seconds) {
            int framebuffer_width = 0, framebuffer_height = 0;
            glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
            glViewport(0, 0, framebuffer_width, framebuffer_height);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            if (playback.state == ui::Playback::State::Playing)
                playback.position += seconds;
            instance.set_playback(playback);
            instance.set_player_tracks(tracks);
            instance.update(seconds);
            instance.draw(framebuffer_width, framebuffer_height);
        };

        if (shot != nullptr)
        {
            // Offscreen: apply the presses, giving animations and image loading time to
            // settle after each, then save what the last frame drew.
            const auto settle = [&] {
                for (int count = 0; count < 45; ++count)
                {
                    frame(1.0f / 60.0f);
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
            };
            settle();
            for (const char letter : keys)
            {
                ui::Button button;
                if (letter == 's')
                    complete_sign_in();
                else if (button_for(letter, button))
                    instance.press(button);
                settle();
            }
            int framebuffer_width = 0, framebuffer_height = 0;
            glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
            glFinish();
            save_png(shot, framebuffer_width, framebuffer_height);
        }
        else
        {
            double previous = glfwGetTime();
            while (!glfwWindowShouldClose(window))
            {
                const double now = glfwGetTime();
                frame(static_cast<float>(now - previous));
                previous = now;
                glfwSwapBuffers(window);
                glfwPollEvents();
            }
        }
        app = nullptr;
    }
    nvgDeleteGL3(vg);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
