#include "icons.hpp"

#include <cmath>

namespace ui
{
namespace
{
constexpr float kPi = 3.14159265f;

void board(NVGcontext *vg, float s)
{
    // A house: roof and body with a doorway cut out.
    nvgBeginPath(vg);
    nvgMoveTo(vg, 0, -0.46f * s);
    nvgLineTo(vg, 0.46f * s, -0.04f * s);
    nvgLineTo(vg, 0.36f * s, -0.04f * s);
    nvgLineTo(vg, 0.36f * s, 0.42f * s);
    nvgLineTo(vg, 0.10f * s, 0.42f * s);
    nvgLineTo(vg, 0.10f * s, 0.14f * s);
    nvgLineTo(vg, -0.10f * s, 0.14f * s);
    nvgLineTo(vg, -0.10f * s, 0.42f * s);
    nvgLineTo(vg, -0.36f * s, 0.42f * s);
    nvgLineTo(vg, -0.36f * s, -0.04f * s);
    nvgLineTo(vg, -0.46f * s, -0.04f * s);
    nvgClosePath(vg);
    nvgFill(vg);
}

void discover(NVGcontext *vg, float s, NVGcolor color)
{
    // A compass: ring with a needle.
    nvgBeginPath(vg);
    nvgCircle(vg, 0, 0, 0.42f * s);
    nvgStrokeColor(vg, color);
    nvgStrokeWidth(vg, 0.08f * s);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgMoveTo(vg, 0.22f * s, -0.22f * s);
    nvgLineTo(vg, 0.07f * s, 0.07f * s);
    nvgLineTo(vg, -0.22f * s, 0.22f * s);
    nvgLineTo(vg, -0.07f * s, -0.07f * s);
    nvgClosePath(vg);
    nvgFill(vg);
}

void library(NVGcontext *vg, float s)
{
    // Books on a shelf: two upright and one leaning.
    nvgBeginPath(vg);
    nvgRoundedRect(vg, -0.42f * s, -0.40f * s, 0.18f * s, 0.80f * s, 0.04f * s);
    nvgRoundedRect(vg, -0.16f * s, -0.40f * s, 0.18f * s, 0.80f * s, 0.04f * s);
    nvgFill(vg);
    nvgSave(vg);
    nvgTranslate(vg, 0.27f * s, 0.02f * s);
    nvgRotate(vg, -0.26f);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, -0.09f * s, -0.40f * s, 0.18f * s, 0.80f * s, 0.04f * s);
    nvgFill(vg);
    nvgRestore(vg);
}

void calendar(NVGcontext *vg, float s, NVGcolor color)
{
    nvgBeginPath(vg);
    nvgRoundedRect(vg, -0.40f * s, -0.34f * s, 0.80f * s, 0.74f * s, 0.10f * s);
    nvgStrokeColor(vg, color);
    nvgStrokeWidth(vg, 0.08f * s);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgRect(vg, -0.40f * s, -0.20f * s, 0.80f * s, 0.10f * s);
    // The two rings on top and a grid of days.
    nvgRoundedRect(vg, -0.24f * s, -0.46f * s, 0.08f * s, 0.20f * s, 0.04f * s);
    nvgRoundedRect(vg, 0.16f * s, -0.46f * s, 0.08f * s, 0.20f * s, 0.04f * s);
    for (int row = 0; row < 2; ++row)
        for (int column = 0; column < 3; ++column)
            nvgRect(vg, (-0.25f + column * 0.20f) * s, (0.02f + row * 0.17f) * s, 0.10f * s,
                    0.09f * s);
    nvgFill(vg);
}

void addons(NVGcontext *vg, float s)
{
    // A jigsaw piece: a square with a knob on its top and right edges.
    nvgBeginPath(vg);
    nvgRoundedRect(vg, -0.40f * s, -0.22f * s, 0.62f * s, 0.62f * s, 0.07f * s);
    nvgCircle(vg, -0.09f * s, -0.27f * s, 0.14f * s);
    nvgCircle(vg, 0.27f * s, 0.09f * s, 0.14f * s);
    nvgFill(vg);
}

void settings(NVGcontext *vg, float s)
{
    // A gear: eight teeth around a ring.
    nvgBeginPath(vg);
    for (int tooth = 0; tooth < 8; ++tooth)
    {
        const float angle = tooth * kPi / 4.0f;
        nvgSave(vg);
        nvgRotate(vg, angle);
        nvgRoundedRect(vg, -0.09f * s, -0.46f * s, 0.18f * s, 0.22f * s, 0.03f * s);
        nvgRestore(vg);
    }
    nvgCircle(vg, 0, 0, 0.32f * s);
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, 0, 0, 0.32f * s);
    nvgCircle(vg, 0, 0, 0.13f * s);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFill(vg);
}

void search(NVGcontext *vg, float s, NVGcolor color)
{
    nvgStrokeColor(vg, color);
    nvgStrokeWidth(vg, 0.09f * s);
    nvgLineCap(vg, NVG_ROUND);
    nvgBeginPath(vg);
    nvgCircle(vg, -0.08f * s, -0.08f * s, 0.28f * s);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgMoveTo(vg, 0.13f * s, 0.13f * s);
    nvgLineTo(vg, 0.40f * s, 0.40f * s);
    nvgStroke(vg);
}
} // namespace

void draw_icon(NVGcontext *vg, Icon icon, float x, float y, float size, NVGcolor color)
{
    nvgSave(vg);
    nvgTranslate(vg, x, y);
    nvgFillColor(vg, color);
    switch (icon)
    {
    case Icon::Board:
        board(vg, size);
        break;
    case Icon::Discover:
        discover(vg, size, color);
        break;
    case Icon::Library:
        library(vg, size);
        break;
    case Icon::Calendar:
        calendar(vg, size, color);
        break;
    case Icon::Addons:
        addons(vg, size);
        break;
    case Icon::Settings:
        settings(vg, size);
        break;
    case Icon::Search:
        search(vg, size, color);
        break;
    }
    nvgRestore(vg);
}
} // namespace ui
