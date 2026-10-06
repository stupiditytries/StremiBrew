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
#include "stb_image.h"

#include "../../../ps5/app/src/black_bars.hpp"

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

// A picture standing in for a trailer (see --trailer).
std::string trailer_picture;

char calibration_mock = 0; // l listening, w working, d done, f failed

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
    case 'o':
        button = ui::Button::Options;
        return true;
    case 'p':
        button = ui::Button::SkipBack;
        return true;
    case 'n':
        button = ui::Button::SkipForward;
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
        else if (option == "--calibrate")
        {
            // A calibration notice mock-up: "a" or "b" for the look, then the state.
            const std::string look = argv[index + 1];
            calibration_mock = look.size() > 1 ? look[1] : 'l';
        }
        else if (option == "--trailer")
        {
            // A still that stands in for a playing trailer: a picture of what the
            // console's trailer texture would hold (the video fitted into 1920x1080).
            trailer_picture = argv[index + 1];
        }
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
    static std::string board_json;
    board_json = json.str();
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
            [] {},
            [](double) {},
        });
        static ui::SpeechModels speech;
        speech.ready[1] = true;
        if (calibration_mock == 'p')
        {
            // The download mock-up: the first model part-way down.
            speech.downloading = 0;
            speech.progress = 43;
        }
        instance.set_speech_models(speech);
        instance.set_speech_handlers(
            [&instance](int model) {
                speech.chosen = model;
                instance.set_speech_models(speech);
            },
            [](int) {});
        instance.set_library_handler([](ui::LibraryAction action, const ui::BoardItem &item) {
            std::printf("library action %d on %s\n", static_cast<int>(action), item.name.c_str());
        });
        // The console's keyboard is stood in for by one that has already typed something.
        instance.set_keyboard_handler([&instance](const std::string &) {
            instance.submit_search("breaking bad");
            return true;
        });
        // A sample month for the calendar, with the board's posters, and sample add-ons.
        static ui::CalendarMonth month;
        const auto fill_month = [](int year, int number) {
            static const int kLengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            month = ui::CalendarMonth{};
            month.year = year;
            month.month = number;
            month.days = kLengths[number - 1];
            month.first_weekday = (number * 3 + year) % 7;
            month.today = number == 10 ? 7 : 0;
            std::vector<ui::BoardRow> rows;
            ui::parse_board(board_json, rows);
            std::vector<ui::BoardItem> series;
            for (const ui::BoardRow &row : rows)
                for (const ui::BoardItem &item : row.items)
                    if (item.type == "series")
                        series.push_back(item);
            std::size_t next = static_cast<std::size_t>(number);
            for (const int day : {2, 5, 7, 8, 12, 13, 16, 21, 22, 27, 30})
            {
                ui::CalendarDay entry;
                entry.day = day;
                const int count = day == 7 ? 5 : day % 3 == 0 ? 2 : 1;
                for (int index = 0; index < count && !series.empty(); ++index, ++next)
                {
                    const ui::BoardItem &item = series[next % series.size()];
                    entry.items.push_back({item.id, item.type, item.name, item.poster, item.id + ":2:" + std::to_string(day),
                                           "Episode", 2, day % 10 + 1});
                }
                month.items.push_back(std::move(entry));
            }
        };
        fill_month(2026, 10);
        instance.set_calendar(month);
        instance.set_calendar_handler([&instance, fill_month](int year, int number) {
            if (year != 0)
                fill_month(year, number);
            instance.set_calendar(month);
        });
        // Discover: the board's rows stand in for catalogs.
        static ui::DiscoverData discover;
        static int discover_catalog = 0;
        const auto fill_discover = [] {
            std::vector<ui::BoardRow> rows;
            ui::parse_board(board_json, rows);
            discover = ui::DiscoverData{};
            discover.types = {{"Movie", discover_catalog % 2 == 0}, {"Series", discover_catalog % 2 == 1},
                              {"Channel", false}, {"TV", false}};
            discover.catalogs = {{"Popular", true}, {"Featured", false}, {"New", false}};
            for (const char *genre : {"All", "Action", "Adventure", "Animation", "Biography", "Comedy", "Crime",
                                      "Documentary", "Drama", "Family", "Fantasy", "History", "Horror", "Mystery"})
                discover.genres.push_back({genre, discover.genres.empty()});
            discover.more = false;
            for (std::size_t row = static_cast<std::size_t>(discover_catalog) % 2; row < rows.size(); row += 2)
                for (const ui::BoardItem &item : rows[row].items)
                    discover.items.push_back(item);
        };
        instance.set_discover_handler([&instance, fill_discover](int kind, int index) {
            if (kind == 0)
                discover_catalog = index;
            if (kind != 3)
            {
                fill_discover();
                instance.set_discover(discover);
            }
        });
        instance.set_addons({
            {"Cinemeta", "3.0.13", "The official addon for movie and series catalogs", "", "v3-cinemeta.strem.io",
             {"movie", "series"}, {"catalog", "meta", "addon_catalog"}, true},
            {"OpenSubtitles v3", "1.0.0", "OpenSubtitles v3 Addon for Stremio", "", "opensubtitles-v3.strem.io",
             {"movie", "series"}, {"subtitles"}, true},
            {"Torrentio", "0.0.15", "Provides torrent streams from scraped torrent providers. Currently supports "
             "YTS, EZTV, RARBG, 1337x, ThePirateBay and others.", "", "torrentio.strem.fun",
             {"movie", "series", "anime"}, {"stream"}, false},
            {"Local Files", "1.10.0", "Local add-on to find playable files on this device", "", "127.0.0.1",
             {"movie", "series", "other"}, {"catalog", "meta", "stream"}, true},
            {"WatchHub", "0.0.4", "Find where to stream your favourite movies and shows amongst Netflix, HBO, "
             "Hulu and more", "", "watchhub.strem.io", {"movie", "series"}, {"stream"}, true},
            {"Public Domain Movies", "0.0.1", "Movies in the public domain", "", "caching.stremio.net",
             {"movie"}, {"catalog", "stream"}, false},
            {"YouTube", "1.4.2", "Watch your favourite YouTube channels ad-free", "", "v3-channels.strem.io",
             {"channel"}, {"catalog", "meta", "stream"}, true},
        });
        instance.set_languages_handler([](const std::string &audio, const std::string &subtitles) {
            std::printf("languages: %s / %s\n", audio.c_str(), subtitles.c_str());
        });
        // The stand-in trailer is made the way the console makes the real one: a texture
        // whose first row is its bottom one, handed to the UI as an image flagged as
        // upside down; and its black bars are found by the player's own bar finder.
        int trailer_image = 0;
        float trailer_top = 0, trailer_bottom = 0;
        if (!trailer_picture.empty())
        {
            int width = 0, height = 0, channels = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char *pixels = stbi_load(trailer_picture.c_str(), &width, &height, &channels, 4);
            if (pixels != nullptr)
            {
                std::vector<unsigned char> luma(static_cast<std::size_t>(width) * height);
                for (std::size_t index = 0; index < luma.size(); ++index)
                    luma[index] = static_cast<unsigned char>(
                        (pixels[index * 4] * 54 + pixels[index * 4 + 1] * 183 + pixels[index * 4 + 2] * 19) >> 8);
                const ps5::BarSample sample = ps5::measure_bars(luma.data(), width, width, height, 1, 8, false);
                ps5::decide_bars(&sample, 1, trailer_top, trailer_bottom);
                std::printf("trailer picture %dx%d: bars %.1f%% top, %.1f%% bottom\n", width, height,
                            trailer_top * 100, trailer_bottom * 100);
                // Rows bottom-up, as a texture that has been drawn into.
                std::vector<unsigned char> turned(static_cast<std::size_t>(width) * height * 4);
                for (int row = 0; row < height; ++row)
                    std::copy(pixels + static_cast<std::size_t>(row) * width * 4,
                              pixels + static_cast<std::size_t>(row + 1) * width * 4,
                              turned.begin() + static_cast<std::ptrdiff_t>(height - 1 - row) * width * 4);
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, turned.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D, 0);
                trailer_image = nvglCreateImageFromHandleGL3(vg, texture, width, height,
                                                             NVG_IMAGE_FLIPY | NVG_IMAGE_NODELETE);
                stbi_image_free(pixels);
            }
        }
        glfwSetKeyCallback(window, on_key);

        const auto frame = [&](float seconds) {
            int framebuffer_width = 0, framebuffer_height = 0;
            glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
            glViewport(0, 0, framebuffer_width, framebuffer_height);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            if (playback.state == ui::Playback::State::Playing)
                playback.position += seconds;
            if (calibration_mock != 0 && calibration_mock != 'p')
            {
                playback.subtitle.clear();
                playback.calibration.progress = 0.62f;
                playback.calibration.delay = 1.25;
                playback.calibration.message = "No dialogue matched. Try again while someone is speaking.";
                playback.calibration.state = calibration_mock == 'w'   ? ui::Calibration::State::Working
                                             : calibration_mock == 'd' ? ui::Calibration::State::Done
                                             : calibration_mock == 'f' ? ui::Calibration::State::Failed
                                                                       : ui::Calibration::State::Listening;
            }
            instance.set_playback(playback);
            instance.set_player_tracks(tracks);
            if (trailer_image != 0)
                instance.set_trailer(trailer_image, true, trailer_top, trailer_bottom);
            {
                // Sample rows stand in for whatever view the screen has moved to.
                static ui::View served = ui::View::Board;
                static std::string served_query;
                if (instance.view() != served || instance.search_query() != served_query)
                {
                    served = instance.view();
                    served_query = instance.search_query();
                    instance.update(0.0f);
                    std::vector<ui::BoardRow> again;
                    ui::parse_board(board_json, again);
                    if (served == ui::View::Search && again.size() > 2)
                    {
                        again.erase(again.begin());
                        again.resize(2);
                        again[0].title = "Movies";
                        again[1].title = "Series";
                    }
                    instance.set_board(std::move(again));
                }
            }
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
                if (letter == 'S')
                    account.link = ui::Account::Link::Waiting; // signed in without the steps
                if (letter == 's' || letter == 'S')
                    complete_sign_in();
                else if (button_for(letter, button))
                    instance.press(button);
                settle();
            }
            if (trailer_image != 0)
                for (int wait = 0; wait < 6; ++wait)
                    settle();
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
