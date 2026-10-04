// The controller, as UI button presses: the directional buttons and the left stick move
// the focus (repeating while held), Cross accepts and Circle goes back.

#pragma once

#include <cstdint>
#include <functional>

#include "app.hpp"

namespace ps5
{
class Pad
{
  public:
    // Opens the first signed-in user's controller. Returns false when there is none.
    bool open();
    // Reads the controller and calls `press` for each button press that happened since
    // the last call, including repeats of a held direction. `seconds` is the time since
    // the last call.
    void poll(float seconds, const std::function<void(ui::Button)> &press);
    // How far through being held the triangle button is, 0 to 1. Held all the way it
    // sends Button::Calibrate, once, and this goes back to 0 until it is let go.
    float hold_progress() const;

  private:
    int handle_ = -1;
    std::uint32_t held_ = 0;      // directions and buttons down at the last poll
    std::uint32_t repeating_ = 0; // the direction being repeated, if any
    float repeat_in_ = 0;         // seconds until its next repeat
    float triangle_for_ = 0;      // seconds the triangle has been held
    bool triangle_sent_ = false;  // its hold has been acted on
};
} // namespace ps5
