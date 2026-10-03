#include "details_screen.hpp"

#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "draw_util.hpp"
#include "images.hpp"
#include "nanovg.h"
#include "theme.hpp"

namespace ui
{
using namespace theme;

namespace
{
// The page's layout, in the same units as the rest of the UI.
constexpr float kLeft = units(4.5f);
constexpr float kAboutWidth = units(36.0f);
constexpr float kLogoTop = units(3.2f);

// Episodes: large 16:9 tiles along the bottom, season buttons above them.
constexpr float kTileWidth = units(19.0f);
constexpr float kTileHeight = kTileWidth * 9.0f / 16.0f;
constexpr float kTileGap = units(1.1f);
constexpr float kTileLabel = units(2.4f);
constexpr float kTilesTop = kScreenHeight - kTileHeight - kTileLabel - units(1.6f);
constexpr float kSeasonHeight = units(2.6f);
constexpr float kSeasonsTop = kTilesTop - kSeasonHeight - units(1.3f);
constexpr float kSeasonGap = units(0.6f);
// Where the backdrop starts darkening towards a series' episode row.
constexpr float kLowerShadeTop = kSeasonsTop - units(14.0f);
// How much of each end of the stream list fades away.
constexpr float kListEdge = units(1.8f);

// Seconds the page takes to fade in or out, and the episode text to change.
constexpr float kOpenTime = 0.24f;
constexpr float kTextOut = 0.09f;
constexpr float kTextIn = 0.20f;

// Starts fast and settles: for things arriving at their place.
float settle(float progress)
{
    const float rest = 1.0f - std::clamp(progress, 0.0f, 1.0f);
    return 1.0f - rest * rest * rest;
}

// Streams: a list down the right-hand side.
constexpr float kListLeft = units(46.0f);
constexpr float kListWidth = kScreenWidth - kListLeft - units(3.5f);
constexpr float kListHeaderTop = units(3.4f);
constexpr float kListTop = units(7.0f);
constexpr float kListBottom = kScreenHeight - units(2.0f);
constexpr float kStreamHeight = units(5.6f);
constexpr float kStreamGap = units(0.5f);
constexpr float kRowPadding = units(0.7f);

std::string joined(const std::vector<std::string> &parts, std::size_t most)
{
    std::string text;
    for (std::size_t index = 0; index < parts.size() && index < most; ++index)
        text += (text.empty() ? "" : ", ") + parts[index];
    return text;
}

// The first line of `text` and everything after it, split at the first line break.
std::pair<std::string, std::string> first_line(const std::string &text)
{
    const auto split = text.find('\n');
    if (split == std::string::npos)
        return {text, {}};
    return {text.substr(0, split), text.substr(split + 1)};
}

std::string season_name(int season)
{
    return season == 0 ? "Specials" : "Season " + std::to_string(season);
}
} // namespace

namespace
{
Backdrop backdrop = Backdrop::Light;
}

void set_backdrop(Backdrop style)
{
    backdrop = style;
}

DetailsScreen::DetailsScreen(NVGcontext *context, Images &images) : vg_{context}, images_{images}
{
}

void DetailsScreen::set_callbacks(Callbacks callbacks)
{
    callbacks_ = std::move(callbacks);
}

void DetailsScreen::leave()
{
    closing_ = true;
}

void DetailsScreen::open(const std::string &type, const std::string &id, const std::string &name)
{
    details_ = Details{};
    details_.type = type;
    details_.id = id;
    opened_name_ = name;
    zone_ = type == "series" ? Zone::Episodes : Zone::Streams;
    season_ = 1;
    episode_focus_ = 0;
    stream_focus_ = 0;
    chosen_id_.clear();
    episodes_scroll_ = episodes_scroll_target_ = 0;
    seasons_scroll_ = seasons_scroll_target_ = 0;
    streams_scroll_ = streams_scroll_target_ = 0;
    appear_ = 0;
    closing_ = false;
    streams_ = type == "series" ? 0.0f : 1.0f;
    about_valid_ = false;
    about_alpha_ = 0;
    tiles_alpha_ = 1;
    tiles_shift_ = 0;
}

void DetailsScreen::set_details(Details details)
{
    // Answers for a title other than the one on screen (a late reply) are ignored.
    if (details.id != details_.id && !details.id.empty() && !details_.id.empty())
        return;
    const bool first = details_.episodes.empty() && !details.episodes.empty();
    details_ = std::move(details);
    if (first)
    {
        // Start on the first regular season; specials (season 0) come last.
        const std::vector<int> all = seasons();
        if (!all.empty())
            season_ = all.front();
    }
    const std::size_t episodes = season_episodes().size();
    episode_focus_ = std::min(episode_focus_, episodes == 0 ? 0 : episodes - 1);
    stream_focus_ =
        std::min(stream_focus_, details_.streams.empty() ? 0 : details_.streams.size() - 1);
}

bool DetailsScreen::is_series() const
{
    return details_.type == "series";
}

std::vector<int> DetailsScreen::seasons() const
{
    std::vector<int> result;
    for (const Episode &episode : details_.episodes)
        if (std::find(result.begin(), result.end(), episode.season) == result.end())
            result.push_back(episode.season);
    std::sort(result.begin(), result.end(), [](int left, int right) {
        // Season 0 holds specials and sorts after the numbered seasons.
        return (left == 0 ? 1 << 20 : left) < (right == 0 ? 1 << 20 : right);
    });
    return result;
}

std::vector<const Episode *> DetailsScreen::season_episodes() const
{
    std::vector<const Episode *> result;
    for (const Episode &episode : details_.episodes)
        if (episode.season == season_)
            result.push_back(&episode);
    std::sort(result.begin(), result.end(),
              [](const Episode *left, const Episode *right) { return left->episode < right->episode; });
    return result;
}

const Episode *DetailsScreen::episode_by_id(const std::string &id) const
{
    for (const Episode &episode : details_.episodes)
        if (episode.id == id)
            return &episode;
    return nullptr;
}

const Episode *DetailsScreen::shown_episode() const
{
    if (!is_series())
        return nullptr;
    if (zone_ == Zone::Streams)
        return episode_by_id(chosen_id_);
    const std::vector<const Episode *> episodes = season_episodes();
    return episode_focus_ < episodes.size() ? episodes[episode_focus_] : nullptr;
}

std::string DetailsScreen::playing_title(const Episode *episode) const
{
    const std::string &name = details_.name.empty() ? opened_name_ : details_.name;
    if (episode == nullptr)
        return name;
    return name + " - S" + std::to_string(episode->season) + " E" + std::to_string(episode->episode) +
           (episode->title.empty() ? "" : " - " + episode->title);
}

// Sets the scroll targets so whatever has the focus is inside its row or list.
void DetailsScreen::follow_focus()
{
    {
        const float pitch = kTileWidth + kTileGap;
        const float window = kScreenWidth - kLeft - units(2.0f);
        const float left = static_cast<float>(episode_focus_) * pitch;
        if (left < episodes_scroll_target_)
            episodes_scroll_target_ = left;
        else if (left + kTileWidth > episodes_scroll_target_ + window)
            episodes_scroll_target_ = left + kTileWidth - window;
    }
    {
        const float pitch = kStreamHeight + kStreamGap;
        // The focused card stays clear of the fading edges at both ends of the list.
        const float window =
            (is_series() ? kSeasonsTop - units(0.6f) : kListBottom) - kListTop - kListEdge;
        const float top = static_cast<float>(stream_focus_) * pitch;
        const float lead = stream_focus_ == 0 ? 0.0f : kListEdge;
        if (top - lead < streams_scroll_target_)
            streams_scroll_target_ = top - lead;
        else if (top + pitch > streams_scroll_target_ + window)
            streams_scroll_target_ = top + pitch - window;
    }
}

void DetailsScreen::press(Button button)
{
    if (closing_)
        return;
    const int season_before = season_;
    switch (zone_)
    {
    case Zone::Seasons:
    {
        const std::vector<int> all = seasons();
        const auto current = std::find(all.begin(), all.end(), season_);
        const long index = current == all.end() ? 0 : current - all.begin();
        if (button == Button::Left && index > 0)
            season_ = all[static_cast<std::size_t>(index - 1)];
        else if (button == Button::Right && index + 1 < static_cast<long>(all.size()))
            season_ = all[static_cast<std::size_t>(index + 1)];
        else if (button == Button::Down || button == Button::Accept)
            zone_ = Zone::Episodes;
        else if (button == Button::Back)
            leave();
        if (season_ != season_before)
        {
            episode_focus_ = 0;
            episodes_scroll_ = episodes_scroll_target_ = 0;
            // The new season's episodes come in from the side the season was on.
            tiles_alpha_ = 0;
            tiles_shift_ = button == Button::Right ? units(5.0f) : -units(5.0f);
        }
        break;
    }
    case Zone::Episodes:
    {
        const std::vector<const Episode *> episodes = season_episodes();
        if (button == Button::Left && episode_focus_ > 0)
            --episode_focus_;
        else if (button == Button::Right && episode_focus_ + 1 < episodes.size())
            ++episode_focus_;
        else if (button == Button::Up && seasons().size() > 1)
            zone_ = Zone::Seasons;
        else if (button == Button::Accept && episode_focus_ < episodes.size())
        {
            chosen_id_ = episodes[episode_focus_]->id;
            details_.streams.clear();
            details_.streams_loading = 1;
            zone_ = Zone::Streams;
            stream_focus_ = 0;
            streams_scroll_ = streams_scroll_target_ = 0;
            if (callbacks_.select_video)
                callbacks_.select_video(chosen_id_);
        }
        else if (button == Button::Back)
            leave();
        break;
    }
    case Zone::Streams:
        if (button == Button::Up && stream_focus_ > 0)
            --stream_focus_;
        else if (button == Button::Down && stream_focus_ + 1 < details_.streams.size())
            ++stream_focus_;
        else if (button == Button::Accept && stream_focus_ < details_.streams.size() &&
                 !details_.streams[stream_focus_].url.empty() && callbacks_.play)
            callbacks_.play(details_.streams[stream_focus_], playing_title(episode_by_id(chosen_id_)));
        else if (button == Button::Back)
        {
            if (is_series())
                zone_ = Zone::Episodes; // back to the episode the streams were for
            else
                leave();
        }
        break;
    }
    follow_focus();
}

void DetailsScreen::update(float seconds)
{
    episodes_scroll_ = eased(episodes_scroll_, episodes_scroll_target_, seconds);
    seasons_scroll_ = eased(seasons_scroll_, seasons_scroll_target_, seconds);
    streams_scroll_ = eased(streams_scroll_, streams_scroll_target_, seconds);
    streams_ = eased(streams_, zone_ == Zone::Streams ? 1.0f : 0.0f, seconds);
    tiles_alpha_ = std::min(1.0f, tiles_alpha_ + seconds / 0.22f);
    tiles_shift_ = eased(tiles_shift_, 0.0f, seconds);

    if (closing_)
    {
        appear_ = std::max(0.0f, appear_ - seconds / kOpenTime);
        if (appear_ <= 0 && callbacks_.close)
        {
            closing_ = false;
            callbacks_.close();
        }
    }
    else
    {
        appear_ = std::min(1.0f, appear_ + seconds / kOpenTime);
    }

    // The episode text trails the focus: out, switch, in.
    const Episode *target = shown_episode();
    const bool stale = about_valid_ ? target == nullptr || target->id != about_.id : target != nullptr;
    if (stale)
    {
        about_alpha_ = std::max(0.0f, about_alpha_ - seconds / kTextOut);
        if (about_alpha_ <= 0)
        {
            about_valid_ = target != nullptr;
            if (target != nullptr)
                about_ = *target;
        }
    }
    else if (about_valid_)
    {
        // Kept current, so a summary that arrives late (details still loading) shows.
        about_ = *target;
        about_alpha_ = std::min(1.0f, about_alpha_ + seconds / kTextIn);
    }
}

// The title's logo (or name) and its key facts. `top` is left at the line under them.
void DetailsScreen::draw_header(float &top)
{
    const float fade = settle(appear_);
    const float left = kLeft;
    // Everything on the page rises a little into place as it fades in.
    top = kLogoTop + (1.0f - fade) * units(0.8f);
    const Images::Texture logo = images_.get(details_.logo, kLogoPixels);
    if (!draw_logo(vg_, left, top, kHeroLogoWidth, kHeroLogoHeight, logo, fade) &&
        logo.state == Images::State::Unavailable)
    {
        nvgFontFace(vg_, "bold");
        nvgFontSize(vg_, kHeroTitleSize);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        nvgFillColor(vg_, foreground(fade));
        fitted_text(vg_, left, top + kHeroLogoHeight, kAboutWidth,
                    details_.name.empty() ? opened_name_ : details_.name);
    }
    top += kHeroLogoHeight + units(1.3f);

    nvgFontSize(vg_, kHeroMetaSize);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    float x = left;
    const float middle = top + kHeroMetaSize / 2;
    if (!details_.imdb_rating.empty())
    {
        nvgFontFace(vg_, "bold");
        nvgFontSize(vg_, kHeroMetaSize * 0.8f);
        const float badge = nvgTextBounds(vg_, 0, 0, "IMDb", nullptr, nullptr) + units(0.7f);
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, x, middle - units(0.7f), badge, units(1.4f), units(0.25f));
        nvgFillColor(vg_, nvgTransRGBAf(imdb_yellow(), fade));
        nvgFill(vg_);
        nvgFillColor(vg_, nvgRGBAf(0, 0, 0, fade));
        nvgText(vg_, x + units(0.35f), middle, "IMDb", nullptr);
        nvgFontSize(vg_, kHeroMetaSize);
        nvgFontFace(vg_, "semibold");
        nvgFillColor(vg_, foreground(fade));
        x = nvgText(vg_, x + badge + units(0.5f), middle, details_.imdb_rating.c_str(), nullptr);
    }
    const auto fact = [&](const std::string &text) {
        if (text.empty())
            return;
        if (x > left)
        {
            nvgFontFace(vg_, "regular");
            nvgFillColor(vg_, foreground(0.35f * fade));
            x = nvgText(vg_, x + units(0.6f), middle, "\xC2\xB7", nullptr) + units(0.6f);
        }
        nvgFontFace(vg_, "medium");
        nvgFillColor(vg_, foreground(0.8f * fade));
        x = nvgText(vg_, x, middle, text.c_str(), nullptr);
    };
    fact(details_.release_info);
    fact(details_.runtime);
    fact(joined(details_.genres, 3));
    top += kHeroMetaSize + units(1.4f);
}

// A film's description and cast.
void DetailsScreen::draw_film_about(float top)
{
    const float fade = settle(appear_);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    if (details_.state == Details::State::Error)
    {
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.2f));
        nvgFillColor(vg_, foreground(0.6f * fade));
        nvgText(vg_, kLeft, top, "No add-on has details for this title.", nullptr);
        return;
    }
    nvgFontFace(vg_, "regular");
    nvgFontSize(vg_, units(1.1f));
    nvgFillColor(vg_, foreground(0.75f * fade));
    top += wrapped_text(vg_, kLeft, top, kAboutWidth, units(1.1f) * 1.5f, 7, details_.description);
    top += units(1.2f);
    if (!details_.cast.empty())
    {
        nvgFontFace(vg_, "medium");
        nvgFontSize(vg_, units(0.95f));
        nvgFillColor(vg_, foreground(0.45f * fade));
        nvgText(vg_, kLeft, top, "Cast", nullptr);
        top += units(1.5f);
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.05f));
        nvgFillColor(vg_, foreground(0.75f * fade));
        fitted_text(vg_, kLeft, top, kAboutWidth, joined(details_.cast, 5));
    }
}

// An episode's name, date and summary, where a film's description would be.
void DetailsScreen::draw_episode_about(float top, const Episode &episode)
{
    const float fade = settle(appear_) * about_alpha_;
    // Under the title's facts, rising a little into place as it fades in.
    top += units(0.4f) + (1.0f - about_alpha_) * units(0.6f);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, units(0.95f));
    nvgFillColor(vg_, foreground(0.9f * fade));
    std::string place = "Season " + std::to_string(episode.season) + "  \xC2\xB7  Episode " +
                        std::to_string(episode.episode);
    if (episode.season == 0)
        place = "Special  \xC2\xB7  Episode " + std::to_string(episode.episode);
    if (!episode.released.empty())
        place += "  \xC2\xB7  " + episode.released;
    nvgText(vg_, kLeft, top, place.c_str(), nullptr);
    top += units(2.3f);

    nvgFontFace(vg_, "semibold");
    nvgFontSize(vg_, units(1.9f));
    nvgFillColor(vg_, foreground(fade));
    fitted_text(vg_, kLeft, top, kAboutWidth, drawable(episode.title));
    top += units(2.9f);

    nvgFontFace(vg_, "regular");
    nvgFontSize(vg_, units(1.1f));
    nvgFillColor(vg_, foreground(0.75f * fade));
    wrapped_text(vg_, kLeft, top, kAboutWidth, units(1.1f) * 1.5f, 4,
                 episode.overview.empty() ? details_.description : drawable(episode.overview));
}

void DetailsScreen::draw_seasons()
{
    const std::vector<int> all = seasons();
    if (all.size() < 2)
        return;
    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, units(1.05f));
    nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    const float shown = settle(appear_);
    const float rise = (1.0f - shown) * units(0.8f);
    float x = kLeft - seasons_scroll_;
    for (const int season : all)
    {
        const std::string name = season_name(season);
        const float width = nvgTextBounds(vg_, 0, 0, name.c_str(), nullptr, nullptr) + units(2.2f);
        const bool selected = season == season_;
        const bool focused = selected && zone_ == Zone::Seasons;
        if (selected)
        {
            // Keep the selected season on screen.
            const float left = x + seasons_scroll_ - kLeft;
            const float window = kScreenWidth - kLeft - units(2.0f);
            if (left < seasons_scroll_target_)
                seasons_scroll_target_ = left;
            else if (left + width > seasons_scroll_target_ + window)
                seasons_scroll_target_ = left + width - window;
        }
        const float y = kSeasonsTop + rise;
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, x, y, width, kSeasonHeight, kRadius);
        nvgFillColor(vg_, selected ? accent(shown) : overlay(2.4f * shown));
        nvgFill(vg_);
        if (focused)
            focus_ring(vg_, x, y, width, kSeasonHeight, kRadius);
        nvgFillColor(vg_, foreground((selected ? 1.0f : 0.75f) * shown));
        nvgText(vg_, x + width / 2, y + kSeasonHeight / 2, name.c_str(), nullptr);
        x += width + kSeasonGap;
    }
}

void DetailsScreen::draw_episodes()
{
    const std::vector<const Episode *> episodes = season_episodes();
    if (episodes.empty())
    {
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.1f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground(0.6f * settle(appear_)));
        nvgText(vg_, kLeft, kTilesTop,
                details_.state == Details::State::Ready ? "No episodes listed." : "Loading episodes",
                nullptr);
        return;
    }
    const float pitch = kTileWidth + kTileGap;
    const auto first = static_cast<std::size_t>(std::max(0.0f, std::floor(episodes_scroll_ / pitch) - 1));
    const std::size_t last =
        std::min(episodes.size(), first + static_cast<std::size_t>(kScreenWidth / pitch) + 3);
    const std::size_t focused = zone_ == Zone::Episodes ? episode_focus_ : episodes.size();
    // While the streams are open the row stays, dimmed, with the chosen episode marked.
    const float dim = 1.0f - 0.65f * streams_;
    const float shown = settle(appear_) * tiles_alpha_;
    const float rise = (1.0f - settle(appear_)) * units(0.8f);

    const auto draw_tile = [&](std::size_t index) {
        const Episode &episode = *episodes[index];
        const bool is_focused = index == focused;
        const bool is_chosen = zone_ == Zone::Streams && episode.id == chosen_id_;
        float x = kLeft + static_cast<float>(index) * pitch - episodes_scroll_ + tiles_shift_;
        float y = kTilesTop + rise;
        float w = kTileWidth, h = kTileHeight;
        if (is_focused)
        {
            x -= w * (kFocusScale - 1) / 2;
            y -= h * (kFocusScale - 1) / 2;
            w *= kFocusScale;
            h *= kFocusScale;
        }
        const float alpha = shown * (is_chosen ? 1.0f : dim);
        const Images::Texture still = images_.get(episode.thumbnail, kStillPixels);
        if (still.state == Images::State::Ready)
        {
            cover_image(vg_, x, y, w, h, kRadius, still, alpha);
        }
        else
        {
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, x, y, w, h, kRadius);
            nvgFillColor(vg_, overlay(2.0f * alpha));
            nvgFill(vg_);
        }
        // The episode's number over the still's lower left corner.
        nvgFontFace(vg_, "bold");
        nvgFontSize(vg_, units(1.0f));
        const std::string number = std::to_string(episode.episode);
        const float badge = nvgTextBounds(vg_, 0, 0, number.c_str(), nullptr, nullptr) + units(1.0f);
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, x + units(0.5f), y + h - units(2.1f), badge, units(1.6f), units(0.4f));
        nvgFillColor(vg_, nvgRGBAf(0, 0, 0, 0.7f * alpha));
        nvgFill(vg_);
        nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, foreground(alpha));
        nvgText(vg_, x + units(0.5f) + badge / 2, y + h - units(1.3f), number.c_str(), nullptr);
        if (is_focused || is_chosen)
            focus_ring(vg_, x, y, w, h, kRadius);

        nvgFontFace(vg_, "medium");
        nvgFontSize(vg_, units(1.0f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, foreground((is_focused || is_chosen ? 1.0f : 0.8f) * alpha));
        fitted_text(vg_,
                    kLeft + static_cast<float>(index) * pitch - episodes_scroll_ + tiles_shift_ + units(0.2f),
                    kTilesTop + rise + kTileHeight + kTileLabel / 2 + units(0.3f),
                    kTileWidth - units(0.4f), drawable(episode.title));
    };
    for (std::size_t index = first; index < last; ++index)
        if (index != focused)
            draw_tile(index);
    if (focused >= first && focused < last)
        draw_tile(focused);
}

void DetailsScreen::draw_stream(const Stream &stream, float x, float y, float width, bool focused)
{
    const bool playable = !stream.url.empty();
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, x, y, width, kStreamHeight, kRadius);
    nvgFillColor(vg_, focused ? nvgRGBAf(0.16f, 0.16f, 0.18f, 0.95f) : nvgRGBAf(0.07f, 0.07f, 0.08f, 0.88f));
    nvgFill(vg_);
    if (focused)
        focus_ring(vg_, x, y, width, kStreamHeight, kRadius);
    const float dim = playable ? 1.0f : 0.45f;

    // Left: who offers it (the stream's own name, and the add-on under it).
    const float name_width = units(10.5f);
    const auto [name, name_rest] = first_line(drawable(stream.name));
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFontFace(vg_, "semibold");
    nvgFontSize(vg_, units(1.0f));
    nvgFillColor(vg_, foreground(dim));
    fitted_text(vg_, x + kRowPadding, y + kRowPadding, name_width, name.empty() ? stream.addon : name);
    nvgFontFace(vg_, "regular");
    nvgFontSize(vg_, units(0.85f));
    nvgFillColor(vg_, foreground(0.6f * dim));
    wrapped_text(vg_, x + kRowPadding, y + kRowPadding + units(1.5f), name_width,
                 units(0.85f) * 1.4f, 2, name_rest.empty() ? stream.addon : name_rest);

    // Right: what it is.
    const float text_x = x + kRowPadding + name_width + units(1.0f);
    const float text_width = x + width - kRowPadding - text_x;
    std::string description = drawable(stream.description);
    std::replace(description.begin(), description.end(), '\n', ' ');
    nvgFontSize(vg_, units(0.9f));
    nvgFillColor(vg_, foreground(0.8f * dim));
    const float used = wrapped_text(vg_, text_x, y + kRowPadding, text_width, units(0.9f) * 1.4f,
                                    playable ? 3 : 2, description);
    if (!playable)
    {
        nvgFontFace(vg_, "medium");
        nvgFontSize(vg_, units(0.85f));
        nvgFillColor(vg_, foreground(0.5f));
        const std::string reason = "Cannot be played here: " + stream.unsupported;
        nvgText(vg_, text_x, y + kRowPadding + used + units(0.2f), reason.c_str(), nullptr);
    }
}

void DetailsScreen::draw_streams()
{
    if (streams_ < 0.01f)
        return;
    // The list slides in from the right.
    const float shift = (1.0f - streams_) * units(6.0f);
    const float x = kListLeft + shift;
    const float shown = streams_ * settle(appear_);
    nvgGlobalAlpha(vg_, shown);

    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, kRowTitleSize);
    nvgFillColor(vg_, foreground());
    nvgText(vg_, x, kListHeaderTop, "Streams", nullptr);

    nvgSave(vg_);
    // A series' list stops above its episode row; a film's runs to the bottom. Each end of
    // the list has a short edge in which cards fade away; the top one only once the list
    // has been scrolled.
    constexpr int kEdgeBands = 14;
    const float bottom = is_series() ? kSeasonsTop - units(0.6f) : kListBottom;
    const float list_top = kListTop - units(0.4f);
    const float clip_x = x - units(0.5f), clip_width = kListWidth + units(1.0f);
    const float solid_bottom = bottom - kListEdge;
    const float solid_top =
        list_top + kListEdge * std::clamp(streams_scroll_ / units(1.0f), 0.0f, 1.0f);
    float y = kListTop - streams_scroll_;
    for (std::size_t index = 0; index < details_.streams.size(); ++index)
    {
        if (y + kStreamHeight > list_top && y < bottom)
        {
            const bool focused = zone_ == Zone::Streams && index == stream_focus_;
            const Stream &stream = details_.streams[index];
            // The part of the card between the two edges is drawn as it is.
            nvgScissor(vg_, clip_x, solid_top, clip_width, solid_bottom - solid_top);
            nvgGlobalAlpha(vg_, shown);
            draw_stream(stream, x, y, kListWidth, focused);
            // The part inside an edge is drawn band by band, each fainter than the last,
            // down to nothing at the list's very end: the card dissolves there, so the
            // square cut that ends it is never seen.
            const auto edge = [&](bool lower) {
                const float from = lower ? solid_bottom : list_top;
                const float span = lower ? bottom - solid_bottom : solid_top - list_top;
                if (span <= 0 || y + kStreamHeight + kFocusOutline < from || y - kFocusOutline > from + span)
                    return;
                const float band = span / kEdgeBands;
                for (int step = 0; step < kEdgeBands; ++step)
                {
                    // 1 next to the solid part, 0 at the list's end; squared, so the card
                    // is mostly gone well before the end.
                    const float near = lower ? 1.0f - (step + 0.5f) / kEdgeBands : (step + 0.5f) / kEdgeBands;
                    nvgScissor(vg_, clip_x, from + step * band, clip_width, band + 0.5f);
                    nvgGlobalAlpha(vg_, shown * near * near);
                    draw_stream(stream, x, y, kListWidth, focused);
                }
            };
            edge(true);
            edge(false);
        }
        y += kStreamHeight + kStreamGap;
    }
    nvgScissor(vg_, clip_x, list_top, clip_width, bottom - list_top);
    nvgGlobalAlpha(vg_, shown);
    // Under the streams so far (or instead of them): whether more are on their way.
    nvgFontFace(vg_, "regular");
    nvgFontSize(vg_, units(1.05f));
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg_, foreground(0.7f));
    if (details_.streams_loading > 0)
        nvgText(vg_, x, y + units(0.3f), "Asking add-ons for streams", nullptr);
    else if (details_.streams.empty())
        nvgTextBox(vg_, x, y + units(0.3f), kListWidth,
                   "No add-on offered a stream for this. Streams come from the add-ons "
                   "installed on your Stremio account.",
                   nullptr);
    nvgRestore(vg_);
    nvgGlobalAlpha(vg_, 1.0f);

}

// The page's backdrop: the title's artwork, softened, under a grey wash, so it sets the
// page's mood without fighting the text. It is darkened further down the left behind the
// text, and along the bottom behind a series' episodes.
void DetailsScreen::draw_backdrop(float alpha)
{
    nvgBeginPath(vg_);
    nvgRect(vg_, 0, 0, kScreenWidth, kScreenHeight);
    nvgFillColor(vg_, nvgRGBAf(0, 0, 0, alpha));
    nvgFill(vg_);
    const Images::Texture art = backdrop == Backdrop::Sharp
                                    ? images_.get(details_.background, kArtPixels)
                                    : images_.get_soft(details_.background, backdrop == Backdrop::Light);
    if (art.state != Images::State::Ready)
        return;
    const float shown = settle(appear_) * alpha;
    cover_image(vg_, 0, 0, kScreenWidth, kScreenHeight, 0, art, shown);
    const auto shade = [&](float x0, float y0, float x1, float y1, NVGcolor from, NVGcolor to) {
        nvgBeginPath(vg_);
        nvgRect(vg_, 0, 0, kScreenWidth, kScreenHeight);
        nvgFillPaint(vg_, nvgLinearGradient(vg_, x0, y0, x1, y1, from, to));
        nvgFill(vg_);
    };
    const float wash = backdrop == Backdrop::Sharp ? 0.62f : 0.5f;
    const NVGcolor grey = nvgRGBAf(0.07f, 0.07f, 0.085f, wash * shown);
    shade(0, 0, kScreenWidth, 0, grey, grey);
    shade(0, 0, kScreenWidth * 0.75f, 0, nvgRGBAf(0, 0, 0, 0.72f * shown), nvgRGBAf(0, 0, 0, 0));
    if (is_series())
        shade(0, kLowerShadeTop, 0, kSeasonsTop + units(1.0f), nvgRGBAf(0, 0, 0, 0),
              nvgRGBAf(0, 0, 0, 0.78f * shown));
}

void DetailsScreen::draw()
{
    draw_backdrop(1.0f);

    float top = 0;
    draw_header(top);
    if (is_series())
    {
        if (about_valid_)
            draw_episode_about(top, about_);
        draw_seasons();
        draw_episodes();
    }
    else
    {
        draw_film_about(top);
    }
    draw_streams();
}
} // namespace ui
