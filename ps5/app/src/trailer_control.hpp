// Trailers on the home screen: when the focus rests on a title its trailer is found and
// opened (so it is ready), and after the UI's dwell it plays in the featured area in place
// of the artwork, quietly. The picture is drawn into a texture of its own, which the UI
// draws as it would any image.

#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "app.hpp"
#include "player.hpp"

struct NVGcontext;

namespace ps5
{
class TrailerControl
{
  public:
    TrailerControl(ui::App &app, NVGcontext *vg);

    // What the UI calls as the focus rests on a title, has rested long enough, and moves on.
    ui::TrailerHandler handler();
    // Once a frame, before the screen's own drawing begins.
    void frame();

  private:
    struct Shared; // what the lookup thread leaves for the drawing thread

    bool create_target();

    ui::App &app_;
    NVGcontext *vg_;
    Player player_;
    std::shared_ptr<Shared> shared_;
    int ticket_ = 0;        // the lookup whose answer is wanted
    bool wanted_ = false;   // a title has the focus and its trailer is being got ready
    bool started_ = false;  // the UI has asked for it to play
    bool opened_ = false;   // the player has been given the trailer
    bool playing_ = false;  // ... and told to play it
    std::chrono::steady_clock::time_point opened_at_{};
    unsigned framebuffer_ = 0, texture_ = 0;
    int image_ = 0;
};
} // namespace ps5
