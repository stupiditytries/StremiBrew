#include "player_screen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app.hpp"
#include "draw_util.hpp"
#include "nanovg.h"
#include "theme.hpp"

namespace ui
{
using namespace theme;

namespace
{
constexpr float kSide = units(4.0f);
constexpr float kBarTop = kScreenHeight - units(6.4f);
constexpr float kBarHeight = units(0.3f);
constexpr float kRowMiddle = kScreenHeight - units(3.6f);
constexpr float kTitleMiddle = units(3.6f);
// Seconds the controls stay up after the last press while a video plays.
constexpr float kControlsStay = 4.0f;
// Seconds after the last scrub press before the video jumps to the marker.
constexpr float kScrubSettle = 0.6f;

// 1:02:03 for anything in a video an hour or longer, 2:03 otherwise.
std::string clock_text(double seconds, bool hours)
{
    const long whole = static_cast<long>(std::max(0.0, seconds));
    char text[24];
    if (hours)
        std::snprintf(text, sizeof text, "%ld:%02ld:%02ld", whole / 3600, whole / 60 % 60, whole % 60);
    else
        std::snprintf(text, sizeof text, "%ld:%02ld", whole / 60, whole % 60);
    return text;
}
} // namespace

PlayerScreen::PlayerScreen(NVGcontext *context) : vg_{context}
{
}

void PlayerScreen::set_handler(PlayerHandler handler)
{
    handler_ = std::move(handler);
}

void PlayerScreen::open(const std::string &title)
{
    title_ = title;
    playback_ = Playback{};
    controls_ = 1;
    idle_ = 0;
    busy_ = 0;
    scrubbing_ = false;
    scrub_presses_ = 0;
}

void PlayerScreen::set_playback(const Playback &playback)
{
    playback_ = playback;
}

void PlayerScreen::commit_scrub()
{
    if (!scrubbing_)
        return;
    scrubbing_ = false;
    scrub_presses_ = 0;
    if (handler_.seek)
        handler_.seek(scrub_target_);
}

bool PlayerScreen::press(Button button)
{
    idle_ = 0;
    const bool ready =
        playback_.state == Playback::State::Playing || playback_.state == Playback::State::Paused;
    switch (button)
    {
    case Button::Back:
        return false;
    case Button::Accept:
        if (scrubbing_)
            commit_scrub();
        else if (ready && handler_.set_paused)
            handler_.set_paused(playback_.state == Playback::State::Playing);
        break;
    case Button::Left:
    case Button::Right:
    {
        if (!ready || playback_.duration <= 0)
            break;
        if (!scrubbing_)
        {
            scrubbing_ = true;
            scrub_target_ = playback_.position;
            scrub_presses_ = 0;
        }
        // Held or pressed again and again, the steps grow.
        ++scrub_presses_;
        const double step = scrub_presses_ > 16 ? 60.0 : scrub_presses_ > 6 ? 30.0 : 10.0;
        scrub_target_ = std::clamp(scrub_target_ + (button == Button::Right ? step : -step), 0.0,
                                   std::max(0.0, playback_.duration - 1.0));
        scrub_idle_ = 0;
        break;
    }
    default:
        break; // any other button only brings the controls up
    }
    return true;
}

void PlayerScreen::update(float seconds)
{
    idle_ += seconds;
    spin_ += seconds * 5.0f;
    if (scrubbing_)
    {
        scrub_idle_ += seconds;
        if (scrub_idle_ >= kScrubSettle)
            commit_scrub();
    }
    const bool stay = playback_.state != Playback::State::Playing || scrubbing_ || idle_ < kControlsStay;
    const float step = seconds / (stay ? 0.15f : 0.5f);
    controls_ = stay ? std::min(1.0f, controls_ + step) : std::max(0.0f, controls_ - step);
    const bool busy = playback_.state == Playback::State::Opening ||
                      (playback_.state == Playback::State::Playing && playback_.buffering);
    busy_ = busy ? std::min(1.0f, busy_ + seconds / 0.25f) : std::max(0.0f, busy_ - seconds / 0.15f);
}

// A ring with a brighter arc going round it.
void PlayerScreen::draw_spinner(float x, float y, float radius, float alpha)
{
    nvgStrokeWidth(vg_, units(0.35f));
    nvgLineCap(vg_, NVG_ROUND);
    nvgBeginPath(vg_);
    nvgCircle(vg_, x, y, radius);
    nvgStrokeColor(vg_, foreground(0.18f * alpha));
    nvgStroke(vg_);
    nvgBeginPath(vg_);
    nvgArc(vg_, x, y, radius, spin_, spin_ + 1.6f, NVG_CW);
    nvgStrokeColor(vg_, foreground(alpha));
    nvgStroke(vg_);
    nvgLineCap(vg_, NVG_BUTT);
}

void PlayerScreen::draw()
{
    const float centre_x = kScreenWidth / 2, centre_y = kScreenHeight / 2;
    if (playback_.state == Playback::State::Failed)
    {
        nvgBeginPath(vg_);
        nvgRect(vg_, 0, 0, kScreenWidth, kScreenHeight);
        nvgFillColor(vg_, nvgRGBAf(0, 0, 0, 0.75f));
        nvgFill(vg_);
        nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontFace(vg_, "semibold");
        nvgFontSize(vg_, units(1.8f));
        nvgFillColor(vg_, foreground());
        nvgText(vg_, centre_x, centre_y - units(1.6f), "This stream could not be played", nullptr);
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.1f));
        nvgFillColor(vg_, foreground(0.65f));
        nvgText(vg_, centre_x, centre_y + units(0.6f), playback_.error.c_str(), nullptr);
        nvgText(vg_, centre_x, centre_y + units(2.6f), "Press the circle button to go back", nullptr);
        return;
    }
    if (busy_ > 0.01f)
        draw_spinner(centre_x, centre_y, units(2.0f), busy_);
    if (controls_ < 0.01f)
        return;
    const float shown = controls_;

    // Shade at the top and the bottom, so the controls read over a bright picture.
    const auto shade = [&](float top, float height, bool lower) {
        const NVGcolor dark = nvgRGBAf(0, 0, 0, 0.78f * shown), clear = nvgRGBAf(0, 0, 0, 0);
        nvgBeginPath(vg_);
        nvgRect(vg_, 0, top, kScreenWidth, height);
        nvgFillPaint(vg_, nvgLinearGradient(vg_, 0, top, 0, top + height, lower ? clear : dark,
                                            lower ? dark : clear));
        nvgFill(vg_);
    };
    shade(0, units(9.0f), false);
    shade(kScreenHeight - units(12.0f), units(12.0f), true);

    // Top: a back mark and what is playing.
    nvgBeginPath(vg_);
    nvgMoveTo(vg_, kSide + units(0.7f), kTitleMiddle - units(0.7f));
    nvgLineTo(vg_, kSide, kTitleMiddle);
    nvgLineTo(vg_, kSide + units(0.7f), kTitleMiddle + units(0.7f));
    nvgStrokeColor(vg_, foreground(shown));
    nvgStrokeWidth(vg_, units(0.22f));
    nvgLineCap(vg_, NVG_ROUND);
    nvgLineJoin(vg_, NVG_ROUND);
    nvgStroke(vg_);
    nvgLineCap(vg_, NVG_BUTT);
    nvgLineJoin(vg_, NVG_MITER);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontFace(vg_, "semibold");
    nvgFontSize(vg_, units(1.5f));
    nvgFillColor(vg_, foreground(shown));
    fitted_text(vg_, kSide + units(2.2f), kTitleMiddle, kScreenWidth - 2 * kSide - units(2.2f),
                drawable(title_));

    // Bottom: the seek bar.
    const double duration = playback_.duration;
    const double position = scrubbing_ ? scrub_target_ : playback_.position;
    const float along =
        duration > 0 ? static_cast<float>(std::clamp(position / duration, 0.0, 1.0)) : 0.0f;
    const float bar_width = kScreenWidth - 2 * kSide;
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, kSide, kBarTop, bar_width, kBarHeight, kBarHeight / 2);
    nvgFillColor(vg_, nvgRGBAf(1, 1, 1, 0.28f * shown));
    nvgFill(vg_);
    if (scrubbing_ && duration > 0)
    {
        // Where the video still is, while the marker is away from it.
        const float playing = static_cast<float>(std::clamp(playback_.position / duration, 0.0, 1.0));
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, kSide, kBarTop, bar_width * playing, kBarHeight, kBarHeight / 2);
        nvgFillColor(vg_, nvgRGBAf(1, 1, 1, 0.35f * shown));
        nvgFill(vg_);
    }
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, kSide, kBarTop, bar_width * along, kBarHeight, kBarHeight / 2);
    nvgFillColor(vg_, accent(shown));
    nvgFill(vg_);
    const float thumb_x = kSide + bar_width * along, thumb_y = kBarTop + kBarHeight / 2;
    nvgBeginPath(vg_);
    nvgCircle(vg_, thumb_x, thumb_y, scrubbing_ ? units(0.7f) : units(0.5f));
    nvgFillColor(vg_, foreground_solid(shown));
    nvgFill(vg_);

    const bool hours = duration >= 3600;
    if (scrubbing_)
    {
        // The time the marker is at, above it.
        const std::string label = clock_text(scrub_target_, hours);
        nvgFontFace(vg_, "semibold");
        nvgFontSize(vg_, units(1.2f));
        const float width = nvgTextBounds(vg_, 0, 0, label.c_str(), nullptr, nullptr) + units(1.4f);
        const float left = std::clamp(thumb_x - width / 2, kSide, kScreenWidth - kSide - width);
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, left, kBarTop - units(3.4f), width, units(2.2f), units(0.5f));
        nvgFillColor(vg_, nvgRGBAf(0.1f, 0.1f, 0.12f, 0.92f * shown));
        nvgFill(vg_);
        nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, foreground(shown));
        nvgText(vg_, left + width / 2, kBarTop - units(2.3f), label.c_str(), nullptr);
    }

    // Under the bar: play or pause (what the cross button will do), and the time.
    const float mark = units(1.5f);
    const float mark_x = kSide + units(0.2f), mark_top = kRowMiddle - mark / 2;
    nvgFillColor(vg_, foreground_solid(shown));
    if (playback_.state == Playback::State::Playing)
    {
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, mark_x, mark_top, mark * 0.32f, mark, units(0.12f));
        nvgRoundedRect(vg_, mark_x + mark * 0.56f, mark_top, mark * 0.32f, mark, units(0.12f));
        nvgFill(vg_);
    }
    else
    {
        nvgBeginPath(vg_);
        nvgMoveTo(vg_, mark_x, mark_top);
        nvgLineTo(vg_, mark_x + mark * 0.9f, kRowMiddle);
        nvgLineTo(vg_, mark_x, mark_top + mark);
        nvgClosePath(vg_);
        nvgFill(vg_);
    }
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, units(1.15f));
    nvgFillColor(vg_, foreground(shown));
    const std::string now = clock_text(position, hours);
    const float end = nvgText(vg_, kSide + units(3.0f), kRowMiddle, now.c_str(), nullptr);
    if (duration > 0)
    {
        nvgFillColor(vg_, foreground(0.6f * shown));
        nvgText(vg_, end, kRowMiddle, ("  /  " + clock_text(duration, hours)).c_str(), nullptr);
    }
}
} // namespace ui
