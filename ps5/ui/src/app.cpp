#include "app.hpp"

#include <algorithm>
#include <cmath>

#include "details_screen.hpp"
#include "draw_util.hpp"
#include "icons.hpp"
#include "images.hpp"
#include "languages.hpp"
#include "nanovg.h"
#include "player_screen.hpp"
#include "theme.hpp"

namespace ui
{
using namespace theme;

namespace
{
struct Tab
{
    Icon icon;
    const char *label;
};

constexpr Tab kTabs[] = {
    {Icon::Board, "Board"},     {Icon::Discover, "Discover"}, {Icon::Library, "Library"},
    {Icon::Calendar, "Calendar"}, {Icon::Addons, "Addons"},   {Icon::Settings, "Settings"},
};
constexpr int kTabCount = static_cast<int>(sizeof kTabs / sizeof kTabs[0]);

} // namespace

App::App(NVGcontext *context, const std::string &font_folder, const std::string &image_folder)
    : vg_{context}, images_{std::make_unique<Images>(context, image_folder)}
{
    const auto font = [&](const char *name, const char *file) {
        nvgCreateFont(vg_, name, (font_folder + "/" + file).c_str());
    };
    font("regular", "PlusJakartaSans-Regular.ttf");
    font("medium", "PlusJakartaSans-Medium.ttf");
    font("semibold", "PlusJakartaSans-SemiBold.ttf");
    font("bold", "PlusJakartaSans-Bold.ttf");

    player_ = std::make_unique<PlayerScreen>(vg_);
    details_ = std::make_unique<DetailsScreen>(vg_, *images_);
    details_->set_callbacks({
        [this](const std::string &video) {
            if (title_handler_.select_video)
                title_handler_.select_video(title_type_, title_id_, video);
        },
        [this](const Stream &stream, const std::string &title, const std::string &video) {
            player_open_ = true;
            player_->open(title);
            if (title_handler_.play)
                title_handler_.play(stream, title, title_type_, title_id_,
                                    video.empty() ? title_id_ : video);
        },
        [this] {
            title_open_ = false;
            if (title_handler_.close)
                title_handler_.close();
        },
    });
}

App::~App() = default;

void App::set_image_fetcher(
    std::function<void(const std::string &address, const std::string &file)> fetch,
    std::function<bool(const std::string &address)> failed)
{
    images_->set_fetcher(std::move(fetch), std::move(failed));
}

void App::set_title_handler(TitleHandler handler)
{
    title_handler_ = std::move(handler);
}

void App::set_details(Details details)
{
    if (title_open_)
        details_->set_details(std::move(details));
    else if (veil_rising_ && details.id == pending_id_)
        pending_known_ = std::move(details);
}

void App::set_account(Account account)
{
    account_ = std::move(account);
}

void App::set_intent_handler(std::function<void(Intent)> handler)
{
    intent_ = std::move(handler);
}

// Moves the focus from the sidebar into the selected tab's screen.
void App::enter_tab()
{
    selected_tab_ = navigation_focus_;
    if (kTabs[selected_tab_].icon == Icon::Board)
    {
        if (!rows_.empty())
            zone_ = Zone::Rows;
    }
    else if (kTabs[selected_tab_].icon == Icon::Settings)
    {
        zone_ = Zone::Content;
    }
}

void App::set_board(std::vector<BoardRow> rows)
{
    // The board is refreshed as each row finishes loading. While the set of rows stays the
    // same the focus and scroll positions are kept; they are only reset for a new set.
    const bool same_rows = rows.size() == rows_.size();
    rows_ = std::move(rows);
    if (!same_rows)
    {
        column_focus_.assign(rows_.size(), 0);
        scroll_x_.assign(rows_.size(), 0.0f);
        scroll_x_target_.assign(rows_.size(), 0.0f);
    }
    for (std::size_t row = 0; row < rows_.size(); ++row)
        column_focus_[row] =
            std::min(column_focus_[row], rows_[row].items.empty() ? 0 : rows_[row].items.size() - 1);
    row_focus_ = std::min(row_focus_, rows_.empty() ? 0 : rows_.size() - 1);
    const float pulse = focus_pulse_;
    follow_focus();
    if (same_rows)
        focus_pulse_ = pulse; // a refresh is not a focus move
}

float App::card_width(const BoardRow &row) const
{
    const float poster = row.shape == PosterShape::Landscape
                             ? kPosterWidth * kPosterRatio / kLandscapeRatio * 0.62f
                             : kPosterWidth;
    return poster + 2 * kCardPadding;
}

float App::poster_height(const BoardRow &row) const
{
    const float width = card_width(row) - 2 * kCardPadding;
    switch (row.shape)
    {
    case PosterShape::Square:
        return width;
    case PosterShape::Landscape:
        return width * kLandscapeRatio;
    case PosterShape::Poster:
        break;
    }
    return width * kPosterRatio;
}

float App::row_height(const BoardRow &row) const
{
    return kRowTitleSize * 1.2f + kRowTitleGap + 2 * kCardPadding + poster_height(row) +
           kCardTitleHeight + kRowGap;
}

float App::row_top(std::size_t index) const
{
    float top = 0;
    for (std::size_t row = 0; row < index && row < rows_.size(); ++row)
        top += row_height(rows_[row]);
    return top;
}

// Sets the scroll targets so the focused row sits at the top of the content area and the
// focused card is fully inside it.
void App::follow_focus()
{
    focus_pulse_ = 0;
    if (rows_.empty())
        return;
    scroll_y_target_ = row_top(row_focus_);

    const BoardRow &row = rows_[row_focus_];
    const float visible = kScreenWidth - kNavWidth - 2 * kContentInset;
    const float width = card_width(row);
    const float left = static_cast<float>(column_focus_[row_focus_]) * width;
    float &target = scroll_x_target_[row_focus_];
    if (left < target)
        target = left;
    else if (left + width > target + visible)
        target = left + width - visible;
}

void App::set_player_handler(PlayerHandler handler)
{
    player_handler_ = handler;
    player_->set_handler(std::move(handler));
}

void App::set_playback(const Playback &playback)
{
    if (!player_open_)
        return;
    player_->set_playback(playback);
    // A video that has run to its end goes back to the page it was started from.
    if (playback.state == Playback::State::Ended)
        close_player();
}

void App::set_player_tracks(PlayerTracks tracks)
{
    if (player_open_)
        player_->set_tracks(std::move(tracks));
}

void App::set_languages_handler(
    std::function<void(const std::string &audio, const std::string &subtitles)> handler)
{
    languages_ = std::move(handler);
}

// Settings: moves a language preference to the next or the previous choice. Subtitles
// have "off" as their first choice.
void App::change_language(bool subtitles, int step)
{
    constexpr int kCount = static_cast<int>(std::size(kLanguages));
    std::string &code = subtitles ? account_.subtitles_language : account_.audio_language;
    const Language *current = find_language(code);
    // Positions: for subtitles 0 is off and the languages follow.
    const int choices = kCount + (subtitles ? 1 : 0);
    int position = current != nullptr ? static_cast<int>(current - kLanguages) + (subtitles ? 1 : 0) : 0;
    position = ((position + step) % choices + choices) % choices;
    if (subtitles)
        code = position == 0 ? "" : kLanguages[position - 1].code;
    else
        code = kLanguages[position].code;
    if (languages_)
        languages_(account_.audio_language, account_.subtitles_language);
}

void App::close_player()
{
    player_open_ = false;
    player_leaving_ = 1.0f;
    if (player_handler_.close)
        player_handler_.close();
}

void App::set_sound_handler(std::function<void(Sound)> handler)
{
    sound_ = std::move(handler);
}

std::size_t App::focus_mark() const
{
    std::size_t mark = static_cast<std::size_t>(zone_);
    const auto add = [&mark](std::size_t value) { mark = mark * 1000003u + value; };
    add(static_cast<std::size_t>(navigation_focus_));
    add(static_cast<std::size_t>(selected_tab_));
    add(row_focus_);
    add(row_focus_ < column_focus_.size() ? column_focus_[row_focus_] : 0);
    add(title_open_);
    add(veil_rising_);
    add(title_open_ ? details_->focus_mark() : 0);
    add(static_cast<std::size_t>(content_focus_));
    add(std::hash<std::string>{}(account_.audio_language + '/' + account_.subtitles_language));
    return mark;
}

void App::press(Button button)
{
    if (player_open_)
    {
        // No sound effects over a video.
        if (!player_->press(button))
            close_player();
        return;
    }
    const std::size_t before = focus_mark();
    apply(button);
    if (!sound_)
        return;
    // A press that got nowhere (the end of a row, say) is silent.
    const bool changed = focus_mark() != before;
    if (button == Button::Accept)
        sound_(Sound::Select);
    else if (changed)
        sound_(button == Button::Back ? Sound::Back : Sound::Move);
}

void App::apply(Button button)
{
    if (title_open_)
    {
        details_->press(button);
        return;
    }
    if (veil_rising_)
        return; // on the way to a title's page
    switch (zone_)
    {
    case Zone::Navigation:
        if (button == Button::Up && navigation_focus_ > 0)
            --navigation_focus_;
        else if (button == Button::Down && navigation_focus_ < kTabCount - 1)
            ++navigation_focus_;
        else if (button == Button::Right || button == Button::Accept)
            enter_tab();
        break;
    case Zone::Content:
        if (button == Button::Up && content_focus_ > 0)
            --content_focus_;
        else if (button == Button::Down && content_focus_ < 2)
            ++content_focus_;
        else if (content_focus_ > 0 && (button == Button::Left || button == Button::Right ||
                                        button == Button::Accept))
            change_language(content_focus_ == 2, button == Button::Left ? -1 : 1);
        else if (button == Button::Left || button == Button::Back)
        {
            zone_ = Zone::Navigation;
            navigation_focus_ = selected_tab_;
        }
        else if (button == Button::Accept && intent_)
        {
            // The Settings screen's one button does whatever the account's state calls for.
            if (account_.signed_in)
                intent_(Intent::SignOut);
            else if (account_.link == Account::Link::Idle || account_.link == Account::Link::Error)
                intent_(Intent::SignIn);
            else
                intent_(Intent::CancelSignIn);
        }
        break;
    case Zone::Search:
        if (button == Button::Down && !rows_.empty())
            zone_ = Zone::Rows;
        else if (button == Button::Left)
            zone_ = Zone::Navigation;
        break;
    case Zone::Rows:
    {
        if (rows_.empty())
        {
            zone_ = Zone::Navigation;
            break;
        }
        std::size_t &column = column_focus_[row_focus_];
        if (button == Button::Left)
        {
            if (column > 0)
                --column;
            else
            {
                zone_ = Zone::Navigation;
                navigation_focus_ = selected_tab_;
            }
        }
        else if (button == Button::Right && column + 1 < rows_[row_focus_].items.size())
            ++column;
        else if (button == Button::Up)
        {
            if (row_focus_ > 0)
                --row_focus_;
            else
                zone_ = Zone::Search;
        }
        else if (button == Button::Down && row_focus_ + 1 < rows_.size())
            ++row_focus_;
        else if (button == Button::Accept)
        {
            // Open the focused title's page.
            if (const BoardItem *item = focused_item())
            {
                title_type_ = item->type;
                title_id_ = item->id;
                // The board fades out first; the page opens once it has (see update).
                veil_rising_ = true;
                pending_type_ = item->type;
                pending_id_ = item->id;
                pending_name_ = item->name;
                pending_video_ = item->video;
                // What the board already knows shows at once; the rest follows.
                Details known;
                known.type = item->type;
                known.id = item->id;
                known.name = item->name;
                known.description = item->description;
                known.background = item->background;
                known.logo = item->logo;
                known.release_info = item->release_info;
                known.runtime = item->runtime;
                known.imdb_rating = item->imdb_rating;
                known.genres = item->genres;
                known.streams_loading = 1;
                pending_known_ = std::move(known);
                if (title_handler_.open)
                    title_handler_.open(item->type, item->id);
            }
        }
        else if (button == Button::Back)
        {
            zone_ = Zone::Navigation;
            navigation_focus_ = selected_tab_;
        }
        break;
    }
    }
    follow_focus();
}

void App::update(float seconds)
{
    if (player_open_)
        player_->update(seconds);
    else
        player_leaving_ = std::max(0.0f, player_leaving_ - seconds / 0.35f);
    if (title_open_)
        details_->update(seconds);
    constexpr float kVeilTime = 0.12f;
    if (veil_rising_)
    {
        veil_ = std::min(1.0f, veil_ + seconds / kVeilTime);
        if (veil_ >= 1.0f)
        {
            veil_rising_ = false;
            title_open_ = true;
            details_->open(pending_type_, pending_id_, pending_name_, pending_video_);
            details_->set_details(std::move(pending_known_));
        }
    }
    else if (!title_open_)
    {
        veil_ = std::max(0.0f, veil_ - seconds / kVeilTime);
    }

    scroll_y_ = eased(scroll_y_, scroll_y_target_, seconds);
    for (std::size_t row = 0; row < scroll_x_.size(); ++row)
        scroll_x_[row] = eased(scroll_x_[row], scroll_x_target_[row], seconds);
    focus_pulse_ = eased(focus_pulse_, 1.0f, seconds);
    frame_seconds_ = seconds;

    // The featured area follows the focus at a remove. It keeps showing its item until
    // the focus has rested on another one for a moment (so moving along a row does not
    // make it flicker), fades out, switches, and fades the new item in.
    const BoardItem *item = focused_item();
    const std::string target = item != nullptr ? item->id : std::string{};
    if (target != hero_target_)
    {
        hero_target_ = target;
        hero_dwell_ = 0;
    }
    else
    {
        hero_dwell_ += seconds;
    }
    const bool stale = hero_valid_ ? hero_.id != hero_target_ : item != nullptr;
    if (stale && hero_dwell_ >= kHeroDwell)
    {
        const float step = seconds / kHeroFadeOut;
        hero_alpha_ = std::max(0.0f, hero_alpha_ - step);
        hero_logo_alpha_ = std::max(0.0f, hero_logo_alpha_ - step);
        hero_art_alpha_ = std::max(0.0f, hero_art_alpha_ - step);
        if (hero_alpha_ <= 0 && hero_logo_alpha_ <= 0 && hero_art_alpha_ <= 0)
        {
            hero_valid_ = item != nullptr;
            if (item != nullptr)
                hero_ = *item;
            hero_logo_wait_ = 0;
            hero_named_ = false;
        }
    }
    else if (!stale && hero_valid_)
    {
        hero_alpha_ = std::min(1.0f, hero_alpha_ + seconds / kHeroFadeIn);
    }

    // Artwork the featured area is likely to need next is downloaded ahead of time: the
    // focused row's neighbouring items, and the items the rows above and below would
    // land on.
    if (row_focus_ < rows_.size())
    {
        const auto warm = [&](std::size_t row, std::size_t first, std::size_t count) {
            const auto &items = rows_[row].items;
            for (std::size_t column = first; column < items.size() && column < first + count; ++column)
            {
                images_->prefetch(items[column].logo);
                images_->prefetch(items[column].background);
            }
        };
        const std::size_t column = column_focus_[row_focus_];
        warm(row_focus_, column > 2 ? column - 2 : 0, 9);
        if (row_focus_ > 0)
            warm(row_focus_ - 1, column_focus_[row_focus_ - 1], 2);
        if (row_focus_ + 1 < rows_.size())
            warm(row_focus_ + 1, column_focus_[row_focus_ + 1], 2);
    }
    for (int index = 0; index < kTabCount; ++index)
    {
        const bool focused = zone_ == Zone::Navigation && index == navigation_focus_;
        navigation_reveal_[index] = eased(navigation_reveal_[index], focused ? 1.0f : 0.0f, seconds);
    }
}

void App::draw_navigation()
{
    // The icons sit as a group in the middle of the screen's height. Each icon is drawn a
    // little above its button's centre (its name appears beneath it), so the buttons start
    // that much lower for the icons themselves to be centred.
    const float group = kTabCount * kNavButton + (kTabCount - 1) * kNavGap;
    float top = (kScreenHeight - group) / 2 + kNavIconRise;
    const float x = (kNavWidth - kNavButton) / 2;
    for (int index = 0; index < kTabCount; ++index)
    {
        const bool selected = index == selected_tab_;
        const bool focused = zone_ == Zone::Navigation && index == navigation_focus_;
        const float reveal = navigation_reveal_[index];
        if (reveal > 0.01f)
        {
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, x, top, kNavButton, kNavButton, kRadius);
            nvgFillColor(vg_, overlay(2.0f * reveal));
            nvgFill(vg_);
        }
        if (focused)
            focus_ring(vg_, x, top, kNavButton, kNavButton, kRadius);

        // As in Stremio: the selected tab is tinted with the accent colour, the others are
        // dim, and a tab's name appears only while the focus is on it. The icon keeps its
        // place; the name fades in beneath it.
        const float centre_x = x + kNavButton / 2;
        const float icon_y = top + kNavButton / 2 - kNavIconRise;
        const NVGcolor icon_color =
            selected ? accent() : foreground_solid(0.35f + (0.9f - 0.35f) * reveal);
        draw_icon(vg_, kTabs[index].icon, centre_x, icon_y, kNavIcon, icon_color);
        if (reveal > 0.01f)
        {
            nvgFontFace(vg_, "medium");
            nvgFontSize(vg_, kNavLabelSize);
            nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            nvgFillColor(vg_, selected ? accent(reveal) : foreground(0.9f * reveal));
            // The name also rises a little into place as it appears.
            nvgText(vg_, centre_x, icon_y + kNavIcon / 2 + units(0.45f) + units(0.3f) * (1 - reveal),
                    kTabs[index].label, nullptr);
        }
        top += kNavButton + kNavGap;
    }
}

void App::draw_top_bar()
{
    const float x = (kScreenWidth - kSearchWidth) / 2;
    const float y = (kTopBarHeight - kSearchHeight) / 2;
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, x, y, kSearchWidth, kSearchHeight, kSearchHeight / 2);
    nvgFillColor(vg_, overlay(zone_ == Zone::Search ? 2.0f : 1.0f));
    nvgFill(vg_);
    if (zone_ == Zone::Search)
        focus_ring(vg_, x, y, kSearchWidth, kSearchHeight, kSearchHeight / 2);

    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, kSearchTextSize);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg_, foreground(0.6f));
    nvgText(vg_, x + units(1.5f), y + kSearchHeight / 2, "Search or paste link", nullptr);
    draw_icon(vg_, Icon::Search, x + kSearchWidth - units(2.0f), y + kSearchHeight / 2,
              units(1.4f), foreground_solid(0.62f));
}

void App::draw_row(const BoardRow &row, std::size_t index, float top)
{
    const float left = kNavWidth + kContentInset;
    const bool row_focused = zone_ == Zone::Rows && index == row_focus_;

    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, kRowTitleSize);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg_, foreground());
    nvgText(vg_, left + kCardPadding, top, row.title.c_str(), nullptr);

    const float cards_top = top + kRowTitleSize * 1.2f + kRowTitleGap;
    const float width = card_width(row);
    const float poster_width = width - 2 * kCardPadding;
    const float height = poster_height(row);

    if (row.items.empty())
    {
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.3f));
        nvgFillColor(vg_, foreground(0.6f));
        nvgText(vg_, left + kCardPadding, cards_top + kCardPadding,
                row.loading ? "Loading" : row.error.c_str(), nullptr);
        return;
    }

    // Only the cards on screen are drawn. The focused one goes last so its ring and its
    // slightly larger poster sit above its neighbours.
    const float scroll = scroll_x_[index];
    const auto first = static_cast<std::size_t>(std::max(0.0f, std::floor(scroll / width) - 1));
    const std::size_t last = std::min(
        row.items.size(), first + static_cast<std::size_t>(kScreenWidth / width) + 3);
    const std::size_t focused = row_focused ? column_focus_[index] : row.items.size();

    const auto draw_card = [&](std::size_t column) {
        const BoardItem &item = row.items[column];
        const bool is_focused = column == focused;
        float x = left + static_cast<float>(column) * width - scroll + kCardPadding;
        float y = cards_top + kCardPadding;
        float w = poster_width, h = height;
        if (is_focused)
        {
            const float scale = 1.0f + (kFocusScale - 1.0f) * focus_pulse_;
            x -= w * (scale - 1) / 2;
            y -= h * (scale - 1) / 2;
            w *= scale;
            h *= scale;
        }

        const Images::Texture texture = images_->get(item.poster, kPosterPixels);
        if (texture.state == Images::State::Ready)
        {
            cover_image(vg_, x, y, w, h, kRadius, texture);
        }
        else
        {
            // No image (yet): Stremio's placeholder wash with the title on it.
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, x, y, w, h, kRadius);
            nvgFillColor(vg_, overlay(1.6f));
            nvgFill(vg_);
            nvgFontFace(vg_, "medium");
            nvgFontSize(vg_, kCardTitleSize);
            nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg_, foreground(0.5f));
            nvgTextBox(vg_, x + units(0.5f), y + h / 2, w - units(1.0f), item.name.c_str(), nullptr);
        }
        if (item.progress >= 0)
        {
            // How far through it was left, along the poster's foot.
            const float inset = units(0.5f), bar = units(0.32f);
            const float track = w - 2 * inset, bar_top = y + h - inset - bar;
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, x + inset, bar_top, track, bar, bar / 2);
            nvgFillColor(vg_, nvgRGBAf(0, 0, 0, 0.65f));
            nvgFill(vg_);
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, x + inset, bar_top, std::max(bar, track * item.progress), bar, bar / 2);
            nvgFillColor(vg_, accent());
            nvgFill(vg_);
        }
        if (is_focused)
            focus_ring(vg_, x, y, w, h, kRadius);

        nvgFontFace(vg_, "medium");
        nvgFontSize(vg_, kCardTitleSize);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, foreground(is_focused ? 1.0f : 0.9f));
        const float title_x = left + static_cast<float>(column) * width - scroll + kCardPadding;
        fitted_text(vg_, title_x + units(0.2f),
                    cards_top + 2 * kCardPadding + height + kCardTitleHeight / 2,
                    poster_width - units(0.4f), item.name);
    };

    for (std::size_t column = first; column < last; ++column)
        if (column != focused)
            draw_card(column);
    if (focused >= first && focused < last)
        draw_card(focused);
}

const BoardItem *App::focused_item() const
{
    if (row_focus_ >= rows_.size())
        return nullptr;
    const BoardRow &row = rows_[row_focus_];
    const std::size_t column = column_focus_[row_focus_];
    return column < row.items.size() ? &row.items[column] : nullptr;
}

// The featured area: the focused item's background artwork on the right, fading into the
// black on its left and lower edges, with its title, key facts and description on the left.
void App::draw_hero()
{
    if (!hero_valid_)
        return;
    const BoardItem *item = &hero_;
    const float fade = hero_alpha_;
    // Artwork and logo fade in on their own once they have arrived, and only while the
    // area is showing the item they belong to (not while it is fading out to switch).
    const bool current = hero_.id == hero_target_;
    const float rise = frame_seconds_ / kHeroFadeIn;

    // Artwork, 16:9 at the area's full height, against the right edge.
    const float image_height = kHeroHeight;
    const float image_width = image_height * 16.0f / 9.0f;
    const float image_left = kScreenWidth - image_width;
    const Images::Texture background = images_->get(item->background, kArtPixels);
    if (background.state == Images::State::Ready)
    {
        if (current)
            hero_art_alpha_ = std::min(1.0f, hero_art_alpha_ + rise);
        nvgGlobalAlpha(vg_, hero_art_alpha_);
        cover_image(vg_, image_left, 0, image_width, image_height, 0, background);
        nvgGlobalAlpha(vg_, 1.0f);
        const auto shade = [&](float x0, float y0, float x1, float y1, float from, float to) {
            nvgBeginPath(vg_);
            nvgRect(vg_, image_left - 1, 0, image_width + 2, image_height + 1);
            nvgFillPaint(vg_, nvgLinearGradient(vg_, x0, y0, x1, y1, nvgRGBAf(0, 0, 0, from),
                                                nvgRGBAf(0, 0, 0, to)));
            nvgFill(vg_);
        };
        // Into the black on the left, into the rows below, and a little under the search bar.
        shade(image_left, 0, image_left + image_width * 0.55f, 0, 1.0f, 0.0f);
        shade(0, image_height * 0.55f, 0, image_height, 0.0f, 1.0f);
        shade(0, 0, 0, kTopBarHeight * 1.2f, 0.55f, 0.0f);
    }

    const float left = kNavWidth + kContentInset + kCardPadding;
    float top = kHeroTextTop;

    // The title: the item's logo artwork when it has one. While the logo is on its way
    // the space stays empty; the name is written out only for an item that has no logo,
    // whose logo could not be had or has nothing in it, or whose logo is taking too long.
    const Images::Texture logo = images_->get(item->logo, kLogoPixels);
    if (logo.state == Images::State::Pending && current)
        hero_logo_wait_ += frame_seconds_;
    const bool give_up = logo.state == Images::State::Pending && hero_logo_wait_ > kHeroLogoWait;
    if (logo.state == Images::State::Ready && !hero_named_)
    {
        if (current)
            hero_logo_alpha_ = std::min(1.0f, hero_logo_alpha_ + rise);
        draw_logo(vg_, left, top, kHeroLogoWidth, kHeroLogoHeight, logo, hero_logo_alpha_);
    }
    else if (logo.state == Images::State::Unavailable || give_up || hero_named_)
    {
        // Once the name has been shown for this item it stays, even if the logo turns up.
        hero_named_ = true;
        nvgFontFace(vg_, "bold");
        nvgFontSize(vg_, kHeroTitleSize);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        nvgFillColor(vg_, foreground(fade));
        fitted_text(vg_, left, top + kHeroLogoHeight, kHeroTextWidth, item->name);
    }
    top += kHeroLogoHeight + units(1.2f);

    // Key facts on one line: rating, year, running time, genres.
    nvgFontSize(vg_, kHeroMetaSize);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    float x = left;
    const float middle = top + kHeroMetaSize / 2;
    const auto fact = [&](const char *face, NVGcolor color, const std::string &text) {
        if (text.empty())
            return;
        if (x > left)
        {
            nvgFontFace(vg_, "regular");
            nvgFillColor(vg_, foreground(0.35f * fade));
            x = nvgText(vg_, x + units(0.6f), middle, "\xC2\xB7", nullptr) + units(0.6f);
        }
        nvgFontFace(vg_, face);
        nvgFillColor(vg_, color);
        x = nvgText(vg_, x, middle, text.c_str(), nullptr);
    };
    if (!item->imdb_rating.empty())
    {
        // "IMDb" as a small badge, then the score.
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
        x = nvgText(vg_, x + badge + units(0.5f), middle, item->imdb_rating.c_str(), nullptr);
    }
    fact("medium", foreground(0.8f * fade), item->release_info);
    fact("medium", foreground(0.8f * fade), item->runtime);
    std::string genres;
    for (const std::string &genre : item->genres)
        genres += (genres.empty() ? "" : ", ") + genre;
    fact("medium", foreground(0.8f * fade), genres);
    top += kHeroMetaSize + units(1.0f);

    // The description, at most three lines.
    if (!item->description.empty())
    {
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, kHeroDescriptionSize);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground(0.7f * fade));
        NVGtextRow lines[4];
        const int count = nvgTextBreakLines(vg_, item->description.c_str(), nullptr,
                                            kHeroTextWidth, lines, 4);
        const float line_height = kHeroDescriptionSize * 1.45f;
        for (int index = 0; index < count && index < 3; ++index)
        {
            // When there is more than fits, the third line runs on into the rest of the
            // text and is cut off with an ellipsis.
            const bool cut = index == 2 && count > 3;
            if (cut)
                fitted_text(vg_, left, top, kHeroTextWidth, std::string{lines[index].start});
            else
                nvgText(vg_, left, top, lines[index].start, lines[index].end);
            top += line_height;
        }
    }
}

void App::draw_rows()
{
    // Rows are clipped to the area right of the navigation column.
    // The rows live under the featured area; what scrolls above that line is cut off.
    nvgSave(vg_);
    nvgScissor(vg_, kNavWidth, kHeroHeight, kScreenWidth - kNavWidth, kScreenHeight - kHeroHeight);
    float top = kHeroHeight + units(0.4f) - scroll_y_;
    for (std::size_t index = 0; index < rows_.size(); ++index)
    {
        const float height = row_height(rows_[index]);
        if (top + height > 0 && top < kScreenHeight)
            draw_row(rows_[index], index, top);
        top += height;
    }
    nvgRestore(vg_);
}

void App::draw_settings()
{
    const float left = kNavWidth + kContentInset + kCardPadding;
    float top = kTopBarHeight + units(0.5f);

    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, kRowTitleSize);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg_, foreground());
    nvgText(vg_, left, top, "Account", nullptr);
    top += kRowTitleSize * 1.2f + units(1.2f);

    const auto line = [&](const char *face, float size, NVGcolor color, const std::string &text) {
        nvgFontFace(vg_, face);
        nvgFontSize(vg_, size);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, color);
        nvgText(vg_, left, top, text.c_str(), nullptr);
        top += size * 1.5f;
    };

    const char *button = "Sign in";
    if (account_.signed_in)
    {
        line("regular", units(1.1f), foreground(0.6f), "Signed in as");
        line("semibold", units(1.6f), foreground(), account_.email);
        top += units(0.4f);
        line("regular", units(1.1f), foreground(0.6f),
             std::to_string(account_.addons) + " add-ons installed");
        button = "Sign out";
    }
    else
    {
        switch (account_.link)
        {
        case Account::Link::Idle:
            line("regular", units(1.2f), foreground(0.75f),
                 "Sign in to use your add-ons, library and watch progress.");
            break;
        case Account::Link::Requesting:
            line("regular", units(1.2f), foreground(0.75f), "Getting a code from Stremio");
            button = "Cancel";
            break;
        case Account::Link::Waiting:
        {
            // The page's address is shown without its scheme, as a person would type it.
            std::string page = account_.link_page;
            if (const auto scheme = page.find("://"); scheme != std::string::npos)
                page.erase(0, scheme + 3);
            line("regular", units(1.2f), foreground(0.75f),
                 "On a phone or computer where you are signed in to Stremio, open");
            top += units(0.3f);
            line("semibold", units(2.0f), accent(), page);
            top += units(0.6f);
            line("regular", units(1.2f), foreground(0.75f), "and check that it shows this code:");
            top += units(0.4f);

            // The code, one box per character.
            nvgFontFace(vg_, "bold");
            nvgFontSize(vg_, units(3.2f));
            nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            const float box = units(4.6f), gap = units(0.8f);
            float x = left;
            for (const char letter : account_.code)
            {
                nvgBeginPath(vg_);
                nvgRoundedRect(vg_, x, top, box, box * 1.15f, kRadius);
                nvgFillColor(vg_, overlay(1.6f));
                nvgFill(vg_);
                const char text[2] = {letter, 0};
                nvgFillColor(vg_, foreground(1.0f));
                nvgText(vg_, x + box / 2, top + box * 1.15f / 2, text, nullptr);
                x += box + gap;
            }
            top += box * 1.15f + units(1.2f);
            line("regular", units(1.1f), foreground(0.6f),
                 "This screen signs in by itself once you confirm it there.");
            button = "Cancel";
            break;
        }
        case Account::Link::SigningIn:
            line("regular", units(1.2f), foreground(0.75f), "Signing in");
            button = "Cancel";
            break;
        case Account::Link::Error:
            line("regular", units(1.2f), foreground(0.75f), "Signing in did not work:");
            line("regular", units(1.1f), foreground(0.6f), account_.error);
            button = "Try again";
            break;
        }
    }

    top += units(1.0f);
    const float width = units(11.0f), height = units(3.25f);
    const bool focused = zone_ == Zone::Content && content_focus_ == 0;
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, left, top, width, height, height / 2);
    nvgFillColor(vg_, focused ? accent() : overlay(2.0f));
    nvgFill(vg_);
    if (focused)
        focus_ring(vg_, left, top, width, height, height / 2);
    nvgFontFace(vg_, "semibold");
    nvgFontSize(vg_, units(1.15f));
    nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg_, foreground(1.0f));
    nvgText(vg_, left + width / 2, top + height / 2, button, nullptr);
    top += height + units(2.6f);

    // Playback: the languages picked first for a video's sound and subtitles.
    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, kRowTitleSize);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg_, foreground());
    nvgText(vg_, left, top, "Playback", nullptr);
    top += kRowTitleSize * 1.2f + units(1.0f);
    const auto choice = [&](int index, const char *label, const std::string &value) {
        const bool chosen = zone_ == Zone::Content && content_focus_ == index;
        const float row_width = units(30.0f), row_height = units(3.25f);
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, left, top, row_width, row_height, kRadius);
        nvgFillColor(vg_, overlay(chosen ? 3.0f : 1.6f));
        nvgFill(vg_);
        if (chosen)
            focus_ring(vg_, left, top, row_width, row_height, kRadius);
        const float middle = top + row_height / 2;
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.15f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, foreground(0.75f));
        nvgText(vg_, left + units(1.2f), middle, label, nullptr);
        nvgFontFace(vg_, "semibold");
        nvgTextAlign(vg_, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, foreground(1.0f));
        // Arrows either side of the value show that left and right change it.
        const std::string shown = chosen ? "\xE2\x80\xB9   " + value + "   \xE2\x80\xBA" : value;
        nvgText(vg_, left + row_width - units(1.2f), middle, shown.c_str(), nullptr);
        top += row_height + units(0.6f);
    };
    choice(1, "Audio language", language_name(account_.audio_language));
    choice(2, "Subtitles",
           account_.subtitles_language.empty() ? std::string{"Off"}
                                               : language_name(account_.subtitles_language));
}

void App::draw_unbuilt_tab()
{
    nvgFontFace(vg_, "regular");
    nvgFontSize(vg_, units(1.3f));
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg_, foreground(0.5f));
    const std::string text = std::string{kTabs[selected_tab_].label} + " is not built yet.";
    nvgText(vg_, kNavWidth + kContentInset + kCardPadding, kTopBarHeight + units(0.5f),
            text.c_str(), nullptr);
}

void App::draw(int width, int height)
{
    images_->begin_frame();
    nvgBeginFrame(vg_, kScreenWidth, kScreenHeight, static_cast<float>(width) / kScreenWidth);
    (void)height;
    if (player_open_)
    {
        // The host has drawn the picture; only the controls go over it.
        player_->draw();
        nvgEndFrame(vg_);
        return;
    }

    nvgBeginPath(vg_);
    nvgRect(vg_, 0, 0, kScreenWidth, kScreenHeight);
    nvgFillColor(vg_, background());
    nvgFill(vg_);

    if (title_open_)
    {
        // A title's page takes the whole screen. Coming back from the player it fades in
        // from the black the video left.
        details_->draw();
        if (player_leaving_ > 0.0f)
        {
            const float rest = player_leaving_ * player_leaving_;
            nvgBeginPath(vg_);
            nvgRect(vg_, 0, 0, kScreenWidth, kScreenHeight);
            nvgFillColor(vg_, nvgRGBAf(0, 0, 0, rest));
            nvgFill(vg_);
        }
        nvgEndFrame(vg_);
        return;
    }
    switch (kTabs[selected_tab_].icon)
    {
    case Icon::Board:
        draw_hero();
        draw_rows();
        break;
    case Icon::Settings:
        draw_settings();
        break;
    default:
        draw_unbuilt_tab();
        break;
    }
    draw_top_bar();
    draw_navigation();
    // On the way to or from a title's page the board is under a veil of black.
    if (veil_ > 0.0f)
    {
        nvgBeginPath(vg_);
        nvgRect(vg_, 0, 0, kScreenWidth, kScreenHeight);
        nvgFillColor(vg_, nvgRGBAf(0, 0, 0, veil_));
        nvgFill(vg_);
    }

    nvgEndFrame(vg_);
}
} // namespace ui
