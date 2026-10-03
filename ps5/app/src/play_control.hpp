// What happens around the player while a video plays: it starts the player where the
// video was left, offers the audio tracks and subtitles (the video's own and the ones the
// account's add-ons have), keeps the scrubbing picture, and tells the core how far
// playback has got so the library's progress follows.
//
// The audio track, the subtitles and their delay chosen for a title are remembered (in a
// small file of the app's own) and applied the next time anything of that title is played,
// so a choice made for one episode holds for the rest of the series.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app.hpp"
#include "core_link.hpp"
#include "player.hpp"

struct NVGcontext;

namespace ps5
{
class PlayControl
{
  public:
    PlayControl(ui::App &app, CoreLink &core, NVGcontext *vg);

    // Plays a stream of the title `id`; `video` is the episode's id, or the film's own.
    void start(const ui::Stream &stream, const std::string &type, const std::string &id,
               const std::string &video);
    // What the player's controls call.
    ui::PlayerHandler handler();
    // Once a frame, with the framebuffer to draw into bound and before the UI is drawn:
    // draws the picture and brings the UI up to date.
    void frame(int width, int height);

  private:
    // A subtitle an add-on offers.
    struct External
    {
        std::string addon, language, url;
    };
    // What was last chosen for a title.
    struct Remembered
    {
        std::string audio_language, audio_detail; // empty when no audio track was chosen
        // Subtitles: 0 nothing chosen, 1 off, 2 one of the video's own, 3 an add-on's.
        int subtitles = 0;
        std::string subtitle_language;
        int delay = 0; // milliseconds
    };
    // What the background fetches leave for the drawing thread. A new video gets a new
    // one, so a fetch that finishes late leaves its result where nobody looks.
    struct Shared;

    void choose_subtitle(int index);
    Remembered recall(const std::string &title) const;
    void remember();
    void show_preview(ui::Playback &status);
    void rebuild_tracks();
    void close();

    ui::App &app_;
    CoreLink &core_;
    NVGcontext *vg_;
    Player player_;
    std::shared_ptr<Shared> shared_;
    std::string subtitle_language_;
    std::vector<TrackInfo> embedded_;
    std::vector<External> externals_;
    bool tracks_known_ = false, externals_known_ = false;
    int subtitle_selected_ = 0;
    double subtitle_delay_ = 0;
    bool subtitle_chosen_ = false; // by the user or automatically; stops later automatic picks
    std::string title_;       // the title playing, which choices are remembered under
    Remembered remembered_;   // what is on file for it
    bool audio_applied_ = false;
    // The scrubbing picture: the texture it is in and the image the UI draws it as, which
    // of the player's pictures it holds, and the moment the UI last asked about.
    unsigned preview_texture_ = 0;
    int preview_image_ = 0, preview_width_ = 0, preview_height_ = 0;
    int preview_index_ = -1;
    double preview_time_ = 0, preview_asked_ = -1;
    std::vector<std::uint16_t> preview_wide_;
    // What the core has been told.
    bool reported_start_ = false, reported_paused_ = false, reported_end_ = false;
    double reported_at_ = 0;
};
} // namespace ps5
