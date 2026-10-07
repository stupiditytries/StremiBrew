// The Calendar and Addons screens (part of App).
//
// The calendar follows Stremio's: a month as a grid of days, each showing the posters of
// the episodes the library's series release that day, and beside it the same days as a
// list of episodes. L1 and R1 (or left and right on the month's name) change the month;
// cross on a day goes into its episodes, and cross on one of those opens the series at
// that episode.

#include "app.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "draw_util.hpp"
#include "icons.hpp"
#include "images.hpp"
#include "nanovg.h"
#include "theme.hpp"

namespace ui
{
using namespace theme;

namespace
{
constexpr const char *kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};
constexpr const char *kWeekdays[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

constexpr float kLeft = kNavWidth + units(1.5f);
constexpr float kTop = kTopBarHeight + units(0.2f);
float kBottom()
{
    return kScreenHeight() - units(1.5f);
}
constexpr float kListWidth = units(19.0f);
float kListLeft()
{
    return kScreenWidth() - units(1.5f) - kListWidth;
}
float kMainWidth()
{
    return kListLeft() - units(1.0f) - kLeft;
}
constexpr float kSelectorHeight = units(3.0f);
constexpr float kWeekHeight = units(2.4f);
constexpr float kGridTop = kTop + kSelectorHeight + units(0.4f) + kWeekHeight;
// The list: a day's heading, and each of its episodes.
constexpr float kDayHeading = units(3.0f), kEntryHeight = units(2.8f), kDayGap = units(0.6f);

std::string capitalised(std::string text)
{
    if (!text.empty())
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

std::string joined(const std::vector<std::string> &parts, bool capitals)
{
    std::string text;
    for (const std::string &part : parts)
        text += (text.empty() ? "" : ", ") + (capitals ? capitalised(part) : part);
    return text;
}
} // namespace

void App::set_calendar_handler(std::function<void(int year, int month)> handler)
{
    calendar_handler_ = std::move(handler);
}

void App::set_calendar(CalendarMonth month)
{
    // A month newly arrived starts on today, or on its first day.
    if (month.year != calendar_.year || month.month != calendar_.month)
    {
        calendar_day_ = month.today > 0 ? month.today : 1;
        calendar_area_ = std::min(calendar_area_, 1);
        calendar_scroll_ = 0;
    }
    calendar_ = std::move(month);
    calendar_day_ = std::clamp(calendar_day_, 1, std::max(1, calendar_.days));
}

void App::set_addons(std::vector<Addon> addons)
{
    addons_ = std::move(addons);
    addons_focus_ = std::clamp(addons_focus_, 0, std::max(0, static_cast<int>(addons_.size()) - 1));
}

const CalendarDay *App::calendar_day(int day) const
{
    for (const CalendarDay &entry : calendar_.items)
        if (entry.day == day)
            return &entry;
    return nullptr;
}

void App::press_calendar(Button button)
{
    const CalendarMonth &shown = calendar_;
    const auto change_month = [&](int step) {
        if (shown.year == 0 || !calendar_handler_)
            return;
        int year = shown.year, month = shown.month + step;
        if (month < 1)
            month = 12, --year;
        else if (month > 12)
            month = 1, ++year;
        calendar_handler_(year, month);
    };
    if (button == Button::SkipBack || button == Button::SkipForward)
    {
        change_month(button == Button::SkipBack ? -1 : 1);
        return;
    }
    const CalendarDay *day = calendar_day(calendar_day_);
    if (calendar_area_ == 0)
    {
        // The month's name.
        if (button == Button::Left)
            change_month(-1);
        else if (button == Button::Right)
            change_month(1);
        else if (button == Button::Down && shown.days > 0)
            calendar_area_ = 1;
        else if (button == Button::Up)
            zone_ = Zone::Search;
        else if (button == Button::Back)
            leave_content();
    }
    else if (calendar_area_ == 1)
    {
        // The grid of days.
        const int column = (shown.first_weekday + calendar_day_ - 1) % 7;
        const int row = (shown.first_weekday + calendar_day_ - 1) / 7;
        const int last_row = (shown.first_weekday + shown.days - 1) / 7;
        if (button == Button::Left)
        {
            if (column > 0 && calendar_day_ > 1)
                --calendar_day_;
            else
                leave_content();
        }
        else if (button == Button::Right)
        {
            if (column < 6 && calendar_day_ < shown.days)
                ++calendar_day_;
            else if (day != nullptr)
                calendar_area_ = 2, calendar_entry_ = 0;
        }
        else if (button == Button::Up)
        {
            if (calendar_day_ > 7)
                calendar_day_ -= 7;
            else
                calendar_area_ = 0;
        }
        else if (button == Button::Down)
        {
            if (calendar_day_ + 7 <= shown.days)
                calendar_day_ += 7;
            else if (row < last_row)
                calendar_day_ = shown.days;
        }
        else if (button == Button::Accept && day != nullptr)
            calendar_area_ = 2, calendar_entry_ = 0;
        else if (button == Button::Back)
            leave_content();
    }
    else if (day == nullptr)
    {
        calendar_area_ = 1;
    }
    else
    {
        // The focused day's episodes, in the list.
        const int count = static_cast<int>(day->items.size());
        calendar_entry_ = std::clamp(calendar_entry_, 0, count - 1);
        if (button == Button::Up && calendar_entry_ > 0)
            --calendar_entry_;
        else if (button == Button::Down && calendar_entry_ + 1 < count)
            ++calendar_entry_;
        else if (button == Button::Back || button == Button::Left)
            calendar_area_ = 1;
        else if (button == Button::Accept)
        {
            const CalendarEntry &entry = day->items[static_cast<std::size_t>(calendar_entry_)];
            BoardItem item;
            item.id = entry.id;
            item.type = entry.type;
            item.name = entry.name;
            item.video = entry.video;
            open_title(item);
        }
    }
}

void App::draw_calendar()
{
    const bool active = zone_ == Zone::Content;
    const CalendarMonth &shown = calendar_;
    const auto message = [&](const char *text) {
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.3f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground(0.55f));
        nvgText(vg_, kLeft + units(0.5f), kTop + units(0.5f), text, nullptr);
    };
    if (!account_.signed_in)
    {
        message("Sign in (in Settings) to see when your library's series have new episodes.");
        return;
    }
    if (shown.year == 0 || shown.days == 0)
    {
        message("Loading");
        return;
    }

    // The month, with the arrows that change it.
    {
        const float centre = kLeft + kMainWidth() / 2, middle = kTop + kSelectorHeight / 2;
        const float half = units(11.0f);
        const bool focused = active && calendar_area_ == 0;
        if (focused)
        {
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, centre - half, kTop, 2 * half, kSelectorHeight, kSelectorHeight / 2);
            nvgFillColor(vg_, overlay(2.0f));
            nvgFill(vg_);
            focus_ring(vg_, centre - half, kTop, 2 * half, kSelectorHeight, kSelectorHeight / 2);
        }
        nvgFontFace(vg_, "semibold");
        nvgFontSize(vg_, units(1.4f));
        nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, foreground(1.0f));
        const std::string name =
            std::string{kMonths[std::clamp(shown.month, 1, 12) - 1]} + " " + std::to_string(shown.year);
        nvgText(vg_, centre, middle, name.c_str(), nullptr);
        nvgFillColor(vg_, foreground(focused ? 1.0f : 0.6f));
        nvgText(vg_, centre - half + units(1.6f), middle, "\xE2\x80\xB9", nullptr);
        nvgText(vg_, centre + half - units(1.6f), middle, "\xE2\x80\xBA", nullptr);
        // The shoulder buttons change the month from anywhere on the screen.
        nvgFontFace(vg_, "medium");
        nvgFontSize(vg_, units(0.8f));
        nvgFillColor(vg_, foreground(0.4f));
        nvgText(vg_, centre - half - units(1.4f), middle, "L1", nullptr);
        nvgText(vg_, centre + half + units(1.4f), middle, "R1", nullptr);
    }

    // The days of the week, over their columns.
    const float cell_width = (kMainWidth() - 6.0f) / 7.0f;
    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, units(1.0f));
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg_, foreground(0.9f));
    for (int column = 0; column < 7; ++column)
        nvgText(vg_, kLeft + column * (cell_width + 1.0f) + units(0.5f), kGridTop - kWeekHeight / 2,
                kWeekdays[column], nullptr);

    // The grid: a cell for each day, after as many empty places as the month's first day
    // is into the week.
    const int rows = (shown.first_weekday + shown.days + 6) / 7;
    const float cell_height = (kBottom() - kGridTop - static_cast<float>(rows - 1)) / static_cast<float>(rows);
    for (int day = 1; day <= shown.days; ++day)
    {
        const int place = shown.first_weekday + day - 1;
        const float x = kLeft + static_cast<float>(place % 7) * (cell_width + 1.0f);
        const float y = kGridTop + static_cast<float>(place / 7) * (cell_height + 1.0f);
        const bool selected = day == calendar_day_;
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, x, y, cell_width, cell_height, units(0.3f));
        nvgFillColor(vg_, overlay(selected && active && calendar_area_ == 1 ? 2.4f : 1.0f));
        nvgFill(vg_);

        // The day's number; today's is on a disc of the accent colour.
        const float number_x = x + units(1.15f), number_y = y + units(1.15f);
        if (day == shown.today)
        {
            nvgBeginPath(vg_);
            nvgCircle(vg_, number_x, number_y, units(0.8f));
            nvgFillColor(vg_, accent());
            nvgFill(vg_);
        }
        nvgFontFace(vg_, "medium");
        nvgFontSize(vg_, units(1.0f));
        nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, foreground(day == shown.today ? 1.0f : 0.9f));
        nvgText(vg_, number_x, number_y, std::to_string(day).c_str(), nullptr);

        // The posters of what the day has, as many as fit; then how many more.
        if (const CalendarDay *entry = calendar_day(day))
        {
            const float inset = units(0.4f), gap = units(0.25f);
            const float poster_top = y + units(2.3f);
            const float poster_height = cell_height - units(2.3f) - inset;
            const float poster_width = poster_height * 2.0f / 3.0f;
            const int count = static_cast<int>(entry->items.size());
            const int fit = std::max(1, static_cast<int>((cell_width - 2 * inset + gap) / (poster_width + gap)));
            const int drawn = count > fit ? fit - 1 : count;
            float poster_x = x + inset;
            for (int index = 0; index < drawn; ++index)
            {
                const CalendarEntry &item = entry->items[static_cast<std::size_t>(index)];
                const Images::Texture poster = images_->get(item.poster, kPosterPixels);
                if (poster.state == Images::State::Ready)
                    cover_image(vg_, poster_x, poster_top, poster_width, poster_height, kRadius / 2, poster);
                else
                {
                    nvgBeginPath(vg_);
                    nvgRoundedRect(vg_, poster_x, poster_top, poster_width, poster_height, kRadius / 2);
                    nvgFillColor(vg_, overlay(2.0f));
                    nvgFill(vg_);
                }
                poster_x += poster_width + gap;
            }
            if (count > drawn)
            {
                nvgFontFace(vg_, "medium");
                nvgFontSize(vg_, units(0.95f));
                nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgFillColor(vg_, foreground(0.7f));
                const std::string more = "+" + std::to_string(count - drawn);
                nvgText(vg_, poster_x + units(0.3f), poster_top + poster_height / 2, more.c_str(), nullptr);
            }
        }
        if (selected && active && calendar_area_ == 1)
            focus_ring(vg_, x, y, cell_width, cell_height, units(0.3f));
        else if (selected)
        {
            // Still the chosen day while the focus is in its episodes or on the month.
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, x + 1, y + 1, cell_width - 2, cell_height - 2, units(0.3f));
            nvgStrokeColor(vg_, foreground(0.55f));
            nvgStrokeWidth(vg_, units(0.1f));
            nvgStroke(vg_);
        }
    }

    // The list: each day that has something, with its episodes.
    if (shown.items.empty())
    {
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.05f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground(0.5f));
        nvgTextBox(vg_, kListLeft() + units(0.4f), kTop + units(0.8f), kListWidth - units(0.8f),
                   shown.loading ? "Loading" : "Nothing from your library's series this month.", nullptr);
        return;
    }
    // It scrolls to keep the chosen day's episodes (or the next day that has any) in view.
    float offset = 0, wanted = 0;
    bool found = false;
    for (const CalendarDay &entry : shown.items)
    {
        if (!found && entry.day >= calendar_day_)
        {
            wanted = offset;
            found = true;
        }
        offset += kDayHeading + static_cast<float>(entry.items.size()) * kEntryHeight + units(0.4f) + kDayGap;
    }
    const float window = kBottom() - kTop;
    if (!found)
        wanted = offset;
    wanted = std::clamp(wanted, 0.0f, std::max(0.0f, offset - kDayGap - window));
    calendar_scroll_ = eased(calendar_scroll_, wanted, frame_seconds_);
    nvgSave(vg_);
    nvgScissor(vg_, kListLeft() - units(0.4f), kTop - units(0.3f), kListWidth + units(0.8f), window + units(0.6f));
    float y = kTop - calendar_scroll_;
    for (const CalendarDay &entry : shown.items)
    {
        const float height = kDayHeading + static_cast<float>(entry.items.size()) * kEntryHeight + units(0.4f);
        if (y + height > kTop - units(1.0f) && y < kBottom() + units(1.0f))
        {
            const bool selected = entry.day == calendar_day_;
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, kListLeft(), y, kListWidth, height, kRadius);
            nvgFillColor(vg_, overlay(1.0f));
            nvgFill(vg_);
            if (selected)
            {
                nvgBeginPath(vg_);
                nvgRoundedRect(vg_, kListLeft() + 1, y + 1, kListWidth - 2, height - 2, kRadius);
                nvgStrokeColor(vg_, foreground(0.9f));
                nvgStrokeWidth(vg_, units(0.12f));
                nvgStroke(vg_);
            }
            nvgFontFace(vg_, "medium");
            nvgFontSize(vg_, units(1.0f));
            nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            const bool today = entry.day == shown.today;
            nvgFillColor(vg_, today ? accent() : foreground(0.9f));
            const std::string heading = std::to_string(entry.day) + " " + kMonths[std::clamp(shown.month, 1, 12) - 1] +
                                        (today ? "  \xC2\xB7  Today" : "");
            nvgText(vg_, kListLeft() + units(1.0f), y + kDayHeading / 2, heading.c_str(), nullptr);
            for (std::size_t index = 0; index < entry.items.size(); ++index)
            {
                const CalendarEntry &item = entry.items[index];
                const float row = y + kDayHeading + static_cast<float>(index) * kEntryHeight;
                const bool focused =
                    active && calendar_area_ == 2 && selected && static_cast<int>(index) == calendar_entry_;
                if (focused)
                {
                    const float inset = units(0.4f);
                    nvgBeginPath(vg_);
                    nvgRoundedRect(vg_, kListLeft() + inset, row, kListWidth - 2 * inset, kEntryHeight, kRadius * 0.7f);
                    nvgFillColor(vg_, overlay(3.0f));
                    nvgFill(vg_);
                    focus_ring(vg_, kListLeft() + inset, row, kListWidth - 2 * inset, kEntryHeight, kRadius * 0.7f);
                }
                const std::string number = "S" + std::to_string(item.season) + "E" + std::to_string(item.episode);
                nvgFontFace(vg_, "medium");
                nvgFontSize(vg_, units(1.0f));
                nvgTextAlign(vg_, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                nvgFillColor(vg_, foreground(0.6f));
                nvgText(vg_, kListLeft() + kListWidth - units(1.0f), row + kEntryHeight / 2, number.c_str(), nullptr);
                const float number_width = nvgTextBounds(vg_, 0, 0, number.c_str(), nullptr, nullptr);
                nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgFillColor(vg_, foreground(focused ? 1.0f : 0.9f));
                fitted_text(vg_, kListLeft() + units(1.0f), row + kEntryHeight / 2,
                            kListWidth - units(3.0f) - number_width, item.name);
            }
        }
        y += height + kDayGap;
    }
    nvgRestore(vg_);
}

void App::press_addons(Button button)
{
    const int count = static_cast<int>(addons_.size());
    if (button == Button::Up)
    {
        if (addons_focus_ > 0)
            --addons_focus_;
        else
            zone_ = Zone::Search;
    }
    else if (button == Button::Down && addons_focus_ + 1 < count)
        ++addons_focus_;
    else if (button == Button::Left || button == Button::Back)
        leave_content();
}

void App::draw_addons()
{
    const bool active = zone_ == Zone::Content;
    const float left = kNavWidth + units(2.5f), width = units(56.0f);
    const float top = kTopBarHeight + units(1.2f);
    const float card = units(6.8f), gap = units(0.7f);
    const float heading = kRowTitleSize * 1.2f + units(1.0f);

    // The focused add-on is kept in view.
    const float focus_top = heading + static_cast<float>(addons_focus_) * (card + gap);
    const float window = kScreenHeight() - top - units(1.5f);
    float wanted = addons_scroll_target_;
    if (focus_top - heading < wanted)
        wanted = focus_top - heading;
    else if (focus_top + card > wanted + window)
        wanted = focus_top + card - window;
    addons_scroll_target_ = std::max(0.0f, wanted);
    addons_scroll_ = eased(addons_scroll_, addons_scroll_target_, frame_seconds_);

    nvgSave(vg_);
    nvgScissor(vg_, kNavWidth, kTopBarHeight, kScreenWidth() - kNavWidth, kScreenHeight() - kTopBarHeight);
    float y = top - addons_scroll_;
    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, kRowTitleSize);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg_, foreground());
    const float after = nvgText(vg_, left, y, "Addons", nullptr);
    nvgFontFace(vg_, "regular");
    nvgFontSize(vg_, units(1.1f));
    nvgFillColor(vg_, foreground(0.5f));
    const std::string installed = std::to_string(addons_.size()) + " installed";
    nvgText(vg_, after + units(1.0f), y + units(0.55f), installed.c_str(), nullptr);
    y += heading;

    for (std::size_t index = 0; index < addons_.size(); ++index, y += card + gap)
    {
        if (y + card < kTopBarHeight || y > kScreenHeight())
            continue;
        const Addon &addon = addons_[index];
        const bool focused = active && static_cast<int>(index) == addons_focus_;
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, left, y, width, card, kRadius);
        nvgFillColor(vg_, overlay(focused ? 3.0f : 1.6f));
        nvgFill(vg_);
        if (focused)
            focus_ring(vg_, left, y, width, card, kRadius);

        // Its logo, or the add-ons icon when it has none that can be drawn.
        const float box = card - units(2.0f), box_x = left + units(1.0f), box_y = y + units(1.0f);
        const Images::Texture logo = images_->get(addon.logo, kPosterPixels);
        if (logo.state != Images::State::Ready || !draw_logo(vg_, box_x, box_y, box, box, logo, 1.0f))
        {
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, box_x, box_y, box, box, kRadius);
            nvgFillColor(vg_, overlay(2.0f));
            nvgFill(vg_);
            draw_icon(vg_, Icon::Addons, box_x + box / 2, box_y + box / 2, box * 0.5f, foreground_solid(0.5f));
        }

        const float text_x = box_x + box + units(1.2f);
        const float text_width = left + width - units(1.2f) - text_x;
        nvgFontFace(vg_, "semibold");
        nvgFontSize(vg_, units(1.3f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, foreground(1.0f));
        float x = nvgText(vg_, text_x, y + units(1.6f), addon.name.c_str(), nullptr);
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.0f));
        nvgFillColor(vg_, foreground(0.5f));
        x = nvgText(vg_, x + units(0.7f), y + units(1.65f), ("v" + addon.version).c_str(), nullptr);
        if (addon.official)
        {
            nvgFontFace(vg_, "semibold");
            nvgFontSize(vg_, units(0.8f));
            const float badge = nvgTextBounds(vg_, 0, 0, "Official", nullptr, nullptr) + units(1.0f);
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, x + units(0.8f), y + units(0.95f), badge, units(1.4f), units(0.7f));
            nvgFillColor(vg_, accent(0.85f));
            nvgFill(vg_);
            nvgFillColor(vg_, foreground(1.0f));
            nvgText(vg_, x + units(1.3f), y + units(1.65f), "Official", nullptr);
        }
        // Where it comes from, at the card's right.
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(0.9f));
        nvgTextAlign(vg_, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, foreground(0.4f));
        nvgText(vg_, left + width - units(1.2f), y + units(1.65f), addon.host.c_str(), nullptr);

        // What it covers and what it provides, then what it says of itself.
        std::string covers = joined(addon.types, true);
        const std::string provides = joined(addon.resources, false);
        if (!provides.empty())
            covers += (covers.empty() ? "" : "  \xC2\xB7  ") + provides;
        nvgFontSize(vg_, units(0.95f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, accent());
        fitted_text(vg_, text_x, y + units(3.3f), text_width, covers);
        nvgFontSize(vg_, units(1.0f));
        nvgFillColor(vg_, foreground(0.65f));
        fitted_text(vg_, text_x, y + units(4.95f), text_width, drawable(addon.description));
    }
    nvgRestore(vg_);
    if (addons_.empty())
    {
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.3f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground(0.55f));
        nvgText(vg_, left, top + heading, "No add-ons are installed.", nullptr);
    }
}
} // namespace ui
