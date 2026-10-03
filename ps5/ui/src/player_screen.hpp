// The video player's controls, drawn over the picture (which the host draws itself): the
// title at the top; along the bottom a seek bar and, under it, play or pause, back and
// forward 15 seconds and the time on the left, audio track and subtitles on the right.
// They fade away while a video plays untouched and return with any button. Subtitles are
// drawn here too.
//
// On the seek bar, left and right scrub: each press moves a marker along the bar, with a
// small picture of that moment above it, and the video jumps there once the presses stop,
// so a long jump is one seek and not many. Down goes to the buttons, up back to the bar.
// The audio and subtitles buttons open a list at the right of the screen.

#pragma once

#include <functional>
#include <string>
#include <vector>

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
    std::string subtitle;   // the subtitle to show now; lines separated by '\n'
    // A small picture of the video at `preview_time` (a drawing-library image; 0 for none),
    // made at the controls' request while scrubbing.
    int preview_image = 0;
    double preview_time = 0;
};

// One choice in the audio or subtitles list.
struct TrackOption
{
    std::string label;  // the language, or "Off"
    std::string detail; // what else tells it apart: format, channels, where it is from
};

struct PlayerTracks
{
    std::vector<TrackOption> audio, subtitles;
    int audio_selected = -1;
    int subtitle_selected = 0;
    bool subtitles_loading = false; // add-ons are still being asked
};

// What the controls ask of the host's player.
struct PlayerHandler
{
    std::function<void(bool paused)> set_paused;
    std::function<void(double seconds)> seek;
    std::function<void()> close;
    std::function<void(int index)> choose_audio;
    std::function<void(int index)> choose_subtitle;
    // Make a small picture of the video at this time (see Playback::preview_image).
    std::function<void(double seconds)> preview;
};

class PlayerScreen
{
  public:
    explicit PlayerScreen(NVGcontext *context);

    void set_handler(PlayerHandler handler);
    void open(const std::string &title);
    void set_playback(const Playback &playback);
    void set_tracks(PlayerTracks tracks);
    // Returns false for a press that leaves the player (Back).
    bool press(Button button);
    void update(float seconds);
    void draw();

  private:
    enum class Control
    {
        Play,
        Back,
        Forward,
        Audio,
        Subtitles,
    };
    static constexpr int kControls = 5;
    enum class Menu
    {
        None,
        Audio,
        Subtitles,
    };

    void commit_scrub();
    void activate(Control control);
    void skip(double seconds);
    const std::vector<TrackOption> &menu_options() const;
    int menu_selected() const;
    void draw_spinner(float x, float y, float radius, float alpha);
    void draw_control(Control control, float x, float y, float size, bool focused, float alpha);
    void draw_scrub_preview(float thumb_x, float bar_top, float alpha);
    void draw_subtitle();
    void draw_menu();

    NVGcontext *vg_;
    PlayerHandler handler_;
    std::string title_;
    Playback playback_;
    PlayerTracks tracks_;
    float controls_ = 1; // 0..1, how visible the controls are
    float idle_ = 0;     // seconds since the last press
    float spin_ = 0;     // the busy mark's angle
    float busy_ = 0;     // 0..1, how visible the busy mark is
    // The focus is on the seek bar or on one of the buttons.
    bool on_buttons_ = false;
    int button_ = 0;
    // Scrubbing: where the marker has been moved to, how long since it last moved, and how
    // many presses in a row (the steps grow).
    bool scrubbing_ = false;
    double scrub_target_ = 0;
    float scrub_idle_ = 0;
    int scrub_presses_ = 0;
    double preview_asked_ = -1; // the time a preview picture was last asked for
    // The list of audio tracks or subtitles.
    Menu menu_ = Menu::None;
    Menu menu_shown_ = Menu::None; // the one being drawn (it outlives menu_ while it slides out)
    float menu_slide_ = 0;         // 0..1, how far in it is
    int menu_focus_ = 0;
    float menu_scroll_ = 0, menu_scroll_target_ = 0;
};
} // namespace ui
