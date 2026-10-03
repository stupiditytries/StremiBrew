// The app's look: Stremio's colours, type and proportions (from its web client's style
// sheets), scaled for a television, on a black background instead of Stremio's purple.

#pragma once

#include "nanovg.h"

namespace ui::theme
{
// Every size derives from this unit, as Stremio's are in rem. The screen is laid out at
// 1920x1080 and scaled to the output.
inline constexpr float kUnit = 22.0f;
inline constexpr float kScreenWidth = 1920.0f;
inline constexpr float kScreenHeight = 1080.0f;

constexpr float units(float count)
{
    return count * kUnit;
}

inline NVGcolor background()
{
    return nvgRGB(0, 0, 0);
}
inline NVGcolor foreground(float opacity = 0.9f)
{
    return nvgRGBAf(1.0f, 1.0f, 1.0f, opacity);
}
inline NVGcolor accent(float opacity = 1.0f)
{
    return nvgRGBAf(123 / 255.0f, 91 / 255.0f, 245 / 255.0f, opacity);
}
// White at `opacity` as it looks over the black background, as an opaque colour. Used
// where a translucent colour would reveal overlapping shapes (see draw_icon).
inline NVGcolor foreground_solid(float opacity)
{
    return nvgRGBf(opacity, opacity, opacity);
}
// The faint white wash Stremio puts behind fields, placeholders and hovered buttons.
inline NVGcolor overlay(float strength = 1.0f)
{
    return nvgRGBAf(1.0f, 1.0f, 1.0f, 0.05f * strength);
}

inline constexpr float kRadius = units(0.75f);
inline constexpr float kFocusOutline = units(0.2f);

// Navigation column on the left and the bar across the top.
inline constexpr float kNavWidth = units(6.0f);
inline constexpr float kNavButton = units(4.8f);
inline constexpr float kNavIcon = units(1.5f);
inline constexpr float kNavIconRise = units(0.45f); // how far above its button's centre an icon sits
inline constexpr float kNavGap = units(1.0f);
inline constexpr float kNavLabelSize = units(0.8f);
inline constexpr float kTopBarHeight = units(5.5f);
inline constexpr float kSearchWidth = units(30.0f);
inline constexpr float kSearchHeight = units(3.25f);
inline constexpr float kSearchTextSize = units(1.2f);

// Rows of posters.
inline constexpr float kRowTitleSize = units(1.6f);
inline constexpr float kRowTitleGap = units(0.6f);
inline constexpr float kRowGap = units(1.6f);
inline constexpr float kCardPadding = units(0.55f); // space around a poster inside its cell
inline constexpr float kPosterWidth = units(9.6f);
inline constexpr float kPosterRatio = 1.464f;       // height / width of a poster
inline constexpr float kLandscapeRatio = 0.5625f;   // height / width of a landscape image
inline constexpr float kCardTitleSize = units(1.0f);
inline constexpr float kCardTitleHeight = units(2.2f);
inline constexpr float kContentInset = units(1.0f);
inline constexpr float kFocusScale = 1.05f;

// The featured area at the top of the board: the focused item's artwork and details.
inline constexpr float kHeroHeight = units(24.5f);
inline constexpr float kHeroTextTop = units(6.6f);
inline constexpr float kHeroTextWidth = units(34.0f);
inline constexpr float kHeroLogoWidth = units(20.0f);
inline constexpr float kHeroLogoHeight = units(6.4f);
inline constexpr float kHeroTitleSize = units(2.6f);
inline constexpr float kHeroMetaSize = units(1.1f);
inline constexpr float kHeroDescriptionSize = units(1.05f);
// Seconds: how long the focus rests on an item before the featured area switches to it,
// and how long the area takes to fade out and in.
inline constexpr float kHeroDwell = 0.30f;
inline constexpr float kHeroFadeOut = 0.16f;
inline constexpr float kHeroFadeIn = 0.40f;
// Seconds to wait for a title's logo before writing its name out instead.
inline constexpr float kHeroLogoWait = 2.5f;
inline NVGcolor imdb_yellow()
{
    return nvgRGB(245, 197, 24);
}
} // namespace ui::theme
