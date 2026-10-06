#include "draw_util.hpp"

#include <algorithm>
#include <cmath>

#include "theme.hpp"

namespace ui
{
using namespace theme;

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
                 const Images::Texture &texture, float alpha)
{
    const float scale =
        std::max(width / static_cast<float>(texture.width), height / static_cast<float>(texture.height));
    const float image_width = texture.width * scale;
    const float image_height = texture.height * scale;
    const NVGpaint paint =
        nvgImagePattern(vg, x + (width - image_width) / 2, y + (height - image_height) / 2,
                        image_width, image_height, 0, texture.handle, alpha);
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

bool draw_logo(NVGcontext *vg, float x, float y, float width, float height,
               const Images::Texture &texture, float alpha)
{
    if (texture.state != Images::State::Ready || texture.content_width <= 0 ||
        texture.content_height <= 0)
        return false;
    const float scale = std::min(width / static_cast<float>(texture.content_width),
                                 height / static_cast<float>(texture.content_height));
    const float shown_width = texture.content_width * scale;
    const float shown_height = texture.content_height * scale;
    const float top = y + height - shown_height;
    // The whole image is laid out so that its visible part lands on the box.
    const NVGpaint paint =
        nvgImagePattern(vg, x - texture.content_x * scale, top - texture.content_y * scale,
                        texture.width * scale, texture.height * scale, 0, texture.handle, alpha);
    nvgBeginPath(vg);
    nvgRect(vg, x, top, shown_width, shown_height);
    nvgFillPaint(vg, paint);
    nvgFill(vg);
    return true;
}

float wrapped_text(NVGcontext *vg, float x, float y, float width, float line_height,
                   int max_lines, const std::string &text)
{
    if (text.empty() || max_lines <= 0)
        return 0;
    constexpr int kMost = 12;
    NVGtextRow lines[kMost + 1];
    const int wanted = std::min(max_lines, kMost);
    const int count = nvgTextBreakLines(vg, text.c_str(), nullptr, width, lines, wanted + 1);
    float used = 0;
    for (int index = 0; index < count && index < wanted; ++index)
    {
        // When there is more than fits, the last line runs on into the rest of the text
        // and is cut off with an ellipsis.
        if (index == wanted - 1 && count > wanted)
            fitted_text(vg, x, y + used, width, std::string{lines[index].start});
        else
            nvgText(vg, x, y + used, lines[index].start, lines[index].end);
        used += line_height;
    }
    return used;
}

std::string drawable(const std::string &text)
{
    std::string result;
    bool gap = false;
    for (std::size_t index = 0; index < text.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        unsigned code = lead;
        if (length == 2)
            code = lead & 0x1F;
        else if (length == 3)
            code = lead & 0x0F;
        else if (length == 4)
            code = lead & 0x07;
        for (std::size_t tail = 1; tail < length && index + tail < text.size(); ++tail)
            code = (code << 6) | (static_cast<unsigned char>(text[index + tail]) & 0x3F);
        // Letters and punctuation of the scripts the typeface covers are kept; symbol,
        // dingbat, variation-selector and emoji ranges are not.
        const bool keep = code < 0x2000 || (code >= 0x2010 && code <= 0x2027) ||
                          (code >= 0x2030 && code <= 0x205E) || (code >= 0x20A0 && code <= 0x20BF);
        if (keep)
        {
            if (gap && !result.empty() && result.back() != ' ' && result.back() != '\n')
                result += ' ';
            gap = false;
            result.append(text, index, std::min(length, text.size() - index));
        }
        else
        {
            gap = true;
        }
        index += length;
    }
    return result;
}
} // namespace ui
