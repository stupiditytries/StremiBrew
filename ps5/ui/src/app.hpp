// The app's screens. One object owns the focus, the animations and the drawing; the host
// (the console's main loop or the PC preview) feeds it button presses and time and asks it
// to draw each frame.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "account_data.hpp"
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

// Things the user asks for that the host carries out.
enum class Intent
{
    SignIn,       // start signing in (ask for a link code)
    CancelSignIn, // abandon the sign-in in progress
    SignOut,
};

class App
{
  public:
    // `font_folder` holds the Plus Jakarta Sans files; `image_folder` is the image cache.
    App(NVGcontext *context, const std::string &font_folder, const std::string &image_folder);
    ~App();

    void set_board(std::vector<BoardRow> rows);
    // How images that are not in the cache folder yet are downloaded (see Images).
    void set_image_fetcher(
        std::function<void(const std::string &address, const std::string &file)> fetch,
        std::function<bool(const std::string &address)> failed);
    void set_account(Account account);
    void set_intent_handler(std::function<void(Intent)> handler);
    // The board row the focus is on, so the host can load rows ahead of it.
    std::size_t focused_row() const
    {
        return row_focus_;
    }
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
        Content, // the one button of a screen that is not the board
    };

    void draw_navigation();
    void draw_top_bar();
    void draw_rows();
    void draw_hero();
    const BoardItem *focused_item() const;
    void draw_row(const BoardRow &row, std::size_t index, float top);
    float row_height(const BoardRow &row) const;
    float card_width(const BoardRow &row) const;
    float poster_height(const BoardRow &row) const;
    float row_top(std::size_t index) const;
    void follow_focus();
    void enter_tab();
    void draw_settings();
    void draw_unbuilt_tab();

    NVGcontext *vg_;
    std::unique_ptr<Images> images_;
    std::vector<BoardRow> rows_;
    Account account_;
    std::function<void(Intent)> intent_;

    Zone zone_ = Zone::Rows;
    int navigation_focus_ = 0; // which navigation button the focus is on
    int selected_tab_ = 0;     // which screen is shown
    std::size_t row_focus_ = 0;
    std::vector<std::size_t> column_focus_; // focused card in each row

    // Scroll positions ease towards their targets every frame.
    float scroll_y_ = 0, scroll_y_target_ = 0;
    std::vector<float> scroll_x_, scroll_x_target_;
    float focus_pulse_ = 0; // 0..1, eases to 1 after the focus moves
    // The featured area: the item it shows (a copy, so a board refresh cannot pull it
    // away), the item the focus is on, how long the focus has rested there, and how
    // visible its text, title logo and background artwork each are.
    BoardItem hero_;
    bool hero_valid_ = false;
    std::string hero_target_;
    float hero_dwell_ = 0;
    float hero_alpha_ = 0;
    float hero_logo_alpha_ = 0;
    float hero_art_alpha_ = 0;
    float frame_seconds_ = 0; // the last update's time step, for fades advanced while drawing
    // How visible each navigation button's label and highlight are, 0..1. A tab's name
    // shows only while the focus is on it, and fades in and out as the focus moves.
    static constexpr int kMaxTabs = 8;
    float navigation_reveal_[kMaxTabs] = {};
};
} // namespace ui
