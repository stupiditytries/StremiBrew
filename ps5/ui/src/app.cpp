#include "app.hpp"

#include <algorithm>
#include <cmath>

#include "icons.hpp"
#include "images.hpp"
#include "nanovg.h"
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

// Moves `value` towards `target`, covering most of the distance in about a tenth of a
// second whatever the frame rate.
float eased(float value, float target, float seconds)
{
    return target + (value - target) * std::exp(-seconds * 14.0f);
}

// Draws one line of text, cut short with an ellipsis when it is wider than `width`.
void fitted_text(NVGcontext *vg, float x, float y, float width, const std::string &text)
{
    if (nvgTextBounds(vg, 0, 0, text.c_str(), nullptr, nullptr) <= width)
    {
        nvgText(vg, x, y, text.c_str(), nullptr);
        return;
    }
    static const char kEllipsis[] = "\xE2\x80\xA6";
    const float room = width - nvgTextBounds(vg, 0, 0, kEllipsis, nullptr, nullptr);
    std::size_t length = text.size();
    while (length > 0)
    {
        // Step back one whole UTF-8 character at a time.
        do
            --length;
        while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80);
        if (nvgTextBounds(vg, 0, 0, text.c_str(), text.c_str() + length, nullptr) <= room)
            break;
    }
    while (length > 0 && text[length - 1] == ' ')
        --length;
    const float end = nvgText(vg, x, y, text.c_str(), text.c_str() + length);
    nvgText(vg, end, y, kEllipsis, nullptr);
}

// Fills a rounded rectangle with an image scaled to cover it, cropping what overflows.
void cover_image(NVGcontext *vg, float x, float y, float width, float height, float radius,
                 const Images::Texture &texture)
{
    const float scale =
        std::max(width / static_cast<float>(texture.width), height / static_cast<float>(texture.height));
    const float image_width = texture.width * scale;
    const float image_height = texture.height * scale;
    const NVGpaint paint =
        nvgImagePattern(vg, x + (width - image_width) / 2, y + (height - image_height) / 2,
                        image_width, image_height, 0, texture.handle, 1.0f);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, radius);
    nvgFillPaint(vg, paint);
    nvgFill(vg);
}

void focus_ring(NVGcontext *vg, float x, float y, float width, float height, float radius)
{
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x - kFocusOutline / 2, y - kFocusOutline / 2, width + kFocusOutline,
                   height + kFocusOutline, radius + kFocusOutline / 2);
    nvgStrokeColor(vg, foreground(1.0f));
    nvgStrokeWidth(vg, kFocusOutline);
    nvgStroke(vg);
}
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
}

App::~App() = default;

void App::set_image_fetcher(
    std::function<void(const std::string &address, const std::string &file)> fetcher)
{
    images_->set_fetcher(std::move(fetcher));
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

void App::press(Button button)
{
    switch (zone_)
    {
    case Zone::Navigation:
        if (button == Button::Up && navigation_focus_ > 0)
            --navigation_focus_;
        else if (button == Button::Down && navigation_focus_ < kTabCount - 1)
            ++navigation_focus_;
        else if (button == Button::Right && !rows_.empty())
            zone_ = Zone::Rows;
        else if (button == Button::Accept)
            selected_tab_ = navigation_focus_;
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
    scroll_y_ = eased(scroll_y_, scroll_y_target_, seconds);
    for (std::size_t row = 0; row < scroll_x_.size(); ++row)
        scroll_x_[row] = eased(scroll_x_[row], scroll_x_target_[row], seconds);
    focus_pulse_ = eased(focus_pulse_, 1.0f, seconds);
    for (int index = 0; index < kTabCount; ++index)
    {
        const bool focused = zone_ == Zone::Navigation && index == navigation_focus_;
        navigation_reveal_[index] = eased(navigation_reveal_[index], focused ? 1.0f : 0.0f, seconds);
    }
}

void App::draw_navigation()
{
    // The buttons sit as a group in the middle of the space under the top bar.
    const float group = kTabCount * kNavButton + (kTabCount - 1) * kNavGap;
    float top = kTopBarHeight + (kScreenHeight - kTopBarHeight - group) / 2;
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
        const float icon_y = top + kNavButton / 2 - units(0.45f);
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
    // The bar fades the rows out as they scroll under it.
    const NVGpaint fade = nvgLinearGradient(vg_, 0, kTopBarHeight - units(1.0f), 0,
                                            kTopBarHeight + units(1.0f), nvgRGBA(0, 0, 0, 255),
                                            nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg_);
    nvgRect(vg_, 0, 0, kScreenWidth, kTopBarHeight + units(1.0f));
    nvgFillPaint(vg_, fade);
    nvgFill(vg_);

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

        const Images::Texture texture = images_->get(item.poster);
        if (texture.handle != 0)
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

void App::draw_rows()
{
    // Rows are clipped to the area right of the navigation column.
    nvgSave(vg_);
    nvgScissor(vg_, kNavWidth, 0, kScreenWidth - kNavWidth, kScreenHeight);
    float top = kTopBarHeight + units(0.5f) - scroll_y_;
    for (std::size_t index = 0; index < rows_.size(); ++index)
    {
        const float height = row_height(rows_[index]);
        if (top + height > 0 && top < kScreenHeight)
            draw_row(rows_[index], index, top);
        top += height;
    }
    nvgRestore(vg_);
}

void App::draw(int width, int height)
{
    images_->begin_frame();
    nvgBeginFrame(vg_, kScreenWidth, kScreenHeight, static_cast<float>(width) / kScreenWidth);
    (void)height;

    nvgBeginPath(vg_);
    nvgRect(vg_, 0, 0, kScreenWidth, kScreenHeight);
    nvgFillColor(vg_, background());
    nvgFill(vg_);

    draw_rows();
    draw_top_bar();
    draw_navigation();

    nvgEndFrame(vg_);
}
} // namespace ui
