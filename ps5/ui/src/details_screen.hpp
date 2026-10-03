// A title's page: its artwork and description on the left, and on the right the list the
// user picks from: a film's streams, or a series' episodes and then the chosen episode's
// streams.

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
    // Starts showing a title, known so far only by what the board had about it.
    void open(const std::string &type, const std::string &id, const std::string &name);
    void set_details(Details details);
    void press(Button button);
    void update(float seconds);
    void draw();

  private:
    enum class List
    {
        Episodes,
        Streams,
    };

    bool is_series() const;
    std::vector<int> seasons() const;
    std::vector<const Episode *> season_episodes() const;
    std::size_t row_count() const;
    void draw_about();
    void draw_list();
    float draw_episode(const Episode &episode, float x, float y, float width, bool focused);
    float draw_stream(const Stream &stream, float x, float y, float width, bool focused);
    std::string playing_title(const Episode *episode) const;

    NVGcontext *vg_;
    Images &images_;
    Callbacks callbacks_;
    Details details_;
    std::string opened_name_;

    List list_ = List::Streams;
    int season_ = 1;
    std::size_t focus_ = 0;
    const Episode *chosen_ = nullptr; // the episode whose streams are listed (series)
    std::string chosen_id_;
    float scroll_ = 0, scroll_target_ = 0;
    float appear_ = 0; // 0..1, fades the page in when it opens
};
} // namespace ui
