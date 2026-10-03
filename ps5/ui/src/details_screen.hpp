// A title's page, laid out for a television. The title's artwork fills the screen with its
// logo and key facts at the top left.
//
// A series shows its episodes as a row of large tiles along the bottom, under a row of
// season buttons; the focused episode's name and summary take the place of the series'
// description. Choosing an episode lists its streams on the right. A film goes straight
// to its streams, beside its description.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "details_data.hpp"

struct NVGcontext;

namespace ui
{
class Images;
enum class Button;

// How a title page treats its backdrop artwork.
enum class Backdrop
{
    Soft,  // well blurred under a grey wash
    Light, // lightly blurred under a grey wash
    Sharp, // not blurred, under a darker wash
};
void set_backdrop(Backdrop style);

class DetailsScreen
{
  public:
    // What the screen asks its owner to do.
    struct Callbacks
    {
        // Load the streams for one of the title's videos (an episode).
        std::function<void(const std::string &video)> select_video;
        // Play a stream; `title` is what the player shows.
        std::function<void(const Stream &stream, const std::string &title)> play;
        std::function<void()> close;
    };

    DetailsScreen(NVGcontext *context, Images &images);

    void set_callbacks(Callbacks callbacks);
    // Starts the page's closing animation; `Callbacks::close` is called when it is done.
    void leave();
    // Starts showing a title, known so far only by what the board had about it.
    void open(const std::string &type, const std::string &id, const std::string &name);
    void set_details(Details details);
    void press(Button button);
    void update(float seconds);
    void draw();

  private:
    enum class Zone
    {
        Seasons,  // the row of season buttons
        Episodes, // the row of episode tiles
        Streams,  // the list of streams
    };

    bool is_series() const;
    std::vector<int> seasons() const;
    std::vector<const Episode *> season_episodes() const;
    const Episode *episode_by_id(const std::string &id) const;
    // The episode whose name and summary are shown: the focused tile's, or the one whose
    // streams are listed.
    const Episode *shown_episode() const;
    void follow_focus();
    void draw_header(float &top);
    void draw_film_about(float top);
    void draw_episode_about(float top, const Episode &episode);
    void draw_seasons();
    void draw_episodes();
    void draw_streams();
    void draw_backdrop(float alpha);
    void draw_stream(const Stream &stream, float x, float y, float width, bool focused);
    std::string playing_title(const Episode *episode) const;

    NVGcontext *vg_;
    Images &images_;
    Callbacks callbacks_;
    Details details_;
    std::string opened_name_;

    Zone zone_ = Zone::Streams;
    int season_ = 1;
    std::size_t episode_focus_ = 0;
    std::size_t stream_focus_ = 0;
    std::string chosen_id_; // the episode whose streams are listed (series)
    float episodes_scroll_ = 0, episodes_scroll_target_ = 0;
    float seasons_scroll_ = 0, seasons_scroll_target_ = 0;
    float streams_scroll_ = 0, streams_scroll_target_ = 0;
    float appear_ = 0;    // 0..1, how far the page has opened
    bool closing_ = false; // running the opening backwards before telling the owner
    float streams_ = 0;   // 0..1, how far the stream list has slid in
    // The episode whose name and summary are on screen. It trails the focus: the text
    // fades out, switches, and fades in.
    Episode about_;
    bool about_valid_ = false;
    float about_alpha_ = 0;
    // The episode row fades and slides in afresh when the season changes.
    float tiles_alpha_ = 1;
    float tiles_shift_ = 0;
};
} // namespace ui
