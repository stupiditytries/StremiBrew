// The Discover screen (part of App), after Stremio's: along the top, what is being
// browsed (a kind of title, one of its catalogs, and a genre); under that the catalog as
// a grid of posters that loads further pages as the focus nears its end; and beside the
// grid, the focused title's artwork and details.
//
// Changing what is browsed fades the grid out behind a wheel and brings the new one in
// once its titles and their posters are loaded.

#include "app.hpp"

#include <algorithm>
#include <cmath>

#include "draw_util.hpp"
#include "images.hpp"
#include "nanovg.h"
#include "theme.hpp"

namespace ui
{
using namespace theme;

namespace
{
constexpr float kLeft = kNavWidth + units(1.5f);
constexpr float kTop = kTopBarHeight + units(0.2f);
constexpr float kPillHeight = units(3.0f), kPillWidth = units(13.0f), kPillGap = units(0.8f);
constexpr float kPaneWidth = units(24.0f);
constexpr float kPaneLeft = kScreenWidth - units(1.5f) - kPaneWidth;
constexpr float kGridTop = kTop + kPillHeight + units(1.0f);
constexpr float kGridWidth = kPaneLeft - units(1.5f) - kLeft;
constexpr int kColumns = 6;
constexpr float kCard = kGridWidth / kColumns;
constexpr float kListRow = units(2.6f);
constexpr int kListRows = 10; // of a pill's choices on show at once
constexpr const char *kPillNames[] = {"Type", "Catalog", "Genre"};

int selected_of(const std::vector<Choice> &choices)
{
    for (std::size_t index = 0; index < choices.size(); ++index)
        if (choices[index].selected)
            return static_cast<int>(index);
    return 0;
}
} // namespace

void App::set_discover_handler(std::function<void(int kind, int index)> handler)
{
    discover_handler_ = std::move(handler);
}

void App::set_discover(DiscoverData data)
{
    // Just after something was chosen, what arrives may still be the catalog before it.
    if (discover_ignore_ > 0)
        return;
    discover_ = std::move(data);
    discover_focus_ = std::clamp(discover_focus_, 0, std::max(0, static_cast<int>(discover_.items.size()) - 1));
    if (discover_.items.empty() && discover_area_ == 1)
        discover_area_ = 0;
}

const std::vector<Choice> &App::discover_choices(int pill) const
{
    return pill == 0 ? discover_.types : pill == 1 ? discover_.catalogs : discover_.genres;
}

float App::discover_poster_height() const
{
    const float width = kCard - 2 * kCardPadding;
    return discover_.shape == PosterShape::Square      ? width
           : discover_.shape == PosterShape::Landscape ? width * kLandscapeRatio
                                                       : width * kPosterRatio;
}

// Starts the wait for a newly chosen catalog: the grid goes, a wheel takes its place.
void App::discover_wait()
{
    discover_hold_ = true;
    discover_held_ = 0;
    discover_focus_ = 0;
    discover_scroll_ = discover_scroll_target_ = 0;
    discover_more_at_ = 0;
}

void App::press_discover(Button button)
{
    const int pills = discover_.genres.empty() ? 2 : 3;
    if (discover_list_open_)
    {
        // A pill's choices.
        const auto &choices = discover_choices(discover_pill_);
        const int count = static_cast<int>(choices.size());
        if (button == Button::Up && discover_list_focus_ > 0)
            --discover_list_focus_;
        else if (button == Button::Down && discover_list_focus_ + 1 < count)
            ++discover_list_focus_;
        else if (button == Button::Back)
            discover_list_open_ = false;
        else if (button == Button::Accept && discover_list_focus_ < count)
        {
            discover_list_open_ = false;
            if (!choices[static_cast<std::size_t>(discover_list_focus_)].selected && discover_handler_)
            {
                discover_handler_(discover_pill_, discover_list_focus_);
                discover_ignore_ = 0.2f;
                discover_.loading = true;
                discover_wait();
            }
        }
        return;
    }
    if (discover_area_ == 0)
    {
        discover_pill_ = std::clamp(discover_pill_, 0, pills - 1);
        if (button == Button::Left)
        {
            if (discover_pill_ > 0)
                --discover_pill_;
            else
                leave_content();
        }
        else if (button == Button::Right && discover_pill_ + 1 < pills)
            ++discover_pill_;
        else if (button == Button::Down && !discover_.items.empty() && !discover_hold_)
            discover_area_ = 1;
        else if (button == Button::Up)
            zone_ = Zone::Search;
        else if (button == Button::Back)
            leave_content();
        else if (button == Button::Accept && !discover_choices(discover_pill_).empty())
        {
            // The list opens with the present choice under the focus and in view.
            const int total = static_cast<int>(discover_choices(discover_pill_).size());
            discover_list_open_ = true;
            discover_list_pill_ = discover_pill_;
            discover_list_focus_ = selected_of(discover_choices(discover_pill_));
            discover_list_cursor_ = static_cast<float>(discover_list_focus_);
            discover_list_scroll_ = static_cast<float>(
                std::clamp(discover_list_focus_ - kListRows / 2, 0, std::max(0, total - kListRows)));
        }
        return;
    }
    // The grid.
    const int count = static_cast<int>(discover_.items.size());
    if (count == 0)
    {
        discover_area_ = 0;
        return;
    }
    const int column = discover_focus_ % kColumns;
    if (button == Button::Left)
    {
        if (column > 0)
            --discover_focus_;
        else
            leave_content();
    }
    else if (button == Button::Right && column + 1 < kColumns && discover_focus_ + 1 < count)
        ++discover_focus_;
    else if (button == Button::Up)
    {
        if (discover_focus_ >= kColumns)
            discover_focus_ -= kColumns;
        else
            discover_area_ = 0;
    }
    else if (button == Button::Down)
    {
        if (discover_focus_ + kColumns < count)
            discover_focus_ += kColumns;
        else if (discover_focus_ / kColumns < (count - 1) / kColumns)
            discover_focus_ = count - 1;
    }
    else if (button == Button::Back)
        leave_content();
    else if (button == Button::Accept)
        open_title(discover_.items[static_cast<std::size_t>(discover_focus_)]);
    else if (button == Button::Options)
    {
        // What can be done with the focused title, in a menu over its details.
        const BoardItem &item = discover_.items[static_cast<std::size_t>(discover_focus_)];
        menu_item_ = item;
        menu_choices_.clear();
        menu_choices_.push_back(item.in_library
                                    ? std::pair<std::string, LibraryAction>{"Remove from library", LibraryAction::Remove}
                                    : std::pair<std::string, LibraryAction>{"Add to library", LibraryAction::Add});
        menu_focus_ = 0;
        menu_x_ = kPaneLeft + units(1.0f);
        menu_y_ = kGridTop + units(2.0f);
        menu_open_ = true;
    }
}

void App::update_discover(float seconds)
{
    discover_ignore_ = std::max(0.0f, discover_ignore_ - seconds);
    {
        // An open list: how far unrolled it is, where its highlight is on the way to the
        // focused choice, and its scroll, which keeps that choice inside it.
        discover_list_reveal_ = eased(discover_list_reveal_, discover_list_open_ ? 1.0f : 0.0f, seconds);
        discover_list_cursor_ = eased(discover_list_cursor_, static_cast<float>(discover_list_focus_), seconds);
        const int total = static_cast<int>(discover_choices(discover_list_pill_).size());
        const int rows = std::min(total, kListRows);
        float wanted = discover_list_scroll_;
        if (static_cast<float>(discover_list_focus_) < wanted)
            wanted = static_cast<float>(discover_list_focus_);
        else if (static_cast<float>(discover_list_focus_) > wanted + static_cast<float>(rows - 1))
            wanted = static_cast<float>(discover_list_focus_ - rows + 1);
        wanted = std::clamp(std::round(wanted), 0.0f, static_cast<float>(std::max(0, total - rows)));
        if (discover_list_open_)
            discover_list_scroll_ = std::abs(discover_list_scroll_ - wanted) < 0.01f
                                        ? wanted
                                        : eased(discover_list_scroll_, wanted, seconds);
    }
    const bool showing = page() == Page::Discover;
    if (!showing)
        return;
    const int count = static_cast<int>(discover_.items.size());
    if (discover_hold_)
    {
        // Ready is the catalog answered and the first screenful of posters loaded.
        discover_held_ += seconds;
        bool ready = discover_ignore_ <= 0 && !discover_.loading;
        for (int index = 0; ready && index < count && index < kColumns * 3; ++index)
            if (images_->get(discover_.items[static_cast<std::size_t>(index)].poster, kPosterPixels).state ==
                Images::State::Pending)
                ready = false;
        if ((ready && discover_alpha_ <= 0.0f) || discover_held_ > 10.0f)
        {
            discover_hold_ = false;
            if (count > 0 && discover_area_ == 0 && zone_ == Zone::Content && !discover_list_open_ &&
                discover_enter_grid_)
                discover_area_ = 1;
            discover_enter_grid_ = false;
        }
        discover_alpha_ = std::max(0.0f, discover_alpha_ - seconds / 0.15f);
    }
    else
        discover_alpha_ = std::min(1.0f, discover_alpha_ + seconds / 0.3f);

    // More of the catalog as the focus nears the end of what there is.
    if (!discover_hold_ && discover_.more && count > 0 && discover_handler_ &&
        discover_focus_ / kColumns + 4 >= (count - 1) / kColumns && static_cast<std::size_t>(count) != discover_more_at_)
    {
        discover_more_at_ = static_cast<std::size_t>(count);
        discover_handler_(3, 0);
    }

    // The grid scrolls to keep the focused row whole.
    const float row_height = discover_poster_height() + 2 * kCardPadding + kCardTitleHeight;
    const float window = kScreenHeight - kGridTop;
    const float focus_top = static_cast<float>(discover_focus_ / kColumns) * row_height;
    if (focus_top < discover_scroll_target_)
        discover_scroll_target_ = focus_top;
    else if (focus_top + row_height > discover_scroll_target_ + window)
        discover_scroll_target_ = focus_top + row_height - window + units(0.5f);
    if (discover_focus_ < kColumns)
        discover_scroll_target_ = 0;
    discover_scroll_ = eased(discover_scroll_, discover_scroll_target_, seconds);

    // The details beside the grid follow the focus at a short remove, fading from one
    // title to the next.
    const BoardItem *item =
        count > 0 && !discover_hold_ ? &discover_.items[static_cast<std::size_t>(discover_focus_)] : nullptr;
    const std::string target = item != nullptr ? item->id : std::string{};
    if (target != discover_target_)
    {
        discover_target_ = target;
        discover_dwell_ = 0;
    }
    else
        discover_dwell_ += seconds;
    const bool stale = discover_shown_valid_ ? discover_shown_.id != target : item != nullptr;
    if (stale && discover_dwell_ >= 0.18f)
    {
        discover_shown_alpha_ = std::max(0.0f, discover_shown_alpha_ - seconds / 0.12f);
        if (discover_shown_alpha_ <= 0.0f)
        {
            discover_shown_valid_ = item != nullptr;
            if (item != nullptr)
                discover_shown_ = *item;
            discover_shown_wait_ = 0;
            discover_named_ = false;
        }
    }
    else if (!stale && discover_shown_valid_)
    {
        // A title's details come in whole: they wait for its artwork and logo (a while
        // at most), and its name is written out only if there turns out to be no logo.
        const Images::State art = images_->get(discover_shown_.background, kArtPixels).state;
        const Images::State logo = images_->get(discover_shown_.logo, kLogoPixels).state;
        const bool waiting = art == Images::State::Pending || logo == Images::State::Pending;
        if (waiting && discover_shown_alpha_ <= 0.0f && discover_shown_wait_ < kHeroLogoWait)
            discover_shown_wait_ += seconds;
        else
        {
            if (logo != Images::State::Ready && discover_shown_alpha_ <= 0.0f)
                discover_named_ = true;
            else if (logo == Images::State::Unavailable)
                discover_named_ = true;
            discover_shown_alpha_ = std::min(1.0f, discover_shown_alpha_ + seconds / 0.3f);
        }
    }
}

void App::draw_discover()
{
    const bool active = zone_ == Zone::Content;
    const int pills = discover_.genres.empty() ? 2 : 3;

    // The grid, under whatever fade it is in.
    const float poster_width = kCard - 2 * kCardPadding, poster_height = discover_poster_height();
    const float row_height = poster_height + 2 * kCardPadding + kCardTitleHeight;
    const int count = static_cast<int>(discover_.items.size());
    if (discover_alpha_ > 0.0f && count > 0)
    {
        nvgSave(vg_);
        nvgScissor(vg_, kNavWidth, kGridTop - units(0.4f), kPaneLeft - kNavWidth - units(0.6f),
                   kScreenHeight - kGridTop + units(0.4f));
        nvgGlobalAlpha(vg_, discover_alpha_);
        const int first = std::max(0, static_cast<int>(discover_scroll_ / row_height) - 1) * kColumns;
        const int last = std::min(count, first + kColumns * 6);
        const int focused = active && discover_area_ == 1 && !discover_list_open_ ? discover_focus_ : -1;
        const auto card = [&](int index) {
            const BoardItem &item = discover_.items[static_cast<std::size_t>(index)];
            const bool is_focused = index == focused;
            const float cell_x = kLeft + static_cast<float>(index % kColumns) * kCard;
            const float cell_y = kGridTop + static_cast<float>(index / kColumns) * row_height - discover_scroll_;
            float x = cell_x + kCardPadding, y = cell_y + kCardPadding, w = poster_width, h = poster_height;
            if (is_focused)
            {
                const float scale = 1.0f + (kFocusScale - 1.0f) * focus_pulse_;
                x -= w * (scale - 1) / 2;
                y -= h * (scale - 1) / 2;
                w *= scale;
                h *= scale;
            }
            const Images::Texture poster = images_->get(item.poster, kPosterPixels);
            if (poster.state == Images::State::Ready)
                cover_image(vg_, x, y, w, h, kRadius, poster);
            else
            {
                nvgBeginPath(vg_);
                nvgRoundedRect(vg_, x, y, w, h, kRadius);
                nvgFillColor(vg_, overlay(1.6f));
                nvgFill(vg_);
            }
            if (item.in_library)
            {
                const float mark = units(1.0f), mark_x = x + w - units(1.8f);
                nvgBeginPath(vg_);
                nvgMoveTo(vg_, mark_x, y);
                nvgLineTo(vg_, mark_x + mark, y);
                nvgLineTo(vg_, mark_x + mark, y + mark * 1.5f);
                nvgLineTo(vg_, mark_x + mark / 2, y + mark * 1.1f);
                nvgLineTo(vg_, mark_x, y + mark * 1.5f);
                nvgClosePath(vg_);
                nvgFillColor(vg_, accent());
                nvgFill(vg_);
            }
            if (is_focused)
                focus_ring(vg_, x, y, w, h, kRadius);
            nvgFontFace(vg_, "medium");
            nvgFontSize(vg_, kCardTitleSize);
            nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg_, foreground(is_focused ? 1.0f : 0.9f));
            fitted_text(vg_, cell_x + kCardPadding + units(0.2f),
                        cell_y + 2 * kCardPadding + poster_height + kCardTitleHeight / 2, poster_width - units(0.4f),
                        item.name);
        };
        for (int index = first; index < last; ++index)
            if (index != focused)
                card(index);
        if (focused >= first && focused < last)
            card(focused);
        nvgGlobalAlpha(vg_, 1.0f);
        nvgRestore(vg_);
    }
    if (discover_hold_)
        draw_wheel(kLeft + kGridWidth / 2, kGridTop + (kScreenHeight - kGridTop) / 2,
                   std::min(1.0f, discover_held_ / 0.25f) * (1.0f - discover_alpha_));
    else if (count == 0)
    {
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.3f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground(0.55f));
        nvgText(vg_, kLeft + kCardPadding, kGridTop + units(1.0f),
                discover_.error.empty() ? "Nothing in this catalog." : "This catalog could not be loaded.", nullptr);
    }

    // The focused title, beside the grid.
    if (discover_shown_valid_ && discover_shown_alpha_ > 0.0f)
    {
        const BoardItem &item = discover_shown_;
        const float fade = discover_shown_alpha_ * discover_alpha_;
        const float art_height = kPaneWidth * 9.0f / 16.0f;
        const float pane_top = kGridTop + kCardPadding;
        const Images::Texture art = images_->get(item.background, kArtPixels);
        if (art.state == Images::State::Ready)
        {
            cover_image(vg_, kPaneLeft, pane_top, kPaneWidth, art_height, kRadius, art, fade);
            // Into the black at its foot, where the title sits.
            nvgBeginPath(vg_);
            nvgRect(vg_, kPaneLeft - 1, pane_top + art_height * 0.35f, kPaneWidth + 2, art_height * 0.65f + 2);
            nvgFillPaint(vg_, nvgLinearGradient(vg_, 0, pane_top + art_height * 0.35f, 0, pane_top + art_height,
                                                nvgRGBAf(0, 0, 0, 0), nvgRGBAf(0, 0, 0, 1)));
            nvgFill(vg_);
        }
        const float text_x = kPaneLeft + units(0.4f), text_width = kPaneWidth - units(0.8f);
        // The title, under the artwork: its logo, or its name written out when it has no
        // logo that can be drawn (see update_discover for the wait before deciding so).
        const float logo_height = units(4.4f);
        float y = pane_top + art_height + units(0.8f);
        const Images::Texture logo = images_->get(item.logo, kLogoPixels);
        if (!discover_named_ && logo.state == Images::State::Ready &&
            !draw_logo(vg_, text_x, y, kPaneWidth * 0.62f, logo_height, logo, fade))
            discover_named_ = true; // a logo with nothing in it
        if (discover_named_)
        {
            nvgFontFace(vg_, "bold");
            nvgFontSize(vg_, units(1.9f));
            nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
            nvgFillColor(vg_, foreground(fade));
            fitted_text(vg_, text_x, y + logo_height, text_width, item.name);
        }
        y += logo_height + units(2.0f);

        // Its key facts on one line: the IMDb score beside its badge, the year, the
        // running time.
        {
            float x = text_x;
            const float middle = y;
            nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            if (!item.imdb_rating.empty())
            {
                nvgFontFace(vg_, "bold");
                nvgFontSize(vg_, units(0.85f));
                const float badge = nvgTextBounds(vg_, 0, 0, "IMDb", nullptr, nullptr) + units(0.7f);
                nvgBeginPath(vg_);
                nvgRoundedRect(vg_, x, middle - units(0.7f), badge, units(1.4f), units(0.25f));
                nvgFillColor(vg_, nvgTransRGBAf(imdb_yellow(), fade));
                nvgFill(vg_);
                nvgFillColor(vg_, nvgRGBAf(0, 0, 0, fade));
                nvgText(vg_, x + units(0.35f), middle, "IMDb", nullptr);
                nvgFontFace(vg_, "semibold");
                nvgFontSize(vg_, units(1.05f));
                nvgFillColor(vg_, foreground(fade));
                x = nvgText(vg_, x + badge + units(0.5f), middle, item.imdb_rating.c_str(), nullptr);
            }
            for (const std::string *fact : {&item.release_info, &item.runtime})
            {
                if (fact->empty())
                    continue;
                nvgFontSize(vg_, units(1.05f));
                if (x > text_x)
                {
                    nvgFontFace(vg_, "regular");
                    nvgFillColor(vg_, foreground(0.35f * fade));
                    x = nvgText(vg_, x + units(0.6f), middle, "\xC2\xB7", nullptr) + units(0.6f);
                }
                nvgFontFace(vg_, "medium");
                nvgFillColor(vg_, foreground(0.8f * fade));
                x = nvgText(vg_, x, middle, fact->c_str(), nullptr);
            }
        }
        y += units(2.7f);
        if (!item.genres.empty())
        {
            // Its genres, each on a label.
            float x = text_x;
            nvgFontFace(vg_, "medium");
            nvgFontSize(vg_, units(0.9f));
            nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            for (const std::string &genre : item.genres)
            {
                const float width = nvgTextBounds(vg_, 0, 0, genre.c_str(), nullptr, nullptr) + units(1.4f);
                nvgBeginPath(vg_);
                nvgRoundedRect(vg_, x, y - units(0.9f), width, units(1.8f), units(0.35f));
                nvgFillColor(vg_, overlay(2.4f * fade));
                nvgFill(vg_);
                nvgFillColor(vg_, foreground(0.85f * fade));
                nvgText(vg_, x + units(0.7f), y, genre.c_str(), nullptr);
                x += width + units(0.5f);
            }
            y += units(3.0f);
        }
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.0f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground(0.65f * fade));
        y += wrapped_text(vg_, text_x, y, text_width, units(1.6f), 9, drawable(item.description));
        if (item.in_library)
        {
            nvgFontFace(vg_, "medium");
            nvgFontSize(vg_, units(0.95f));
            nvgFillColor(vg_, accent(fade));
            nvgText(vg_, kPaneLeft + units(0.4f), y + units(1.0f), "In your library", nullptr);
        }
    }

    // What is being browsed: a pill for each choice, over everything else so an open
    // list of choices covers the grid.
    for (int pill = 0; pill < pills; ++pill)
    {
        const auto &choices = discover_choices(pill);
        const float x = kLeft + kCardPadding + static_cast<float>(pill) * (kPillWidth + kPillGap);
        const bool focused = active && discover_area_ == 0 && discover_pill_ == pill;
        const bool open = discover_list_open_ && discover_pill_ == pill;
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, x, kTop, kPillWidth, kPillHeight, kPillHeight / 2);
        nvgFillColor(vg_, overlay(focused || open ? 3.0f : 1.6f));
        nvgFill(vg_);
        if (focused && !open)
            focus_ring(vg_, x, kTop, kPillWidth, kPillHeight, kPillHeight / 2);
        const float middle = kTop + kPillHeight / 2;
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(0.8f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, foreground(0.5f));
        const float after = nvgText(vg_, x + units(1.2f), middle + units(0.05f), kPillNames[pill], nullptr);
        nvgFontFace(vg_, "semibold");
        nvgFontSize(vg_, units(1.05f));
        nvgFillColor(vg_, foreground(1.0f));
        const std::string value =
            choices.empty() ? std::string{} : choices[static_cast<std::size_t>(selected_of(choices))].name;
        fitted_text(vg_, after + units(0.6f), middle, x + kPillWidth - units(2.6f) - after - units(0.6f), value);
        // A chevron: there is more to choose from.
        const float tip_x = x + kPillWidth - units(1.5f), reach = units(0.3f);
        nvgBeginPath(vg_);
        const float turned = (discover_list_pill_ == pill ? 1.0f - 2.0f * discover_list_reveal_ : 1.0f) * reach / 2;
        nvgMoveTo(vg_, tip_x - reach, middle - turned);
        nvgLineTo(vg_, tip_x, middle + turned);
        nvgLineTo(vg_, tip_x + reach, middle - turned);
        nvgStrokeColor(vg_, foreground(0.7f));
        nvgStrokeWidth(vg_, units(0.12f));
        nvgStroke(vg_);
    }
    if (discover_list_reveal_ > 0.01f)
    {
        // The open pill's choices, in a panel that unrolls from under the pill (and rolls
        // back up when it is closed). The highlight slides from choice to choice.
        const auto &choices = discover_choices(discover_list_pill_);
        const int total = static_cast<int>(choices.size());
        const int rows = std::min(total, kListRows);
        const float reveal = discover_list_reveal_;
        const float x = kLeft + kCardPadding + static_cast<float>(discover_list_pill_) * (kPillWidth + kPillGap);
        const float y = kTop + kPillHeight + units(0.4f), pad = units(0.6f);
        const float width = kPillWidth + units(4.0f);
        const float full = static_cast<float>(rows) * kListRow + 2 * pad - units(0.2f);
        const float height = full * reveal;
        nvgSave(vg_);
        nvgGlobalAlpha(vg_, reveal);
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, x, y, width, height, kRadius);
        nvgFillColor(vg_, nvgRGBf(0.09f, 0.09f, 0.11f));
        nvgFill(vg_);
        // Everything inside is cut to the panel as far as it has unrolled; the room
        // around the choices is wide enough for the focus outline.
        nvgScissor(vg_, x, y, width, height);
        const float row_height = kListRow - units(0.2f);
        const auto row_top = [&](float index) { return y + pad + (index - discover_list_scroll_) * kListRow; };
        if (discover_list_open_)
        {
            const float top = row_top(discover_list_cursor_);
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, x + pad, top, width - 2 * pad, row_height, kRadius * 0.7f);
            nvgFillColor(vg_, overlay(3.5f));
            nvgFill(vg_);
            focus_ring(vg_, x + pad, top, width - 2 * pad, row_height, kRadius * 0.7f);
        }
        for (int index = 0; index < total; ++index)
        {
            const float top = row_top(static_cast<float>(index));
            if (top + kListRow < y || top > y + height)
                continue;
            const Choice &choice = choices[static_cast<std::size_t>(index)];
            const bool focused = discover_list_open_ && index == discover_list_focus_;
            // A choice scrolled part-way out past the panel's ends fades with how much of
            // it is left, so nothing is sliced through.
            const float inside = std::clamp(std::min(top + row_height - (y + pad * 0.5f), y + full - pad * 0.5f - top) /
                                                row_height,
                                            0.0f, 1.0f);
            nvgFontFace(vg_, choice.selected ? "semibold" : "medium");
            nvgFontSize(vg_, units(1.05f));
            nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            NVGcolor colour = choice.selected ? accent() : foreground(focused ? 1.0f : 0.8f);
            colour.a *= inside;
            nvgFillColor(vg_, colour);
            fitted_text(vg_, x + pad + units(1.0f), top + row_height / 2, width - 2 * pad - units(2.0f), choice.name);
        }
        nvgRestore(vg_);
    }
}
} // namespace ui
