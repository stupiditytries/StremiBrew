// The video player's controls, drawn over the picture (which the host draws itself): the
// title at the top, and along the bottom a seek bar with the time and a play/pause mark.
// They fade away while a video plays untouched and return with any button.
//
// Left and right scrub: each press moves a marker along the bar, and the video jumps to
// it once the presses stop, so a long jump is one seek and not many.

#pragma once

#include <functional>
#include <string>

struct NVGcontext;

namespace ui
{
enum class Button;

// What the host's player is doing.
struct Playback
{
    enum class State
    {
        Opening, // nothing to show yet
        Playing,
        Paused,
        Ended,
        Failed,
    };
    State state = State::Opening;
    double position = 0; // seconds
    double duration = 0; // seconds; 0 when not known
    bool buffering = false; // playing, but waiting for more of the video to arrive
    std::string error;      // why it failed
};

// What the controls ask of the host's player.
struct PlayerHandler
{
    std::function<void(bool paused)> set_paused;
    std::function<void(double seconds)> seek;
    std::function<void()> close;
};

class PlayerScreen
{
  public:
    explicit PlayerScreen(NVGcontext *context);

    void set_handler(PlayerHandler handler);
    void open(const std::string &title);
    void set_playback(const Playback &playback);
    // Returns false for a press that leaves the player (Back).
    bool press(Button button);
    void update(float seconds);
    void draw();

  private:
    void commit_scrub();
    void draw_spinner(float x, float y, float radius, float alpha);

    NVGcontext *vg_;
    PlayerHandler handler_;
    std::string title_;
    Playback playback_;
    float controls_ = 1; // 0..1, how visible the controls are
    float idle_ = 0;     // seconds since the last press
    float spin_ = 0;     // the busy mark's angle
    float busy_ = 0;     // 0..1, how visible the busy mark is
    // Scrubbing: where the marker has been moved to, how long since it last moved, and how
    // many presses in a row (the steps grow).
    bool scrubbing_ = false;
    double scrub_target_ = 0;
    float scrub_idle_ = 0;
    int scrub_presses_ = 0;
};
} // namespace ui
