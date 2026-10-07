#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

// The on-screen keyboard: four rows of ten keys, and a row of wider ones under them.
constexpr int kKeyColumns = 10, kKeyRows = 5;
constexpr const char *kKeys[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-"};
// The last row's keys, each with the columns it spans.
struct WideKey
{
    const char *label;
    int first, last;
};
constexpr WideKey kWideKeys[] = {{"Space", 0, 3}, {"Delete", 4, 5}, {"Clear", 6, 7}, {"Done", 8, 9}};
constexpr float kKeyWidth = units(3.4f), kKeyHeight = units(2.8f), kKeyGap = units(0.35f);
constexpr float kKeyboardTop = kTopBarHeight + units(0.9f);
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

void App::set_display_options(bool handheld_ui, bool automatic)
{
    display_options_ = true;
    handheld_ui_ = handheld_ui;
    handheld_auto_ = automatic;
}

void App::set_display_handler(std::function<void(bool handheld_ui, bool automatic)> handler)
{
    display_handler_ = std::move(handler);
}

void App::set_intent_handler(std::function<void(Intent)> handler)
{
    intent_ = std::move(handler);
}

// Moves the focus from the sidebar into the selected tab's screen.
View App::view() const
{
    if (!search_sent_.empty())
        return View::Search;
    return kTabs[selected_tab_].icon == Icon::Library ? View::Library : View::Board;
}

void App::set_library_handler(std::function<void(LibraryAction, const BoardItem &)> handler)
{
    library_handler_ = std::move(handler);
}

void App::set_keyboard_handler(std::function<bool(const std::string &text)> open)
{
    keyboard_handler_ = std::move(open);
}

Page App::page() const
{
    if (view() == View::Search)
        return Page::None;
    const Icon tab = kTabs[selected_tab_].icon;
    return tab == Icon::Calendar   ? Page::Calendar
           : tab == Icon::Addons   ? Page::Addons
           : tab == Icon::Discover ? Page::Discover
                                   : Page::None;
}

void App::submit_search(const std::string &text)
{
    keyboard_open_ = false;
    search_text_ = text;
    if (text == search_sent_ && !swap_pending_)
        return;
    // What is on screen fades out before the search takes its place (see update).
    swap_query_ = text;
    swap_pending_ = true;
    focus_rows_when_ready_ = true;
    if (text.empty() && zone_ == Zone::Rows)
        zone_ = Zone::Search;
}

void App::clear_search()
{
    search_text_.clear();
    search_sent_.clear();
    swap_pending_ = swap_holding_ = focus_rows_when_ready_ = false;
    keyboard_open_ = false;
}

// The on-screen keyboard. Returns false when it is not open.
bool App::press_keyboard(Button button)
{
    if (!keyboard_open_)
        return false;
    const auto wide_at = [](int column) {
        for (int index = 0; index < 4; ++index)
            if (column >= kWideKeys[index].first && column <= kWideKeys[index].last)
                return index;
        return 0;
    };
    switch (button)
    {
    case Button::Left:
        if (key_row_ == 4)
        {
            const int key = wide_at(key_column_);
            if (key > 0)
                key_column_ = kWideKeys[key - 1].first;
        }
        else if (key_column_ > 0)
            --key_column_;
        break;
    case Button::Right:
        if (key_row_ == 4)
        {
            const int key = wide_at(key_column_);
            if (key < 3)
                key_column_ = kWideKeys[key + 1].first;
        }
        else if (key_column_ + 1 < kKeyColumns)
            ++key_column_;
        break;
    case Button::Up:
        if (key_row_ > 0)
            --key_row_;
        break;
    case Button::Down:
        if (key_row_ + 1 < kKeyRows)
            ++key_row_;
        break;
    case Button::Accept:
        if (key_row_ < 4)
        {
            if (search_text_.size() < 60)
                search_text_ += kKeys[key_row_][key_column_];
        }
        else
        {
            switch (wide_at(key_column_))
            {
            case 0:
                if (!search_text_.empty() && search_text_.back() != ' ')
                    search_text_ += ' ';
                break;
            case 1:
                if (!search_text_.empty())
                    search_text_.pop_back();
                break;
            case 2:
                search_text_.clear();
                break;
            default:
                // Done: now it is searched for.
                submit_search(search_text_);
                break;
            }
        }
        break;
    case Button::Back:
        keyboard_open_ = false;
        break;
    default:
        break;
    }
    return true;
}

// The options menu. Returns false when it is not open.
bool App::press_menu(Button button)
{
    if (!menu_open_)
        return false;
    const int count = static_cast<int>(menu_choices_.size());
    if (button == Button::Up && menu_focus_ > 0)
        --menu_focus_;
    else if (button == Button::Down && menu_focus_ + 1 < count)
        ++menu_focus_;
    else if (button == Button::Accept && menu_focus_ < count)
    {
        const LibraryAction action = menu_choices_[static_cast<std::size_t>(menu_focus_)].second;
        if (library_handler_)
            library_handler_(action, menu_item_);
        // The tile shows the change at once; the host's next rows confirm it.
        for (BoardRow &row : rows_)
            for (BoardItem &item : row.items)
                if (item.id == menu_item_.id && action != LibraryAction::Forget)
                    item.in_library = action == LibraryAction::Add;
        for (BoardItem &item : discover_.items)
            if (item.id == menu_item_.id && action != LibraryAction::Forget)
                item.in_library = action == LibraryAction::Add;
        menu_open_ = false;
    }
    else if (button == Button::Back || button == Button::Options)
        menu_open_ = false;
    return true;
}

void App::enter_tab()
{
    if (selected_tab_ != navigation_focus_)
        tab_reveal_ = 0;
    selected_tab_ = navigation_focus_;
    clear_search();
    if (kTabs[selected_tab_].icon == Icon::Board || kTabs[selected_tab_].icon == Icon::Library)
    {
        if (!rows_.empty() && view() == shown_view_)
            zone_ = Zone::Rows;
    }
    else
    {
        zone_ = Zone::Content;
        if (kTabs[selected_tab_].icon == Icon::Discover)
        {
            discover_list_open_ = false;
            discover_area_ = discover_.items.empty() || discover_hold_ ? 0 : 1;
            if (discover_.types.empty() && discover_handler_)
                discover_handler_(-1, 0);
        }
        if (kTabs[selected_tab_].icon == Icon::Calendar)
        {
            calendar_area_ = 1;
            if (calendar_handler_)
                calendar_handler_(0, 0);
        }
    }
}

// Moves the focus from a screen back to its tab in the sidebar.
void App::leave_content()
{
    zone_ = Zone::Navigation;
    navigation_focus_ = selected_tab_;
}

// Where the rows start: under the featured area, or, for what a search found (which has
// no featured area), under the search bar.
float App::rows_top() const
{
    return shown_view_ == View::Search ? kTopBarHeight + units(0.8f) : kHeroHeight() + units(0.4f);
}

// Opens a title's page.
void App::open_title(const BoardItem &item)
{
    title_type_ = item.type;
    title_id_ = item.id;
    // The screen fades out first; the page opens once it has (see update).
    veil_rising_ = true;
    pending_type_ = item.type;
    pending_id_ = item.id;
    pending_name_ = item.name;
    pending_video_ = item.video;
    // What is already known shows at once; the rest follows.
    Details known;
    known.type = item.type;
    known.id = item.id;
    known.name = item.name;
    known.description = item.description;
    known.background = item.background;
    known.logo = item.logo;
    known.release_info = item.release_info;
    known.runtime = item.runtime;
    known.imdb_rating = item.imdb_rating;
    known.genres = item.genres;
    known.streams_loading = 1;
    pending_known_ = std::move(known);
    if (title_handler_.open)
        title_handler_.open(item.type, item.id);
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
    // After a search the focus goes to what was found, once there is something.
    if (focus_rows_when_ready_ && !swap_pending_)
    {
        for (const BoardRow &row : rows_)
        {
            if (row.items.empty())
                continue;
            focus_rows_when_ready_ = false;
            if (!keyboard_open_ && (zone_ == Zone::Search || zone_ == Zone::Navigation))
                zone_ = Zone::Rows;
            break;
        }
    }
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
    if (shown_view_ == View::Search)
    {
        // What a search found has the screen's height to itself: it scrolls only as far
        // as the focused row needs.
        const float top = row_top(row_focus_), bottom = top + row_height(rows_[row_focus_]) - kRowGap;
        const float window = kScreenHeight() - rows_top() - units(1.0f);
        if (top < scroll_y_target_)
            scroll_y_target_ = top;
        else if (bottom > scroll_y_target_ + window)
            scroll_y_target_ = bottom - window;
    }
    else
        scroll_y_target_ = row_top(row_focus_);

    const BoardRow &row = rows_[row_focus_];
    const float visible = kScreenWidth() - kNavWidth - 2 * kContentInset;
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

void App::set_calibrate_hold(float progress)
{
    player_->set_hold(player_open_ ? progress : 0.0f);
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

void App::set_trailer_handler(TrailerHandler handler)
{
    trailer_handler_ = std::move(handler);
}

void App::set_trailer(int image, bool live, float bar_top, float bar_bottom)
{
    trailer_image_ = image;
    trailer_live_ = live;
    trailer_bar_top_ = bar_top;
    trailer_bar_bottom_ = bar_bottom;
}

void App::set_speech_models(const SpeechModels &models)
{
    speech_ = models;
}

void App::set_speech_handlers(std::function<void(int model)> choose,
                              std::function<void(int model)> download)
{
    choose_speech_ = std::move(choose);
    download_speech_ = std::move(download);
}

void App::set_subtitle_style(const SubtitleStyle &style)
{
    subtitle_style_ = style;
    player_->set_subtitle_style(style);
}

void App::set_subtitle_style_handler(std::function<void(const SubtitleStyle &)> handler)
{
    subtitle_style_handler_ = std::move(handler);
}

// Settings: moves one of the subtitle style rows (3 size, 4 background, 5 colour,
// 6 weight) to its next or previous choice.
void App::change_subtitle_style(int row, int step)
{
    SubtitleStyle &style = subtitle_style_;
    if (row == 7)
        style.calibration_offset = std::clamp(style.calibration_offset + step * 50, -3000, 3000);
    else if (row == 3)
        style.size = std::clamp(style.size + step * 25, 50, 200);
    else if (row == 4)
        style.background = std::clamp(style.background + step * 20, 0, 100);
    else if (row == 5)
    {
        constexpr int kCount = static_cast<int>(std::size(kSubtitleColours));
        style.colour = ((style.colour + step) % kCount + kCount) % kCount;
    }
    else
        style.bold = !style.bold;
    player_->set_subtitle_style(style);
    if (subtitle_style_handler_)
        subtitle_style_handler_(style);
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
    add(static_cast<std::size_t>(settings_section_ * 16 + settings_row_));
    add(static_cast<std::size_t>(calendar_area_ * 4096 + calendar_day_ * 64 + calendar_entry_));
    add(static_cast<std::size_t>(calendar_.year * 12 + calendar_.month));
    add(static_cast<std::size_t>(addons_focus_));
    add(static_cast<std::size_t>((handheld_ui_ ? 1 : 0) + (handheld_auto_ ? 2 : 0)));
    add(static_cast<std::size_t>(discover_area_ * 8 + discover_pill_ + (discover_list_open_ ? 64 : 0)));
    add(static_cast<std::size_t>(discover_focus_ * 64 + discover_list_focus_));
    add(static_cast<std::size_t>(speech_.chosen));
    add(keyboard_open_);
    add(static_cast<std::size_t>(key_row_ * 16 + key_column_));
    add(search_text_.size());
    add(menu_open_);
    add(static_cast<std::size_t>(menu_focus_));
    add(trailers_);
    add(std::hash<std::string>{}(account_.audio_language + '/' + account_.subtitles_language));
    add(static_cast<std::size_t>(subtitle_style_.size * 7 + subtitle_style_.background * 131 +
                                 subtitle_style_.colour * 1009 + (subtitle_style_.bold ? 5003 : 0) +
                                 subtitle_style_.calibration_offset * 31));
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
    if (press_menu(button) || press_keyboard(button))
        return;
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
        if (kTabs[selected_tab_].icon == Icon::Calendar)
            press_calendar(button);
        else if (kTabs[selected_tab_].icon == Icon::Discover)
            press_discover(button);
        else if (kTabs[selected_tab_].icon == Icon::Addons)
            press_addons(button);
        else
            press_settings(button);
        break;
    case Zone::Search:
    {
        const bool rows_under = kTabs[selected_tab_].icon == Icon::Board ||
                                kTabs[selected_tab_].icon == Icon::Library || view() == View::Search;
        if (button == Button::Accept)
        {
            // The console's keyboard when there is one; the app's own otherwise.
            if (!keyboard_handler_ || !keyboard_handler_(search_text_))
            {
                keyboard_open_ = true;
                key_row_ = 1;
                key_column_ = 0;
            }
        }
        else if (button == Button::Down && rows_under && !rows_.empty())
            zone_ = Zone::Rows;
        else if (button == Button::Down && !rows_under)
            zone_ = Zone::Content;
        else if (button == Button::Back && view() == View::Search)
            submit_search({});
        else if (button == Button::Back && !search_text_.empty())
            search_text_.clear();
        else if (button == Button::Left || button == Button::Back)
            leave_content();
        break;
    }
    case Zone::Rows:
    {
        if (rows_.empty())
        {
            zone_ = Zone::Navigation;
            break;
        }
        std::size_t &column = column_focus_[row_focus_];
        if (button == Button::Options)
        {
            // What can be done with the focused title, in a menu beside its tile.
            if (const BoardItem *item = focused_item())
            {
                menu_item_ = *item;
                menu_choices_.clear();
                if (item->in_library)
                    menu_choices_.push_back({"Remove from library", LibraryAction::Remove});
                else
                    menu_choices_.push_back({"Add to library", LibraryAction::Add});
                if (item->progress >= 0)
                    menu_choices_.push_back({"Remove from Continue watching", LibraryAction::Forget});
                menu_focus_ = 0;
                // Beside the tile, and kept on the screen.
                const BoardRow &row = rows_[row_focus_];
                const float width = card_width(row);
                const float tile_left = kNavWidth + kContentInset + static_cast<float>(column) * width -
                                        scroll_x_[row_focus_];
                const float tile_top = rows_top() - scroll_y_ + row_top(row_focus_) +
                                       kRowTitleSize * 1.2f + kRowTitleGap;
                menu_x_ = std::min(tile_left + width + units(0.3f), kScreenWidth() - units(22.0f));
                menu_y_ = std::clamp(tile_top + units(1.0f), kTopBarHeight, kScreenHeight() - units(9.0f));
                menu_open_ = true;
            }
        }
        else if (button == Button::Left)
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
        else if (button == Button::Up || (button == Button::Down && row_focus_ + 1 < rows_.size()))
        {
            if (button == Button::Up && row_focus_ == 0)
            {
                zone_ = Zone::Search;
                break;
            }
            const std::size_t from = row_focus_;
            row_focus_ = button == Button::Up ? row_focus_ - 1 : row_focus_ + 1;
            if (shown_view_ == View::Search && !rows_[row_focus_].items.empty())
            {
                // In what a search found, up and down go to the card directly above or
                // below, wherever that row's focus was before.
                const float centre = (static_cast<float>(column) + 0.5f) * card_width(rows_[from]) -
                                     scroll_x_target_[from];
                const float width = card_width(rows_[row_focus_]);
                const float place = (centre + scroll_x_target_[row_focus_]) / width;
                column_focus_[row_focus_] = static_cast<std::size_t>(std::clamp(
                    place, 0.0f, static_cast<float>(rows_[row_focus_].items.size() - 1)));
            }
        }
        else if (button == Button::Accept)
        {
            if (const BoardItem *item = focused_item())
                open_title(*item);
        }
        else if (button == Button::Back && view() == View::Search)
        {
            // Out of the search, back to what was there before it.
            submit_search({});
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
    // The handheld UI is the same layout on a smaller screen, so everything on it is
    // larger. (Without display options, as on a television-only console, the screen
    // stays as it is.)
    constexpr float kHandheldScale = 1.15f;
    const float scale = handheld_now() ? kHandheldScale : 1.0f;
    if (kScreenWidth() != 1920.0f / scale)
    {
        set_screen_scale(scale);
        follow_focus();
    }
    if (player_open_)
        player_->update(seconds);
    else
        player_leaving_ = std::max(0.0f, player_leaving_ - seconds / 0.35f);
    // A search (or the end of one) waits for the screen to have faded out, and the screen
    // stays out until there are rows to fade in with, or it is plain there will be none.
    if (swap_pending_)
    {
        content_veil_ = std::min(1.0f, content_veil_ + seconds / 0.15f);
        if (content_veil_ >= 1.0f)
        {
            swap_pending_ = false;
            swap_holding_ = true;
            swap_held_ = 0;
            search_sent_ = swap_query_;
            // A search is shown on the board's screen, whatever screen it was asked from.
            if (!search_sent_.empty() && kTabs[selected_tab_].icon != Icon::Board &&
                kTabs[selected_tab_].icon != Icon::Library)
                selected_tab_ = 0;
        }
    }
    else if (swap_holding_)
    {
        swap_held_ += seconds;
        // Ready is every row answered and the posters that will be on screen loaded, so
        // that nothing arrives after the screen has come back.
        bool ready = !rows_.empty();
        std::size_t shown = 0;
        for (const BoardRow &row : rows_)
        {
            ready = ready && !row.loading && !row.items.empty();
            if (shown++ >= 2)
                continue;
            for (std::size_t column = 0; column < row.items.size() && column < 9; ++column)
                if (images_->get(row.items[column].poster, kPosterPixels).state == Images::State::Pending)
                    ready = false;
        }
        // Nothing at all after a while is a search that found nothing; rows that are
        // there but slow are shown as they are in the end.
        if ((ready && swap_held_ > 0.15f) || swap_held_ > (rows_.empty() ? 7.0f : 10.0f))
            swap_holding_ = false;
    }
    else
        content_veil_ = std::max(0.0f, content_veil_ - seconds / 0.3f);
    update_discover(seconds);
    tab_reveal_ = std::min(1.0f, tab_reveal_ + seconds / 0.28f);
    wheel_turn_ += seconds;
    wheel_alpha_ = swap_holding_ ? std::min(1.0f, wheel_alpha_ + seconds / 0.25f)
                                 : std::max(0.0f, wheel_alpha_ - seconds / 0.15f);
    // A change of view (another tab, a search begun or ended) empties the rows: the ones
    // on screen belong to the view before, and the host's for this one are on their way.
    if (view() != shown_view_ || search_sent_ != shown_query_)
    {
        shown_view_ = view();
        shown_query_ = search_sent_;
        menu_open_ = false;
        set_board({});
        scroll_y_ = scroll_y_target_ = 0;
        if (zone_ == Zone::Rows)
            zone_ = shown_view_ == View::Search ? Zone::Search : Zone::Navigation;
        if (shown_view_ != View::Search)
            navigation_focus_ = selected_tab_;
    }
    speech_shift_ = eased(speech_shift_, 0.0f, seconds);
    speech_alpha_ = std::min(1.0f, speech_alpha_ + seconds / 0.22f);
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
    // The featured area's copy keeps up with facts that arrive after it was taken (a
    // title part-way through gets its description and the rest a moment after the row).
    if (hero_valid_ && item != nullptr && item->id == hero_.id &&
        (item->description != hero_.description || item->release_info != hero_.release_info ||
         item->runtime != hero_.runtime || item->imdb_rating != hero_.imdb_rating ||
         item->genres.size() != hero_.genres.size() || item->logo != hero_.logo ||
         item->background != hero_.background))
        hero_ = *item;
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

    // The trailer of the title the focus rests on. Shortly after the focus arrives the
    // host is asked to get it ready (finding it and opening it take a second or two, which
    // the rest of the dwell covers); after the dwell it plays; when the focus moves on, or
    // anything else takes the screen, it stops.
    {
        constexpr float kPrepareAfter = 0.7f, kPlayAfter = 2.0f;
        const bool browsing = trailers_ && !title_open_ && !veil_rising_ && !player_open_ &&
                              zone_ == Zone::Rows && kTabs[selected_tab_].icon == Icon::Board &&
                              view() == View::Board && !swap_pending_;
        const std::string wanted = browsing && item != nullptr ? item->id : std::string{};
        if (wanted != trailer_for_)
        {
            if (trailer_prepared_ && trailer_handler_.stop)
                trailer_handler_.stop();
            trailer_for_ = wanted;
            trailer_dwell_ = 0;
            trailer_prepared_ = trailer_started_ = false;
        }
        else if (!wanted.empty())
        {
            trailer_dwell_ += seconds;
            if (!trailer_prepared_ && trailer_dwell_ >= kPrepareAfter)
            {
                trailer_prepared_ = true;
                if (trailer_handler_.prepare)
                    trailer_handler_.prepare(wanted);
            }
            if (trailer_prepared_ && !trailer_started_ && trailer_dwell_ >= kPlayAfter)
            {
                trailer_started_ = true;
                if (trailer_handler_.start)
                    trailer_handler_.start();
            }
        }
        // It fades in over the artwork once it is playing, and out faster.
        const bool showing = trailer_live_ && trailer_started_ && trailer_image_ != 0 && hero_valid_ &&
                             hero_.id == trailer_for_;
        trailer_alpha_ = showing ? std::min(1.0f, trailer_alpha_ + seconds / 0.7f)
                                 : std::max(0.0f, trailer_alpha_ - seconds / 0.25f);
        // How far to zoom past black bars is taken from the host while the trailer is not
        // yet visible, and held from then on: the picture never changes size on screen.
        if (trailer_alpha_ <= 0.0f)
        {
            trailer_trim_top_ = trailer_bar_top_;
            trailer_trim_bottom_ = trailer_bar_bottom_;
        }
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
    float top = (kScreenHeight() - group) / 2 + kNavIconRise;
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
    const float x = (kScreenWidth() - kSearchWidth) / 2;
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
    const float middle = y + kSearchHeight / 2;
    if (!search_text_.empty() || keyboard_open_)
    {
        nvgFillColor(vg_, foreground(1.0f));
        const float end = nvgText(vg_, x + units(1.5f), middle, search_text_.c_str(), nullptr);
        if (keyboard_open_)
        {
            // Where the next letter goes.
            nvgBeginPath(vg_);
            nvgRect(vg_, end + units(0.15f), middle - kSearchTextSize * 0.6f, units(0.12f), kSearchTextSize * 1.2f);
            nvgFillColor(vg_, accent());
            nvgFill(vg_);
        }
    }
    else
    {
        nvgFillColor(vg_, foreground(0.6f));
        nvgText(vg_, x + units(1.5f), middle, "Search", nullptr);
    }
    draw_icon(vg_, Icon::Search, x + kSearchWidth - units(2.0f), middle, units(1.4f),
              foreground_solid(0.62f));
}

// The on-screen keyboard, under the search bar and over the featured area.
void App::draw_keyboard()
{
    if (!keyboard_open_)
        return;
    const float pitch_x = kKeyWidth + kKeyGap, pitch_y = kKeyHeight + kKeyGap;
    const float width = kKeyColumns * pitch_x - kKeyGap, height = kKeyRows * pitch_y - kKeyGap;
    const float left = (kScreenWidth() - width) / 2, top = kKeyboardTop;
    const float pad = units(0.9f);
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, left - pad, top - pad, width + 2 * pad, height + 2 * pad, kRadius);
    nvgFillColor(vg_, nvgRGBAf(0.05f, 0.05f, 0.06f, 0.96f));
    nvgFill(vg_);
    const auto key = [&](float x, float y, float key_width, const char *label, bool focused) {
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, x, y, key_width, kKeyHeight, units(0.5f));
        nvgFillColor(vg_, overlay(focused ? 4.0f : 1.8f));
        nvgFill(vg_);
        if (focused)
            focus_ring(vg_, x, y, key_width, kKeyHeight, units(0.5f));
        nvgFontFace(vg_, "medium");
        nvgFontSize(vg_, units(1.25f));
        nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, foreground(focused ? 1.0f : 0.85f));
        nvgText(vg_, x + key_width / 2, y + kKeyHeight / 2, label, nullptr);
    };
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < kKeyColumns; ++column)
        {
            const char label[2] = {kKeys[row][column], 0};
            key(left + column * pitch_x, top + row * pitch_y, kKeyWidth, label,
                row == key_row_ && column == key_column_);
        }
    for (const WideKey &wide : kWideKeys)
        key(left + wide.first * pitch_x, top + 4 * pitch_y,
            (wide.last - wide.first + 1) * pitch_x - kKeyGap, wide.label,
            key_row_ == 4 && key_column_ >= wide.first && key_column_ <= wide.last);
}

// The options menu, beside the tile it is for.
void App::draw_menu()
{
    if (!menu_open_)
        return;
    const float row_height = units(3.0f), pad = units(0.6f);
    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, units(1.15f));
    float width = units(14.0f);
    for (const auto &choice : menu_choices_)
        width = std::max(width, nvgTextBounds(vg_, 0, 0, choice.first.c_str(), nullptr, nullptr) + units(3.0f));
    const float height = static_cast<float>(menu_choices_.size()) * row_height + 2 * pad;
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, menu_x_, menu_y_, width + 2 * pad, height, kRadius);
    nvgFillColor(vg_, nvgRGBAf(0.09f, 0.09f, 0.11f, 0.98f));
    nvgFill(vg_);
    for (std::size_t index = 0; index < menu_choices_.size(); ++index)
    {
        const float x = menu_x_ + pad, y = menu_y_ + pad + static_cast<float>(index) * row_height;
        const bool focused = static_cast<int>(index) == menu_focus_;
        if (focused)
        {
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, x, y, width, row_height - units(0.3f), kRadius * 0.7f);
            nvgFillColor(vg_, overlay(3.5f));
            nvgFill(vg_);
            focus_ring(vg_, x, y, width, row_height - units(0.3f), kRadius * 0.7f);
        }
        nvgFontFace(vg_, "medium");
        nvgFontSize(vg_, units(1.15f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, foreground(focused ? 1.0f : 0.8f));
        nvgText(vg_, x + units(1.2f), y + (row_height - units(0.3f)) / 2, menu_choices_[index].first.c_str(),
                nullptr);
    }
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
        row.items.size(), first + static_cast<std::size_t>(kScreenWidth() / width) + 3);
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
        if (item.in_library)
        {
            // A bookmark at the poster's top right corner: the library has this title.
            const float mark = units(1.1f), mark_x = x + w - units(2.0f);
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
    const float image_height = kHeroHeight();
    const float image_width = image_height * 16.0f / 9.0f;
    const float image_left = kScreenWidth() - image_width;
    const Images::Texture background = images_->get(item->background, kArtPixels);
    const bool art = background.state == Images::State::Ready;
    const bool trailer = trailer_alpha_ > 0.01f && trailer_image_ != 0;
    if (art)
    {
        if (current)
            hero_art_alpha_ = std::min(1.0f, hero_art_alpha_ + rise);
        nvgGlobalAlpha(vg_, hero_art_alpha_);
        cover_image(vg_, image_left, 0, image_width, image_height, 0, background);
        nvgGlobalAlpha(vg_, 1.0f);
    }
    if (trailer)
    {
        // The trailer takes the artwork's place, scaled to cover the same area.
        int picture_width = 0, picture_height = 0;
        nvgImageSize(vg_, trailer_image_, &picture_width, &picture_height);
        // Black bars above and below the picture are zoomed past, so what there is of
        // the picture fills the area's height (its sides are cropped instead), up to a
        // limit that keeps a very wide picture from being cropped to its middle.
        const float kept = std::max(0.5f, 1.0f - trailer_trim_top_ - trailer_trim_bottom_);
        const float cover = std::max(image_width / static_cast<float>(std::max(1, picture_width)),
                                     image_height / static_cast<float>(std::max(1, picture_height)));
        const float scale = cover * std::min(1.0f / kept, 1.4f);
        const float shown_width = picture_width * scale, shown_height = picture_height * scale;
        // The picture's own middle (between its bars) sits on the area's middle.
        const float middle = (trailer_trim_top_ + (1.0f - trailer_trim_bottom_)) / 2;
        nvgBeginPath(vg_);
        nvgRect(vg_, image_left, 0, image_width, image_height);
        nvgFillPaint(vg_, nvgImagePattern(vg_, image_left + (image_width - shown_width) / 2,
                                          image_height / 2 - middle * shown_height, shown_width,
                                          shown_height, 0, trailer_image_, trailer_alpha_ * fade));
        nvgFill(vg_);
    }
    if (art || trailer)
    {
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
    const float edge = shown_view_ == View::Search ? kTopBarHeight : kHeroHeight();
    nvgScissor(vg_, kNavWidth, edge, kScreenWidth() - kNavWidth, kScreenHeight() - edge);
    float top = rows_top() - scroll_y_;
    for (std::size_t index = 0; index < rows_.size(); ++index)
    {
        const float height = row_height(rows_[index]);
        if (top + height > 0 && top < kScreenHeight())
            draw_row(rows_[index], index, top);
        top += height;
    }
    nvgRestore(vg_);
}

// A turning wheel: something is on its way.
void App::draw_wheel(float x, float y, float alpha)
{
    if (alpha <= 0.0f)
        return;
    const float radius = units(1.6f), turn = wheel_turn_ * 5.5f;
    nvgBeginPath(vg_);
    nvgCircle(vg_, x, y, radius);
    nvgStrokeColor(vg_, foreground(0.12f * alpha));
    nvgStrokeWidth(vg_, units(0.28f));
    nvgStroke(vg_);
    nvgBeginPath(vg_);
    nvgArc(vg_, x, y, radius, turn, turn + 1.7f, NVG_CW);
    nvgStrokeColor(vg_, accent(alpha));
    nvgLineCap(vg_, NVG_ROUND);
    nvgStroke(vg_);
    nvgLineCap(vg_, NVG_BUTT);
}

void App::draw(int width, int height)
{
    images_->begin_frame();
    nvgBeginFrame(vg_, kScreenWidth(), kScreenHeight(), static_cast<float>(width) / kScreenWidth());
    (void)height;
    if (player_open_)
    {
        // The host has drawn the picture; only the controls go over it.
        player_->draw();
        nvgEndFrame(vg_);
        return;
    }

    nvgBeginPath(vg_);
    nvgRect(vg_, 0, 0, kScreenWidth(), kScreenHeight());
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
            nvgRect(vg_, 0, 0, kScreenWidth(), kScreenHeight());
            nvgFillColor(vg_, nvgRGBAf(0, 0, 0, rest));
            nvgFill(vg_);
        }
        nvgEndFrame(vg_);
        return;
    }
    // A screen newly switched to rises a little into place as it fades in.
    const float arrived = 1.0f - (1.0f - tab_reveal_) * (1.0f - tab_reveal_);
    nvgSave(vg_);
    nvgTranslate(vg_, 0, (1.0f - arrived) * units(1.0f));
    switch (shown_view_ == View::Search ? Icon::Board : kTabs[selected_tab_].icon)
    {
    case Icon::Board:
    case Icon::Library:
        // What a search found is rows alone: no featured area over them.
        if (shown_view_ != View::Search)
            draw_hero();
        draw_rows();
        break;
    case Icon::Calendar:
        draw_calendar();
        break;
    case Icon::Addons:
        draw_addons();
        break;
    case Icon::Discover:
        draw_discover();
        break;
    case Icon::Settings:
        draw_settings();
        break;
    default:
        break;
    }
    nvgRestore(vg_);
    // Between one search's results and the next, and while a screen newly switched to
    // arrives, the screen is under black.
    const float veil = std::max(content_veil_, 1.0f - arrived);
    if (veil > 0.0f)
    {
        nvgBeginPath(vg_);
        nvgRect(vg_, kNavWidth, 0, kScreenWidth() - kNavWidth, kScreenHeight());
        nvgFillColor(vg_, nvgRGBAf(0, 0, 0, veil));
        nvgFill(vg_);
    }
    // A wheel while a search is on its way.
    draw_wheel(kNavWidth + (kScreenWidth() - kNavWidth) / 2, kScreenHeight() / 2, wheel_alpha_);
    if (rows_.empty() && shown_view_ != View::Board && !swap_pending_ && !swap_holding_ &&
        (shown_view_ == View::Search || kTabs[selected_tab_].icon == Icon::Library))
    {
        // Nothing to show (yet): say why.
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.3f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground(0.55f));
        const std::string text =
            shown_view_ != View::Search
                ? std::string{"Nothing in your library yet. Press OPTIONS on a title to add it."}
                : "Nothing found for \xE2\x80\x9C" + shown_query_ + "\xE2\x80\x9D";
        nvgText(vg_, kNavWidth + kContentInset + kCardPadding, rows_top() + units(0.6f), text.c_str(), nullptr);
    }
    draw_top_bar();
    draw_navigation();
    draw_keyboard();
    draw_menu();
    // On the way to or from a title's page the board is under a veil of black.
    if (veil_ > 0.0f)
    {
        nvgBeginPath(vg_);
        nvgRect(vg_, 0, 0, kScreenWidth(), kScreenHeight());
        nvgFillColor(vg_, nvgRGBAf(0, 0, 0, veil_));
        nvgFill(vg_);
    }

    nvgEndFrame(vg_);
}
} // namespace ui
