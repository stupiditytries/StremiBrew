// The navigation icons, drawn as vector shapes so they stay sharp at any size.

#pragma once

#include "nanovg.h"

namespace ui
{
enum class Icon
{
    Board,
    Discover,
    Library,
    Calendar,
    Addons,
    Settings,
    Search,
};

// Draws `icon` centred on (x, y) within a square of side `size`, in `color`. Some icons are
// built from shapes that overlap, so `color` must be opaque: a translucent one would show
// the overlaps as darker patches. Dim an icon by mixing its colour with the background's.
void draw_icon(NVGcontext *vg, Icon icon, float x, float y, float size, NVGcolor color);
} // namespace ui
