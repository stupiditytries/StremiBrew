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
constexpr float kBarHeight = units(0.3f);
float kBarTop()
{
    return kScreenHeight() - units(7.4f);
}
float kRowMiddle()
{
    return kScreenHeight() - units(4.0f);
}
constexpr float kTitleMiddle = units(3.6f);
constexpr float kButton = units(2.9f); // a button's diameter
constexpr float kButtonPitch = kButton + units(0.9f);
// Seconds the controls stay up after the last press while a video plays.
constexpr float kControlsStay = 4.0f;
// Seconds a calibration's result stays on screen.
constexpr float kNoticeStay = 4.0f;
// Seconds after the last scrub press before the video jumps to the marker.
constexpr float kScrubSettle = 0.9f;
// Seconds the back and forward buttons, and one scrub press, move by.
constexpr double kSkip = 15.0;
constexpr float kPi = 3.14159265f;

// The list of tracks at the right of the screen.
constexpr float kMenuWidth = units(26.0f);
constexpr float kMenuRow = units(3.4f);
constexpr float kMenuTop = units(8.0f);
float kMenuBottom()
{
    return kScreenHeight() - units(3.0f);
}

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
    tracks_ = PlayerTracks{};
    controls_ = 1;
    idle_ = 0;
    busy_ = 0;
    on_buttons_ = false;
    button_ = 0;
    scrubbing_ = false;
    scrub_presses_ = 0;
    preview_asked_ = -1;
    menu_ = menu_shown_ = Menu::None;
    menu_slide_ = 0;
}

void PlayerScreen::set_playback(const Playback &playback)
{
    playback_ = playback;
}

void PlayerScreen::set_tracks(PlayerTracks tracks)
{
    tracks_ = std::move(tracks);
    const int count = static_cast<int>(menu_options().size());
    menu_focus_ = std::clamp(menu_focus_, calibration_available_ ? -2 : -1, std::max(0, count - 1));
}

const std::vector<TrackOption> &PlayerScreen::menu_options() const
{
    return (menu_ != Menu::None ? menu_ : menu_shown_) == Menu::Audio ? tracks_.audio
                                                                       : tracks_.subtitles;
}

int PlayerScreen::menu_selected() const
{
    return (menu_ != Menu::None ? menu_ : menu_shown_) == Menu::Audio ? tracks_.audio_selected
                                                                       : tracks_.subtitle_selected;
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

void PlayerScreen::skip(double seconds)
{
    if (!handler_.seek)
        return;
    const double limit = std::max(0.0, playback_.duration - 1.0);
    const double from = scrubbing_ ? scrub_target_ : playback_.position;
    scrubbing_ = false;
    scrub_presses_ = 0;
    handler_.seek(std::clamp(from + seconds, 0.0, playback_.duration > 0 ? limit : from + seconds));
}

void PlayerScreen::activate(Control control)
{
    switch (control)
    {
    case Control::Play:
        if (handler_.set_paused)
            handler_.set_paused(playback_.state == Playback::State::Playing);
        break;
    case Control::Back:
        skip(-kSkip);
        break;
    case Control::Forward:
        skip(kSkip);
        break;
    case Control::Audio:
    case Control::Subtitles:
        menu_ = menu_shown_ = control == Control::Audio ? Menu::Audio : Menu::Subtitles;
        menu_focus_ = std::max(0, menu_selected());
        menu_scroll_ = menu_scroll_target_ = 0;
        break;
    }
}

bool PlayerScreen::press(Button button)
{
    idle_ = 0;
    const bool ready =
        playback_.state == Playback::State::Playing || playback_.state == Playback::State::Paused;
    if (menu_ != Menu::None)
    {
        const int count = static_cast<int>(menu_options().size());
        // The subtitles list has a row above it for their delay, which left and right
        // change a quarter of a second at a time.
        const int first = menu_ == Menu::Subtitles ? (calibration_available_ ? -2 : -1) : 0;
        if (button == Button::Up && menu_focus_ > first)
            --menu_focus_;
        else if (button == Button::Down && menu_focus_ + 1 < count)
            ++menu_focus_;
        else if (menu_focus_ == -2)
        {
            if (button == Button::Accept)
            {
                menu_ = Menu::None;
                if (handler_.calibrate)
                    handler_.calibrate();
            }
            else if (button == Button::Back || button == Button::Left)
                menu_ = Menu::None;
        }
        else if (menu_focus_ == -1)
        {
            if (button == Button::Left || button == Button::Right)
            {
                tracks_.subtitle_delay = std::clamp(
                    tracks_.subtitle_delay + (button == Button::Right ? 0.25 : -0.25), -30.0, 30.0);
                if (handler_.set_subtitle_delay)
                    handler_.set_subtitle_delay(tracks_.subtitle_delay);
            }
            else if (button == Button::Back)
                menu_ = Menu::None;
        }
        else if (button == Button::Accept && menu_focus_ < count)
        {
            const auto &choose = menu_ == Menu::Audio ? handler_.choose_audio : handler_.choose_subtitle;
            if (choose)
                choose(menu_focus_);
            menu_ = Menu::None;
        }
        else if (button == Button::Back || button == Button::Left)
            menu_ = Menu::None;
        return true;
    }
    if (button == Button::Back)
        return false;
    if (!ready)
        return true;
    if (button == Button::Calibrate)
    {
        if (handler_.calibrate)
            handler_.calibrate();
        return true;
    }
    if (button == Button::SkipBack || button == Button::SkipForward)
    {
        skip(button == Button::SkipForward ? kSkip : -kSkip);
        return true;
    }
    if (on_buttons_)
    {
        if (button == Button::Left && button_ > 0)
            --button_;
        else if (button == Button::Right && button_ + 1 < kControls)
            ++button_;
        else if (button == Button::Up)
            on_buttons_ = false;
        else if (button == Button::Accept)
            activate(static_cast<Control>(button_));
        return true;
    }
    switch (button)
    {
    case Button::Accept:
        if (scrubbing_)
            commit_scrub();
        else
            activate(Control::Play);
        break;
    case Button::Down:
        commit_scrub();
        on_buttons_ = true;
        break;
    case Button::Left:
    case Button::Right:
    {
        if (playback_.duration <= 0)
            break;
        if (!scrubbing_)
        {
            scrubbing_ = true;
            scrub_target_ = playback_.position;
            scrub_presses_ = 0;
        }
        // Held or pressed again and again, the steps grow.
        ++scrub_presses_;
        const double step = scrub_presses_ > 20 ? 60.0 : scrub_presses_ > 8 ? 30.0 : kSkip;
        scrub_target_ = std::clamp(scrub_target_ + (button == Button::Right ? step : -step), 0.0,
                                   std::max(0.0, playback_.duration - 1.0));
        scrub_idle_ = 0;
        // The host shows the picture it has for the marker's moment.
        if (handler_.preview)
            handler_.preview(scrub_target_);
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
    const bool stay = playback_.state != Playback::State::Playing || scrubbing_ ||
                      menu_ != Menu::None || idle_ < kControlsStay;
    // The calibration notice is up while there is something to say: the button being
    // held, the calibration running, and its result for a few seconds.
    const Calibration::State calibrating = playback_.calibration.state;
    const bool result = calibrating == Calibration::State::Done || calibrating == Calibration::State::Failed;
    notice_stay_ = result ? notice_stay_ + seconds : 0.0f;
    // It gives way to the track lists, which open over the same corner.
    const bool notice = menu_ == Menu::None &&
                        (hold_ > 0.02f || (calibrating != Calibration::State::Idle && !result) ||
                         (result && notice_stay_ < kNoticeStay));
    notice_ = notice ? std::min(1.0f, notice_ + seconds / 0.15f) : std::max(0.0f, notice_ - seconds / 0.4f);
    const float step = seconds / (stay ? 0.15f : 0.5f);
    controls_ = stay ? std::min(1.0f, controls_ + step) : std::max(0.0f, controls_ - step);
    const bool busy = playback_.state == Playback::State::Opening ||
                      (playback_.state == Playback::State::Playing && playback_.buffering);
    busy_ = busy ? std::min(1.0f, busy_ + seconds / 0.25f) : std::max(0.0f, busy_ - seconds / 0.15f);

    menu_slide_ = eased(menu_slide_, menu_ != Menu::None ? 1.0f : 0.0f, seconds);
    if (menu_ == Menu::None && menu_slide_ < 0.01f)
        menu_shown_ = Menu::None;
    // The focused row is kept inside the list.
    const float window = kMenuBottom() - kMenuTop - (menu_shown_ == Menu::Subtitles ? 2 * kMenuRow + units(0.6f) : 0.0f);
    const float top = static_cast<float>(std::max(0, menu_focus_)) * kMenuRow;
    if (top < menu_scroll_target_)
        menu_scroll_target_ = top;
    else if (top + kMenuRow > menu_scroll_target_ + window)
        menu_scroll_target_ = top + kMenuRow - window;
    menu_scroll_ = eased(menu_scroll_, menu_scroll_target_, seconds);
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

// One of the buttons, centred on (x, y): a disc when it has the focus, and its mark.
void PlayerScreen::draw_control(Control control, float x, float y, float size, bool focused,
                                float alpha)
{
    if (focused)
    {
        nvgBeginPath(vg_);
        nvgCircle(vg_, x, y, size / 2);
        nvgFillColor(vg_, nvgRGBAf(1, 1, 1, 0.16f * alpha));
        nvgFill(vg_);
        nvgBeginPath(vg_);
        nvgCircle(vg_, x, y, size / 2 + kFocusOutline);
        nvgStrokeColor(vg_, foreground(alpha));
        nvgStrokeWidth(vg_, kFocusOutline);
        nvgStroke(vg_);
    }
    const NVGcolor ink = foreground_solid(alpha);
    const float mark = size * 0.46f; // the mark's height
    nvgFillColor(vg_, ink);
    nvgStrokeColor(vg_, ink);
    nvgStrokeWidth(vg_, units(0.17f));
    nvgLineCap(vg_, NVG_ROUND);
    nvgLineJoin(vg_, NVG_ROUND);
    switch (control)
    {
    case Control::Play:
        nvgBeginPath(vg_);
        if (playback_.state == Playback::State::Playing)
        {
            nvgRoundedRect(vg_, x - mark * 0.42f, y - mark / 2, mark * 0.3f, mark, units(0.1f));
            nvgRoundedRect(vg_, x + mark * 0.12f, y - mark / 2, mark * 0.3f, mark, units(0.1f));
        }
        else
        {
            nvgMoveTo(vg_, x - mark * 0.34f, y - mark / 2);
            nvgLineTo(vg_, x + mark * 0.52f, y);
            nvgLineTo(vg_, x - mark * 0.34f, y + mark / 2);
            nvgClosePath(vg_);
        }
        nvgFill(vg_);
        break;
    case Control::Back:
    case Control::Forward:
    {
        // A turning arrow round the number of seconds; the forward one is its mirror.
        const float radius = mark * 0.62f;
        nvgSave(vg_);
        nvgTranslate(vg_, x, y);
        if (control == Control::Forward)
            nvgScale(vg_, -1, 1);
        nvgBeginPath(vg_);
        nvgArc(vg_, 0, 0, radius, -kPi / 2, kPi * 1.17f, NVG_CW);
        nvgStroke(vg_);
        nvgBeginPath(vg_);
        nvgMoveTo(vg_, -radius * 0.62f, -radius);
        nvgLineTo(vg_, radius * 0.05f, -radius * 1.5f);
        nvgLineTo(vg_, radius * 0.05f, -radius * 0.5f);
        nvgClosePath(vg_);
        nvgFill(vg_);
        nvgRestore(vg_);
        nvgFontFace(vg_, "bold");
        nvgFontSize(vg_, radius * 0.95f);
        nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg_, x, y + radius * 0.08f, "15", nullptr);
        break;
    }
    case Control::Audio:
    {
        // A loudspeaker and two waves.
        const float left = x - mark * 0.5f;
        nvgBeginPath(vg_);
        nvgMoveTo(vg_, left, y - mark * 0.18f);
        nvgLineTo(vg_, left + mark * 0.22f, y - mark * 0.18f);
        nvgLineTo(vg_, left + mark * 0.5f, y - mark * 0.42f);
        nvgLineTo(vg_, left + mark * 0.5f, y + mark * 0.42f);
        nvgLineTo(vg_, left + mark * 0.22f, y + mark * 0.18f);
        nvgLineTo(vg_, left, y + mark * 0.18f);
        nvgClosePath(vg_);
        nvgFill(vg_);
        nvgStroke(vg_);
        for (const float reach : {0.24f, 0.46f})
        {
            nvgBeginPath(vg_);
            nvgArc(vg_, left + mark * 0.5f, y, mark * reach, -kPi / 3.4f, kPi / 3.4f, NVG_CW);
            nvgStroke(vg_);
        }
        break;
    }
    case Control::Subtitles:
    {
        // A screen with two lines of text at its foot.
        const float width = mark * 1.2f, height = mark * 0.88f;
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, x - width / 2, y - height / 2, width, height, units(0.22f));
        nvgStroke(vg_);
        nvgBeginPath(vg_);
        nvgMoveTo(vg_, x - width * 0.28f, y + height * 0.02f);
        nvgLineTo(vg_, x + width * 0.28f, y + height * 0.02f);
        nvgMoveTo(vg_, x - width * 0.16f, y + height * 0.24f);
        nvgLineTo(vg_, x + width * 0.16f, y + height * 0.24f);
        nvgStroke(vg_);
        break;
    }
    }
    nvgLineCap(vg_, NVG_BUTT);
    nvgLineJoin(vg_, NVG_MITER);
}

// While scrubbing: a small picture of the moment the marker is at, with its time.
void PlayerScreen::draw_scrub_preview(float thumb_x, float bar_top, float alpha)
{
    const bool hours = playback_.duration >= 3600;
    const std::string label = clock_text(scrub_target_, hours);
    const float width = units(15.0f), height = width * 9.0f / 16.0f;
    const float left = std::clamp(thumb_x - width / 2, kSide, kScreenWidth() - kSide - width);
    const float top = bar_top - units(2.6f) - height;
    // The picture on hand is shown while it is of a moment near the marker's; a newer one
    // replaces it when it arrives.
    const bool pictured = playback_.preview_image != 0;
    float label_middle = bar_top - units(2.3f);
    if (pictured)
    {
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, left - units(0.15f), top - units(0.15f), width + units(0.3f),
                       height + units(0.3f), kRadius);
        nvgFillColor(vg_, foreground_solid(alpha));
        nvgFill(vg_);
        int image_width = 0, image_height = 0;
        nvgImageSize(vg_, playback_.preview_image, &image_width, &image_height);
        // Scaled to cover the frame, whatever the video's shape.
        const float scale = std::max(width / static_cast<float>(std::max(1, image_width)),
                                     height / static_cast<float>(std::max(1, image_height)));
        const float shown_width = image_width * scale, shown_height = image_height * scale;
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, left, top, width, height, kRadius * 0.8f);
        nvgFillColor(vg_, nvgRGBAf(0, 0, 0, alpha));
        nvgFill(vg_);
        nvgFillPaint(vg_, nvgImagePattern(vg_, left + (width - shown_width) / 2,
                                          top + (height - shown_height) / 2, shown_width,
                                          shown_height, 0, playback_.preview_image, alpha));
        nvgFill(vg_);
        label_middle = top + height - units(1.4f);
    }
    nvgFontFace(vg_, "semibold");
    nvgFontSize(vg_, units(1.2f));
    const float label_width = nvgTextBounds(vg_, 0, 0, label.c_str(), nullptr, nullptr) + units(1.4f);
    const float label_left =
        pictured ? left + (width - label_width) / 2
                 : std::clamp(thumb_x - label_width / 2, kSide, kScreenWidth() - kSide - label_width);
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, label_left, label_middle - units(1.1f), label_width, units(2.2f), units(0.5f));
    nvgFillColor(vg_, nvgRGBAf(0.06f, 0.06f, 0.08f, 0.9f * alpha));
    nvgFill(vg_);
    nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg_, foreground(alpha));
    nvgText(vg_, label_left + label_width / 2, label_middle, label.c_str(), nullptr);
}

void draw_subtitle_text(NVGcontext *vg, const SubtitleStyle &style, const std::string &text,
                        float centre, float bottom, float width, float scale)
{
    const float size = units(2.0f) * static_cast<float>(style.size) / 100.0f * scale;
    const float line_height = size * 1.55f;
    nvgFontFace(vg, style.bold ? "bold" : "medium");
    nvgFontSize(vg, size);
    // Each of the subtitle's own lines is wrapped to the width; all are drawn bottom up.
    constexpr int kMost = 8;
    NVGtextRow rows[kMost];
    int count = 0;
    const char *start = text.c_str();
    const char *const end = start + text.size();
    while (start < end && count < kMost)
    {
        const char *stop = start;
        while (stop < end && *stop != '\n')
            ++stop;
        if (stop > start)
            count += nvgTextBreakLines(vg, start, stop, width, rows + count, kMost - count);
        start = stop + 1;
    }
    const auto middle = [&](int row) {
        return bottom - (static_cast<float>(count - 1 - row) + 0.5f) * line_height;
    };
    const SubtitleColour &colour =
        kSubtitleColours[std::clamp(style.colour, 0, static_cast<int>(std::size(kSubtitleColours)) - 1)];
    if (style.background > 0)
    {
        // A plate of translucent black behind each line, as wide as its text. The plates
        // are one shape, so where two lines' plates meet the black is not laid on twice.
        nvgBeginPath(vg);
        for (int row = 0; row < count; ++row)
        {
            const float plate = rows[row].width + size * 0.8f;
            nvgRect(vg, centre - plate / 2, middle(row) - line_height / 2, plate, line_height);
        }
        nvgFillColor(vg, nvgRGBAf(0, 0, 0, static_cast<float>(style.background) / 100.0f));
        nvgFill(vg);
    }
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    for (int row = 0; row < count; ++row)
    {
        if (style.background < 50)
        {
            // With little or no plate, a soft dark edge keeps the letters readable.
            nvgFontBlur(vg, size * 0.1f);
            nvgFillColor(vg, nvgRGBAf(0, 0, 0, 1));
            for (int pass = 0; pass < 3; ++pass)
                nvgText(vg, centre, middle(row) + size * 0.04f, rows[row].start, rows[row].end);
            nvgFontBlur(vg, 0);
        }
        nvgFillColor(vg, nvgRGBAf(colour.red, colour.green, colour.blue, 1));
        nvgText(vg, centre, middle(row), rows[row].start, rows[row].end);
    }
}

// The subtitle, centred near the bottom and lifted clear of the controls when they are up.
void PlayerScreen::draw_subtitle()
{
    if (playback_.subtitle.empty())
        return;
    draw_subtitle_text(vg_, subtitle_style_, playback_.subtitle, kScreenWidth() / 2,
                       kScreenHeight() - units(3.6f) - controls_ * units(8.0f), kScreenWidth() * 0.72f);
}

void PlayerScreen::set_subtitle_style(const SubtitleStyle &style)
{
    subtitle_style_ = style;
}

void PlayerScreen::set_hold(float progress)
{
    hold_ = progress;
}

// The calibration notice: while the button is being held, while listening and working,
// and for a few seconds after with what came of it.
void PlayerScreen::draw_calibration()
{
    if (notice_ < 0.01f)
        return;
    const Calibration &calibration = playback_.calibration;
    using State = Calibration::State;
    const bool holding = calibration.state == State::Idle;
    char text[160];
    switch (calibration.state)
    {
    case State::Idle:
    case State::Listening:
    case State::Working:
        std::snprintf(text, sizeof text, "Calibrating Subtitles");
        break;
    case State::Done:
        if (std::abs(calibration.delay) < 0.01)
            std::snprintf(text, sizeof text, "Subtitles are in time with the dialogue");
        else
            std::snprintf(text, sizeof text, "Subtitles set %.2f s %s", std::abs(calibration.delay),
                          calibration.delay > 0 ? "later" : "earlier");
        break;
    case State::Failed:
        std::snprintf(text, sizeof text, "%s", calibration.message.c_str());
        break;
    }
    const float alpha = notice_;
    const float progress = holding ? hold_ : calibration.progress;
    // A ring that fills (holding, listening), turns (working) or carries the outcome.
    const auto ring = [&](float x, float y, float radius) {
        nvgLineCap(vg_, NVG_ROUND);
        nvgStrokeWidth(vg_, radius * 0.24f);
        nvgBeginPath(vg_);
        nvgCircle(vg_, x, y, radius);
        nvgStrokeColor(vg_, nvgRGBAf(1, 1, 1, 0.2f * alpha));
        nvgStroke(vg_);
        if (calibration.state == State::Working)
        {
            nvgBeginPath(vg_);
            nvgArc(vg_, x, y, radius, spin_, spin_ + 1.6f, NVG_CW);
            nvgStrokeColor(vg_, accent(alpha));
            nvgStroke(vg_);
        }
        else if (calibration.state == State::Done || calibration.state == State::Failed)
        {
            const bool good = calibration.state == State::Done;
            nvgBeginPath(vg_);
            nvgCircle(vg_, x, y, radius);
            nvgStrokeColor(vg_, good ? accent(alpha) : nvgRGBAf(1.0f, 0.6f, 0.3f, alpha));
            nvgStroke(vg_);
            nvgBeginPath(vg_);
            if (good)
            {
                nvgMoveTo(vg_, x - radius * 0.42f, y + radius * 0.02f);
                nvgLineTo(vg_, x - radius * 0.1f, y + radius * 0.34f);
                nvgLineTo(vg_, x + radius * 0.45f, y - radius * 0.3f);
            }
            else
            {
                nvgMoveTo(vg_, x, y - radius * 0.45f);
                nvgLineTo(vg_, x, y + radius * 0.1f);
                nvgMoveTo(vg_, x, y + radius * 0.42f);
                nvgLineTo(vg_, x, y + radius * 0.44f);
            }
            nvgStrokeColor(vg_, foreground_solid(alpha));
            nvgLineJoin(vg_, NVG_ROUND);
            nvgStroke(vg_);
            nvgLineJoin(vg_, NVG_MITER);
        }
        else if (progress > 0.001f)
        {
            nvgBeginPath(vg_);
            nvgArc(vg_, x, y, radius, -kPi / 2, -kPi / 2 + progress * 2 * kPi, NVG_CW);
            nvgStrokeColor(vg_, accent(alpha));
            nvgStroke(vg_);
        }
        nvgLineCap(vg_, NVG_BUTT);
    };
    // A pill in the top right corner: the ring, then the words.
    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, units(1.15f));
    const float height = units(3.2f), top = units(2.0f);
    const float width = nvgTextBounds(vg_, 0, 0, text, nullptr, nullptr) + units(5.2f);
    const float left = kScreenWidth() - kSide - width;
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, left, top, width, height, height / 2);
    nvgFillColor(vg_, nvgRGBAf(0.05f, 0.05f, 0.07f, 0.9f * alpha));
    nvgFill(vg_);
    ring(left + units(1.9f), top + height / 2, units(0.85f));
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg_, foreground(alpha));
    nvgText(vg_, left + units(3.7f), top + height / 2, text, nullptr);
}

void PlayerScreen::draw_menu()
{
    if (menu_shown_ == Menu::None || menu_slide_ < 0.01f)
        return;
    const float left = kScreenWidth() - kMenuWidth * menu_slide_;
    nvgBeginPath(vg_);
    nvgRect(vg_, left, 0, kMenuWidth, kScreenHeight());
    nvgFillColor(vg_, nvgRGBAf(0.04f, 0.04f, 0.05f, 0.94f));
    nvgFill(vg_);
    const float inset = units(2.2f);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontFace(vg_, "semibold");
    nvgFontSize(vg_, units(1.7f));
    nvgFillColor(vg_, foreground());
    const bool audio = menu_shown_ == Menu::Audio;
    nvgText(vg_, left + inset, units(4.6f), audio ? "Audio" : "Subtitles", nullptr);
    if (!audio && tracks_.subtitles_loading)
    {
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(0.95f));
        nvgFillColor(vg_, foreground(0.55f));
        nvgTextAlign(vg_, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(vg_, left + kMenuWidth - inset, units(4.7f), "Asking add-ons", nullptr);
    }

    // Subtitles: two rows of their own above the list. Auto-calibrate times them to the
    // dialogue; the delay is that timing, which left and right also change by hand.
    float list_top = kMenuTop;
    if (!audio)
    {
        const float row_left = left + units(1.2f), row_width = kMenuWidth - units(2.4f);
        const float row_height = kMenuRow - units(0.4f);
        const auto row = [&](int index, float top) {
            const bool focused = menu_ != Menu::None && menu_focus_ == index;
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, row_left, top, row_width, row_height, kRadius * 0.8f);
            nvgFillColor(vg_, nvgRGBAf(1, 1, 1, focused ? 0.14f : 0.06f));
            nvgFill(vg_);
            if (focused)
                focus_ring(vg_, row_left, top, row_width, row_height, kRadius * 0.8f);
            return focused;
        };
        float middle = kMenuTop + row_height / 2;
        if (calibration_available_)
        {
            row(-2, kMenuTop);
            nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFontFace(vg_, "medium");
            nvgFontSize(vg_, units(1.15f));
            nvgFillColor(vg_, foreground(0.95f));
            nvgText(vg_, row_left + units(1.2f), middle, "Auto-calibrate", nullptr);
            nvgTextAlign(vg_, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgFontFace(vg_, "regular");
            nvgFontSize(vg_, units(0.95f));
            nvgFillColor(vg_, foreground(0.55f));
            nvgText(vg_, row_left + row_width - units(1.2f), middle, "or hold triangle", nullptr);
        }

        const float delay_top = kMenuTop + (calibration_available_ ? kMenuRow : 0.0f);
        middle = delay_top + row_height / 2;
        const bool focused = row(-1, delay_top);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFontFace(vg_, "medium");
        nvgFontSize(vg_, units(1.15f));
        nvgFillColor(vg_, foreground(0.85f));
        nvgText(vg_, row_left + units(1.2f), middle, "Delay", nullptr);
        char amount[24];
        std::snprintf(amount, sizeof amount, "%+.2f s", tracks_.subtitle_delay);
        const std::string shown = focused ? std::string{"\xE2\x80\xB9   "} + amount + "   \xE2\x80\xBA" : amount;
        nvgTextAlign(vg_, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgFontFace(vg_, "semibold");
        nvgFillColor(vg_, foreground(1.0f));
        nvgText(vg_, row_left + row_width - units(1.2f), middle, shown.c_str(), nullptr);
        list_top += (calibration_available_ ? 2.0f : 1.0f) * kMenuRow + units(0.6f);
    }

    const std::vector<TrackOption> &options = menu_options();
    const int selected = menu_selected();
    nvgSave(vg_);
    nvgScissor(vg_, left, list_top - units(0.3f), kMenuWidth, kMenuBottom() - list_top + units(0.6f));
    for (std::size_t index = 0; index < options.size(); ++index)
    {
        const float top = list_top + static_cast<float>(index) * kMenuRow - menu_scroll_;
        if (top + kMenuRow < list_top - units(1.0f) || top > kMenuBottom() + units(1.0f))
            continue;
        const bool focused = menu_ != Menu::None && static_cast<int>(index) == menu_focus_;
        const bool chosen = static_cast<int>(index) == selected;
        const float row_left = left + units(1.2f), row_width = kMenuWidth - units(2.4f);
        const float row_height = kMenuRow - units(0.4f);
        if (focused)
        {
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, row_left, top, row_width, row_height, kRadius * 0.8f);
            nvgFillColor(vg_, nvgRGBAf(1, 1, 1, 0.14f));
            nvgFill(vg_);
            focus_ring(vg_, row_left, top, row_width, row_height, kRadius * 0.8f);
        }
        const float middle = top + row_height / 2;
        // The one in use is marked with a dot.
        if (chosen)
        {
            nvgBeginPath(vg_);
            nvgCircle(vg_, row_left + units(1.2f), middle, units(0.32f));
            nvgFillColor(vg_, accent());
            nvgFill(vg_);
        }
        const float text_left = row_left + units(2.4f);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFontFace(vg_, chosen ? "semibold" : "medium");
        nvgFontSize(vg_, units(1.15f));
        nvgFillColor(vg_, foreground(chosen || focused ? 1.0f : 0.85f));
        const float end = nvgText(vg_, text_left, middle, options[index].label.c_str(), nullptr);
        if (!options[index].detail.empty())
        {
            nvgFontFace(vg_, "regular");
            nvgFontSize(vg_, units(0.95f));
            nvgFillColor(vg_, foreground(0.55f));
            fitted_text(vg_, end + units(0.9f), middle + units(0.05f),
                        row_left + row_width - units(0.8f) - end - units(0.9f),
                        drawable(options[index].detail));
        }
    }
    nvgRestore(vg_);
}

void PlayerScreen::draw()
{
    const float centre_x = kScreenWidth() / 2, centre_y = kScreenHeight() / 2;
    if (playback_.state == Playback::State::Failed)
    {
        nvgBeginPath(vg_);
        nvgRect(vg_, 0, 0, kScreenWidth(), kScreenHeight());
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
    draw_subtitle();
    if (busy_ > 0.01f)
        draw_spinner(centre_x, centre_y, units(2.0f), busy_);
    if (controls_ < 0.01f)
    {
        draw_calibration();
        draw_menu();
        return;
    }
    const float shown = controls_;

    // Shade at the top and the bottom, so the controls read over a bright picture.
    const auto shade = [&](float top, float height, bool lower) {
        const NVGcolor dark = nvgRGBAf(0, 0, 0, 0.82f * shown), clear = nvgRGBAf(0, 0, 0, 0);
        nvgBeginPath(vg_);
        nvgRect(vg_, 0, top, kScreenWidth(), height);
        nvgFillPaint(vg_, nvgLinearGradient(vg_, 0, top, 0, top + height, lower ? clear : dark,
                                            lower ? dark : clear));
        nvgFill(vg_);
    };
    shade(0, units(9.0f), false);
    shade(kScreenHeight() - units(15.0f), units(15.0f), true);

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
    fitted_text(vg_, kSide + units(2.2f), kTitleMiddle, kScreenWidth() * 0.8f, drawable(title_));

    // The seek bar: a little thicker while the focus is on it.
    const double duration = playback_.duration;
    const double position = scrubbing_ ? scrub_target_ : playback_.position;
    const float along =
        duration > 0 ? static_cast<float>(std::clamp(position / duration, 0.0, 1.0)) : 0.0f;
    const bool bar_focused = !on_buttons_ && menu_ == Menu::None;
    const float bar_width = kScreenWidth() - 2 * kSide;
    const float thickness = bar_focused ? kBarHeight * 1.5f : kBarHeight;
    const float track_top = kBarTop() + (kBarHeight - thickness) / 2;
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, kSide, track_top, bar_width, thickness, thickness / 2);
    nvgFillColor(vg_, nvgRGBAf(1, 1, 1, 0.28f * shown));
    nvgFill(vg_);
    if (scrubbing_ && duration > 0)
    {
        // Where the video still is, while the marker is away from it.
        const float playing = static_cast<float>(std::clamp(playback_.position / duration, 0.0, 1.0));
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, kSide, track_top, bar_width * playing, thickness, thickness / 2);
        nvgFillColor(vg_, nvgRGBAf(1, 1, 1, 0.35f * shown));
        nvgFill(vg_);
    }
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, kSide, track_top, bar_width * along, thickness, thickness / 2);
    nvgFillColor(vg_, accent(shown));
    nvgFill(vg_);
    const float thumb_x = kSide + bar_width * along, thumb_y = kBarTop() + kBarHeight / 2;
    nvgBeginPath(vg_);
    nvgCircle(vg_, thumb_x, thumb_y, scrubbing_ ? units(0.75f) : bar_focused ? units(0.6f) : units(0.45f));
    nvgFillColor(vg_, foreground_solid(shown));
    nvgFill(vg_);
    if (scrubbing_)
        draw_scrub_preview(thumb_x, kBarTop(), shown);

    // Under the bar: the transport buttons and the time on the left, the track buttons on
    // the right.
    const auto focused = [&](int index) {
        return on_buttons_ && menu_ == Menu::None && index == button_;
    };
    float x = kSide + kButton / 2;
    for (int index = 0; index < 3; ++index)
    {
        draw_control(static_cast<Control>(index), x, kRowMiddle(), kButton, focused(index), shown);
        x += kButtonPitch;
    }
    const bool hours = duration >= 3600;
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontFace(vg_, "medium");
    nvgFontSize(vg_, units(1.15f));
    nvgFillColor(vg_, foreground(shown));
    const float end = nvgText(vg_, x - kButton / 2 + units(0.6f), kRowMiddle(),
                              clock_text(position, hours).c_str(), nullptr);
    if (duration > 0)
    {
        nvgFillColor(vg_, foreground(0.6f * shown));
        nvgText(vg_, end, kRowMiddle(), ("  /  " + clock_text(duration, hours)).c_str(), nullptr);
    }
    x = kScreenWidth() - kSide - kButton / 2 - kButtonPitch;
    for (int index = 3; index < kControls; ++index)
    {
        draw_control(static_cast<Control>(index), x, kRowMiddle(), kButton, focused(index), shown);
        x += kButtonPitch;
    }
    draw_calibration();
    draw_menu();
}
} // namespace ui
