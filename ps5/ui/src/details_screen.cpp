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
constexpr float kAboutLeft = units(4.5f);
constexpr float kAboutWidth = units(34.0f);
constexpr float kAboutTop = units(6.0f);
constexpr float kListLeft = units(46.0f);
constexpr float kListWidth = kScreenWidth - kListLeft - units(3.5f);
constexpr float kListHeaderTop = units(3.4f);
constexpr float kListTop = units(7.6f);
constexpr float kListBottom = kScreenHeight - units(2.0f);
constexpr float kListRowGap = units(0.5f);
constexpr float kEpisodeHeight = units(6.2f);
constexpr float kStreamHeight = units(5.6f);
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
} // namespace

DetailsScreen::DetailsScreen(NVGcontext *context, Images &images) : vg_{context}, images_{images}
{
}

void DetailsScreen::set_callbacks(Callbacks callbacks)
{
    callbacks_ = std::move(callbacks);
}

void DetailsScreen::open(const std::string &type, const std::string &id, const std::string &name)
{
    details_ = Details{};
    details_.type = type;
    details_.id = id;
    opened_name_ = name;
    list_ = type == "series" ? List::Episodes : List::Streams;
    season_ = 1;
    focus_ = 0;
    chosen_id_.clear();
    scroll_ = scroll_target_ = 0;
    appear_ = 0;
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
    focus_ = std::min(focus_, row_count() == 0 ? 0 : row_count() - 1);
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

std::size_t DetailsScreen::row_count() const
{
    return list_ == List::Episodes ? season_episodes().size() : details_.streams.size();
}

std::string DetailsScreen::playing_title(const Episode *episode) const
{
    const std::string &name = details_.name.empty() ? opened_name_ : details_.name;
    if (episode == nullptr)
        return name;
    return name + " - S" + std::to_string(episode->season) + " E" + std::to_string(episode->episode) +
           (episode->title.empty() ? "" : " - " + episode->title);
}

void DetailsScreen::press(Button button)
{
    const std::size_t count = row_count();
    switch (button)
    {
    case Button::Up:
        if (focus_ > 0)
            --focus_;
        break;
    case Button::Down:
        if (focus_ + 1 < count)
            ++focus_;
        break;
    case Button::Left:
    case Button::Right:
        if (list_ == List::Episodes)
        {
            // Left and right step through the seasons.
            const std::vector<int> all = seasons();
            const auto current = std::find(all.begin(), all.end(), season_);
            if (current != all.end())
            {
                const auto index = current - all.begin();
                if (button == Button::Left && index > 0)
                    season_ = all[index - 1];
                else if (button == Button::Right && index + 1 < static_cast<long>(all.size()))
                    season_ = all[index + 1];
                else
                    break;
                focus_ = 0;
                scroll_ = scroll_target_ = 0;
            }
        }
        break;
    case Button::Accept:
        if (list_ == List::Episodes)
        {
            const std::vector<const Episode *> episodes = season_episodes();
            if (focus_ < episodes.size())
            {
                chosen_id_ = episodes[focus_]->id;
                details_.streams.clear();
                details_.streams_loading = 1;
                list_ = List::Streams;
                focus_ = 0;
                scroll_ = scroll_target_ = 0;
                if (callbacks_.select_video)
                    callbacks_.select_video(chosen_id_);
            }
        }
        else if (focus_ < details_.streams.size() && !details_.streams[focus_].url.empty() &&
                 callbacks_.play)
        {
            const Episode *episode = nullptr;
            for (const Episode &candidate : details_.episodes)
                if (candidate.id == chosen_id_)
                    episode = &candidate;
            callbacks_.play(details_.streams[focus_], playing_title(episode));
        }
        break;
    case Button::Back:
        if (is_series() && list_ == List::Streams)
        {
            // Back to the episode the streams were for.
            list_ = List::Episodes;
            const std::vector<const Episode *> episodes = season_episodes();
            focus_ = 0;
            for (std::size_t index = 0; index < episodes.size(); ++index)
                if (episodes[index]->id == chosen_id_)
                    focus_ = index;
            scroll_ = scroll_target_ = 0;
        }
        else if (callbacks_.close)
        {
            callbacks_.close();
        }
        break;
    }

    // Keep the focused row inside the list's window.
    const float height = (list_ == List::Episodes ? kEpisodeHeight : kStreamHeight) + kListRowGap;
    const float window = kListBottom - kListTop;
    const float top = static_cast<float>(focus_) * height;
    if (top < scroll_target_)
        scroll_target_ = top;
    else if (top + height > scroll_target_ + window)
        scroll_target_ = top + height - window;
}

void DetailsScreen::update(float seconds)
{
    scroll_ = eased(scroll_, scroll_target_, seconds);
    appear_ = std::min(1.0f, appear_ + seconds / 0.25f);
}

void DetailsScreen::draw_about()
{
    float top = kAboutTop;
    const float fade = appear_;

    // Title: the logo when the title has one, its name otherwise or while nothing else is
    // known about it yet.
    const Images::Texture logo = images_.get(details_.logo);
    if (!draw_logo(vg_, kAboutLeft, top, kHeroLogoWidth, kHeroLogoHeight, logo, fade) &&
        logo.state == Images::State::Unavailable)
    {
        nvgFontFace(vg_, "bold");
        nvgFontSize(vg_, kHeroTitleSize);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        nvgFillColor(vg_, foreground(fade));
        fitted_text(vg_, kAboutLeft, top + kHeroLogoHeight, kAboutWidth,
                    details_.name.empty() ? opened_name_ : details_.name);
    }
    top += kHeroLogoHeight + units(1.4f);

    if (details_.state == Details::State::Loading && details_.name.empty())
    {
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.2f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground(0.6f * fade));
        nvgText(vg_, kAboutLeft, top, "Loading", nullptr);
        return;
    }
    if (details_.state == Details::State::Error)
    {
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.2f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground(0.6f * fade));
        nvgText(vg_, kAboutLeft, top, "No add-on has details for this title.", nullptr);
        return;
    }

    // Key facts on one line.
    nvgFontSize(vg_, kHeroMetaSize);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    float x = kAboutLeft;
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
        if (x > kAboutLeft)
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
    top += kHeroMetaSize + units(1.2f);

    nvgFontFace(vg_, "regular");
    nvgFontSize(vg_, units(1.1f));
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg_, foreground(0.75f * fade));
    top += wrapped_text(vg_, kAboutLeft, top, kAboutWidth, units(1.1f) * 1.5f, 7,
                        details_.description);
    top += units(1.2f);

    if (!details_.cast.empty())
    {
        nvgFontFace(vg_, "medium");
        nvgFontSize(vg_, units(0.95f));
        nvgFillColor(vg_, foreground(0.45f * fade));
        nvgText(vg_, kAboutLeft, top, "Cast", nullptr);
        top += units(1.5f);
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.05f));
        nvgFillColor(vg_, foreground(0.75f * fade));
        fitted_text(vg_, kAboutLeft, top, kAboutWidth, joined(details_.cast, 5));
    }
}

float DetailsScreen::draw_episode(const Episode &episode, float x, float y, float width, bool focused)
{
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, x, y, width, kEpisodeHeight, kRadius);
    nvgFillColor(vg_, overlay(focused ? 2.6f : 1.2f));
    nvgFill(vg_);
    if (focused)
        focus_ring(vg_, x, y, width, kEpisodeHeight, kRadius);

    // Thumbnail, 16:9.
    const float image_height = kEpisodeHeight - 2 * kRowPadding;
    const float image_width = image_height * 16.0f / 9.0f;
    const Images::Texture thumbnail = images_.get(episode.thumbnail);
    if (thumbnail.state == Images::State::Ready)
    {
        cover_image(vg_, x + kRowPadding, y + kRowPadding, image_width, image_height, units(0.4f),
                    thumbnail);
    }
    else
    {
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, x + kRowPadding, y + kRowPadding, image_width, image_height, units(0.4f));
        nvgFillColor(vg_, overlay(1.6f));
        nvgFill(vg_);
    }

    const float text_x = x + kRowPadding + image_width + units(0.9f);
    const float text_width = x + width - kRowPadding - text_x;
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFontFace(vg_, "semibold");
    nvgFontSize(vg_, units(1.1f));
    nvgFillColor(vg_, foreground(1.0f));
    fitted_text(vg_, text_x, y + kRowPadding, text_width,
                std::to_string(episode.episode) + ". " + drawable(episode.title));
    nvgFontFace(vg_, "regular");
    nvgFontSize(vg_, units(0.85f));
    nvgFillColor(vg_, foreground(0.5f));
    nvgText(vg_, text_x, y + kRowPadding + units(1.5f), episode.released.c_str(), nullptr);
    nvgFontSize(vg_, units(0.9f));
    nvgFillColor(vg_, foreground(0.65f));
    wrapped_text(vg_, text_x, y + kRowPadding + units(2.7f), text_width, units(0.9f) * 1.4f, 2,
                 drawable(episode.overview));
    return kEpisodeHeight + kListRowGap;
}

float DetailsScreen::draw_stream(const Stream &stream, float x, float y, float width, bool focused)
{
    const bool playable = !stream.url.empty();
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, x, y, width, kStreamHeight, kRadius);
    nvgFillColor(vg_, overlay(focused ? 2.6f : 1.2f));
    nvgFill(vg_);
    if (focused)
        focus_ring(vg_, x, y, width, kStreamHeight, kRadius);
    const float dim = playable ? 1.0f : 0.45f;

    // Left: who offers it (the add-on, and the stream's own name under it).
    const float name_width = units(10.5f);
    const auto [name, name_rest] = first_line(drawable(stream.name));
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFontFace(vg_, "semibold");
    nvgFontSize(vg_, units(1.0f));
    nvgFillColor(vg_, foreground(dim));
    fitted_text(vg_, x + kRowPadding, y + kRowPadding, name_width,
                name.empty() ? stream.addon : name);
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
    return kStreamHeight + kListRowGap;
}

void DetailsScreen::draw_list()
{
    // Header: what the list is.
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, kRowTitleSize);
    nvgFillColor(vg_, foreground(appear_));
    std::string header = "Streams";
    std::string hint;
    if (list_ == List::Episodes)
    {
        header = season_ == 0 ? "Specials" : "Season " + std::to_string(season_);
        if (seasons().size() > 1)
            hint = "Left and right change season";
    }
    else if (is_series())
    {
        for (const Episode &episode : details_.episodes)
            if (episode.id == chosen_id_)
                header = "S" + std::to_string(episode.season) + " E" +
                         std::to_string(episode.episode) + "  " + drawable(episode.title);
    }
    fitted_text(vg_, kListLeft, kListHeaderTop, kListWidth, header);
    if (!hint.empty())
    {
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(0.9f));
        nvgFillColor(vg_, foreground(0.5f * appear_));
        nvgText(vg_, kListLeft, kListHeaderTop + kRowTitleSize * 1.35f, hint.c_str(), nullptr);
    }

    nvgSave(vg_);
    nvgScissor(vg_, kListLeft - units(0.5f), kListTop - units(0.4f), kListWidth + units(1.0f),
               kListBottom - kListTop + units(0.8f));
    nvgGlobalAlpha(vg_, appear_);
    float y = kListTop - scroll_;
    if (list_ == List::Episodes)
    {
        const std::vector<const Episode *> episodes = season_episodes();
        for (std::size_t index = 0; index < episodes.size(); ++index)
        {
            if (y + kEpisodeHeight > kListTop - units(1.0f) && y < kListBottom + units(1.0f))
                draw_episode(*episodes[index], kListLeft, y, kListWidth, index == focus_);
            y += kEpisodeHeight + kListRowGap;
        }
        if (episodes.empty())
        {
            nvgFontFace(vg_, "regular");
            nvgFontSize(vg_, units(1.1f));
            nvgFillColor(vg_, foreground(0.6f));
            nvgText(vg_, kListLeft, kListTop,
                    details_.state == Details::State::Loading ? "Loading episodes" : "No episodes listed.",
                    nullptr);
        }
    }
    else
    {
        for (std::size_t index = 0; index < details_.streams.size(); ++index)
        {
            if (y + kStreamHeight > kListTop - units(1.0f) && y < kListBottom + units(1.0f))
                draw_stream(details_.streams[index], kListLeft, y, kListWidth, index == focus_);
            y += kStreamHeight + kListRowGap;
        }
        // Under the streams so far (or instead of them): whether more are on their way.
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.05f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground(0.6f));
        if (details_.streams_loading > 0)
            nvgText(vg_, kListLeft, y + units(0.3f), "Asking add-ons for streams", nullptr);
        else if (details_.streams.empty())
            nvgTextBox(vg_, kListLeft, y + units(0.3f), kListWidth,
                       "No add-on offered a stream for this. Streams come from the add-ons "
                       "installed on your Stremio account.",
                       nullptr);
    }
    nvgGlobalAlpha(vg_, 1.0f);
    nvgRestore(vg_);
}

void DetailsScreen::draw()
{
    nvgBeginPath(vg_);
    nvgRect(vg_, 0, 0, kScreenWidth, kScreenHeight);
    nvgFillColor(vg_, background());
    nvgFill(vg_);

    // The title's artwork over the whole screen, darkened so the text on it reads: more on
    // the left, where the description is, and behind the list on the right.
    const Images::Texture art = images_.get(details_.background);
    if (art.state == Images::State::Ready)
    {
        cover_image(vg_, 0, 0, kScreenWidth, kScreenHeight, 0, art, appear_);
        const auto shade = [&](float x0, float x1, float from, float to) {
            nvgBeginPath(vg_);
            nvgRect(vg_, 0, 0, kScreenWidth, kScreenHeight);
            nvgFillPaint(vg_, nvgLinearGradient(vg_, x0, 0, x1, 0, nvgRGBAf(0, 0, 0, from),
                                                nvgRGBAf(0, 0, 0, to)));
            nvgFill(vg_);
        };
        shade(0, kScreenWidth * 0.6f, 0.92f, 0.55f);
        shade(kListLeft - units(8.0f), kListLeft, 0.0f, 0.62f);
    }
    draw_about();
    draw_list();
}
} // namespace ui
