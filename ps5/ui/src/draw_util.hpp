// Drawing helpers the screens share.

#pragma once

#include <string>

#include "images.hpp"
#include "nanovg.h"

namespace ui
{
// Moves `value` towards `target`, covering most of the distance in about a tenth of a
// second whatever the frame rate.
float eased(float value, float target, float seconds);

// Draws one line of text, cut short with an ellipsis when it is wider than `width`.
void fitted_text(NVGcontext *vg, float x, float y, float width, const std::string &text);

// Draws `text` wrapped to `width`, at most `max_lines` lines, the last one cut short with
// an ellipsis when there is more. Returns the height used.
float wrapped_text(NVGcontext *vg, float x, float y, float width, float line_height,
                   int max_lines, const std::string &text);

// Fills a rounded rectangle with an image scaled to cover it, cropping what overflows.
void cover_image(NVGcontext *vg, float x, float y, float width, float height, float radius,
                 const Images::Texture &texture, float alpha = 1.0f);

// Draws a title logo inside a box, as large as fits, against the box's left and lower
// edges. The logo is placed by its visible part, so a transparent margin in the image
// does not shift it. Returns false (and draws nothing) when the texture has nothing to show.
bool draw_logo(NVGcontext *vg, float x, float y, float width, float height,
               const Images::Texture &texture, float alpha);

// The white outline around whatever has the focus.
void focus_ring(NVGcontext *vg, float x, float y, float width, float height, float radius);

// `text` without the characters the app's typeface cannot draw (emoji and pictographs,
// which add-ons like to put in stream names); each run of them becomes one space.
std::string drawable(const std::string &text);
} // namespace ui
