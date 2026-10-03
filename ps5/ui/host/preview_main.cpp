// The app's screens on a PC, in a window or straight to a PNG, with sample data.
//
//   stremio_preview <board.json> <image cache folder> <font folder> [options]
//     --shot <file.png>   draw without showing a window, save the last frame and exit
//     --keys <letters>    button presses to apply first: u d l r (directions), a (accept),
//                         b (back); the screen settles between presses
//
// In a window the arrow keys, Enter and Backspace are the buttons.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
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
#include "theme.hpp"

namespace
{
ui::App *app = nullptr;

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
        glfwSetKeyCallback(window, on_key);

        const auto frame = [&](float seconds) {
            int framebuffer_width = 0, framebuffer_height = 0;
            glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
            glViewport(0, 0, framebuffer_width, framebuffer_height);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            instance.update(seconds);
            instance.draw(framebuffer_width, framebuffer_height);
        };

        if (shot != nullptr)
        {
            // Offscreen: apply the presses, giving animations and image loading time to
            // settle after each, then save what the last frame drew.
            const auto settle = [&] {
                for (int count = 0; count < 45; ++count)
                    frame(1.0f / 60.0f);
            };
            settle();
            for (const char letter : keys)
            {
                ui::Button button;
                if (button_for(letter, button))
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
