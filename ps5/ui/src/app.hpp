// The app's screens. One object owns the focus, the animations and the drawing; the host
// (the console's main loop or the PC preview) feeds it button presses and time and asks it
// to draw each frame.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "board_data.hpp"

struct NVGcontext;

namespace ui
{
class Images;

enum class Button
{
    Up,
    Down,
    Left,
    Right,
    Accept,
    Back,
};

class App
{
  public:
    // `font_folder` holds the Plus Jakarta Sans files; `image_folder` is the image cache.
    App(NVGcontext *context, const std::string &font_folder, const std::string &image_folder);
    ~App();

    void set_board(std::vector<BoardRow> rows);
    void press(Button button);
    void update(float seconds);
    // Draws one frame into a framebuffer of the given size in pixels.
    void draw(int width, int height);

  private:
    enum class Zone
    {
        Navigation,
        Search,
        Rows,
    };

    void draw_navigation();
    void draw_top_bar();
    void draw_rows();
    void draw_row(const BoardRow &row, std::size_t index, float top);
    float row_height(const BoardRow &row) const;
    float card_width(const BoardRow &row) const;
    float poster_height(const BoardRow &row) const;
    float row_top(std::size_t index) const;
    void follow_focus();

    NVGcontext *vg_;
    std::unique_ptr<Images> images_;
    std::vector<BoardRow> rows_;

    Zone zone_ = Zone::Rows;
    int navigation_focus_ = 0; // which navigation button the focus is on
    int selected_tab_ = 0;     // which screen is shown
    std::size_t row_focus_ = 0;
    std::vector<std::size_t> column_focus_; // focused card in each row

    // Scroll positions ease towards their targets every frame.
    float scroll_y_ = 0, scroll_y_target_ = 0;
    std::vector<float> scroll_x_, scroll_x_target_;
    float focus_pulse_ = 0; // 0..1, eases to 1 after the focus moves
};
} // namespace ui
