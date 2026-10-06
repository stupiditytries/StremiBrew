// The Settings screen (part of App): the account, what plays by default, how subtitles
// look, and subtitle auto-calibration.
//
// The settings are one list, each section under its name; up and down go through all of
// them, left and right change the one the focus is on. Beside the list is whatever goes
// with the section the focus is in: the account's details, or a sample of the subtitles.

#include "app.hpp"

#include <algorithm>
#include <cstdio>

#include "draw_util.hpp"
#include "languages.hpp"
#include "nanovg.h"
#include "theme.hpp"

namespace ui
{
using namespace theme;

namespace
{
constexpr const char *kSections[] = {"Account", "Playback", "Subtitles", "Auto-calibrate"};
constexpr int kSectionCount = 4;
// How many settings each section has.
constexpr int kRows[kSectionCount] = {1, 3, 4, 3};
constexpr const char *kLabels[kSectionCount][4] = {
    {"", "", "", ""},
    {"Audio language", "Subtitles", "Trailers on the home screen", ""},
    {"Size", "Background", "Colour", "Weight"},
    {"Speech model", "Download Status", "Calibration Offset", ""},
};

constexpr float kRowHeight = units(3.25f), kSettingGap = units(0.6f);
constexpr float kTop = kTopBarHeight + units(1.2f);
} // namespace

// Moves the setting at (section, row) to its next or previous choice.
void App::change_setting(int section, int row, int step)
{
    if (section == 1 && row < 2)
        change_language(row == 1, step);
    else if (section == 1)
    {
        trailers_ = !trailers_;
        if (subtitle_style_handler_)
            subtitle_style_handler_(subtitle_style_); // the host saves all of these together
    }
    else if (section == 2)
        change_subtitle_style(3 + row, step);
    else if (section == 3 && row == 0)
    {
        speech_.chosen = ((speech_.chosen + step) % kSpeechModelCount + kSpeechModelCount) % kSpeechModelCount;
        speech_shift_ = step < 0 ? -units(1.6f) : units(1.6f);
        speech_alpha_ = 0;
        if (choose_speech_)
            choose_speech_(speech_.chosen);
    }
    else if (section == 3 && row == 2)
        change_subtitle_style(7, step);
}

// Whether the setting is a button (pressed) rather than a value (changed).
static bool is_button(int section, int row)
{
    return section == 0 || (section == 3 && row == 1);
}

void App::press_setting(int section, int row)
{
    if (section == 0)
    {
        // The account's one button does whatever its state calls for.
        if (!intent_)
            return;
        if (account_.signed_in)
            intent_(Intent::SignOut);
        else if (account_.link == Account::Link::Idle || account_.link == Account::Link::Error)
            intent_(Intent::SignIn);
        else
            intent_(Intent::CancelSignIn);
    }
    else if (section == 3 && row == 1)
    {
        if (!speech_.ready[speech_.chosen] && speech_.downloading < 0 && download_speech_)
            download_speech_(speech_.chosen);
    }
    else
        change_setting(section, row, 1);
}

void App::press_settings(Button button)
{
    int &section = settings_section_, &row = settings_row_;
    if (button == Button::Up)
    {
        if (row > 0)
            --row;
        else if (section > 0)
            row = kRows[--section] - 1;
        else
            zone_ = Zone::Search;
    }
    else if (button == Button::Down)
    {
        if (row + 1 < kRows[section])
            ++row;
        else if (section + 1 < kSectionCount)
        {
            ++section;
            row = 0;
        }
    }
    else if (button == Button::Back || (button == Button::Left && is_button(section, row)))
        leave_content();
    else if (button == Button::Accept)
        press_setting(section, row);
    else if ((button == Button::Left || button == Button::Right) && !is_button(section, row))
        change_setting(section, row, button == Button::Left ? -1 : 1);
}

// What a setting is set to, as its row shows it.
std::string App::setting_value(int section, int row) const
{
    char text[32];
    if (section == 1)
    {
        if (row == 0)
            return language_name(account_.audio_language);
        if (row == 1)
            return account_.subtitles_language.empty() ? std::string{"Off"}
                                                       : language_name(account_.subtitles_language);
        return trailers_ ? "On" : "Off";
    }
    if (section == 2)
    {
        const SubtitleStyle &style = subtitle_style_;
        if (row == 0)
            return std::to_string(style.size) + "%";
        if (row == 1)
            return style.background == 0 ? std::string{"None"} : std::to_string(style.background) + "%";
        if (row == 2)
            return kSubtitleColours[style.colour].name;
        return style.bold ? "Bold" : "Regular";
    }
    if (section == 3 && row == 0)
        return std::string{kSpeechModels[speech_.chosen].name} + "  \xC2\xB7  " + kSpeechModels[speech_.chosen].size;
    if (section == 3 && row == 1)
    {
        const bool busy = speech_.downloading == speech_.chosen;
        return busy                                ? std::to_string(speech_.progress) + "%"
               : speech_.ready[speech_.chosen]     ? "Downloaded"
               : speech_.downloading >= 0          ? "Waiting for the other download"
               : !speech_.error.empty()            ? "Failed  \xC2\xB7  Try again"
                                                   : "Not downloaded  \xC2\xB7  Download";
    }
    std::snprintf(text, sizeof text, "%+.2f s", subtitle_style_.calibration_offset / 1000.0);
    return text;
}

// One setting: its name on the left, what it is set to on the right, with arrows either
// side of that while the focus is on it.
void App::draw_setting(int section, int row, float x, float y, float width, bool focused)
{
    const bool download = section == 3 && row == 1;
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, x, y, width, kRowHeight, kRadius);
    nvgFillColor(vg_, overlay(focused ? 3.0f : 1.6f));
    nvgFill(vg_);
    if (download && speech_.downloading == speech_.chosen)
    {
        // The row fills from the left as the download goes.
        nvgSave(vg_);
        nvgScissor(vg_, x, y, width * static_cast<float>(speech_.progress) / 100.0f, kRowHeight);
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, x, y, width, kRowHeight, kRadius);
        nvgFillColor(vg_, accent(0.45f));
        nvgFill(vg_);
        nvgRestore(vg_);
    }
    if (focused)
        focus_ring(vg_, x, y, width, kRowHeight, kRadius);
    const float middle = y + kRowHeight / 2;
    nvgFontFace(vg_, "regular");
    nvgFontSize(vg_, units(1.15f));
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg_, foreground(0.75f));
    nvgText(vg_, x + units(1.2f), middle, kLabels[section][row], nullptr);

    const std::string value = setting_value(section, row);
    // The speech model's name and its status slide in when the model is changed.
    const bool slides = section == 3 && row < 2;
    const float shift = slides ? speech_shift_ : 0.0f, alpha = slides ? speech_alpha_ : 1.0f;
    NVGcolor colour = download && speech_.ready[speech_.chosen] ? accent() : foreground(1.0f);
    colour.a *= alpha;
    nvgFontFace(vg_, "semibold");
    nvgTextAlign(vg_, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    const float right = x + width - units(1.2f);
    if (focused && !is_button(section, row))
    {
        const float value_width = nvgTextBounds(vg_, 0, 0, value.c_str(), nullptr, nullptr);
        const float arrow = nvgTextBounds(vg_, 0, 0, "\xE2\x80\xBA", nullptr, nullptr) + units(0.7f);
        nvgFillColor(vg_, foreground(1.0f));
        nvgText(vg_, right, middle, "\xE2\x80\xBA", nullptr);
        nvgText(vg_, right - arrow - value_width - units(0.7f), middle, "\xE2\x80\xB9", nullptr);
        nvgFillColor(vg_, colour);
        nvgText(vg_, right - arrow + shift, middle, value.c_str(), nullptr);
    }
    else
    {
        nvgFillColor(vg_, colour);
        nvgText(vg_, right + shift, middle, value.c_str(), nullptr);
    }
}

// The account: who is signed in, or how to sign in (a code to confirm on another device).
void App::draw_account(float left, float top, float width)
{
    const auto line = [&](const char *face, float size, NVGcolor color, const std::string &text) {
        nvgFontFace(vg_, face);
        nvgFontSize(vg_, size);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, color);
        nvgTextBox(vg_, left, top, width, text.c_str(), nullptr);
        float bounds[4];
        nvgTextBoxBounds(vg_, left, top, width, text.c_str(), nullptr, bounds);
        top += std::max(size * 1.5f, bounds[3] - bounds[1] + size * 0.5f);
    };
    if (account_.signed_in)
    {
        line("regular", units(1.1f), foreground(0.6f), "Signed in as");
        line("semibold", units(1.6f), foreground(), account_.email);
        top += units(0.4f);
        line("regular", units(1.1f), foreground(0.6f), std::to_string(account_.addons) + " add-ons installed");
    }
    else
    {
        switch (account_.link)
        {
        case Account::Link::Idle:
            line("regular", units(1.2f), foreground(0.75f),
                 "Sign in to use your add-ons, library and watch progress.");
            break;
        case Account::Link::Requesting:
            line("regular", units(1.2f), foreground(0.75f), "Getting a code from Stremio");
            break;
        case Account::Link::Waiting:
        {
            // The page's address is shown without its scheme, as a person would type it.
            std::string page = account_.link_page;
            if (const auto scheme = page.find("://"); scheme != std::string::npos)
                page.erase(0, scheme + 3);
            line("regular", units(1.2f), foreground(0.75f),
                 "On a phone or computer where you are signed in to Stremio, open");
            top += units(0.3f);
            line("semibold", units(2.0f), accent(), page);
            top += units(0.6f);
            line("regular", units(1.2f), foreground(0.75f), "and check that it shows this code:");
            top += units(0.4f);
            // The code, one box per character.
            nvgFontFace(vg_, "bold");
            nvgFontSize(vg_, units(3.2f));
            nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            const float box = units(4.6f), gap = units(0.8f);
            float x = left;
            for (const char letter : account_.code)
            {
                nvgBeginPath(vg_);
                nvgRoundedRect(vg_, x, top, box, box * 1.15f, kRadius);
                nvgFillColor(vg_, overlay(1.6f));
                nvgFill(vg_);
                const char text[2] = {letter, 0};
                nvgFillColor(vg_, foreground(1.0f));
                nvgText(vg_, x + box / 2, top + box * 1.15f / 2, text, nullptr);
                x += box + gap;
            }
            top += box * 1.15f + units(1.2f);
            line("regular", units(1.1f), foreground(0.6f),
                 "This screen signs in by itself once you confirm it there.");
            break;
        }
        case Account::Link::SigningIn:
            line("regular", units(1.2f), foreground(0.75f), "Signing in");
            break;
        case Account::Link::Error:
            line("regular", units(1.2f), foreground(0.75f), "Signing in did not work:");
            line("regular", units(1.1f), foreground(0.6f), account_.error);
            break;
        }
    }
}

// A sample of the subtitles as they are set to look, on a patch of mid grey standing in
// for a picture.
void App::draw_subtitle_sample(float left, float top, float width, float height)
{
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, left, top, width, height, kRadius);
    nvgFillPaint(vg_, nvgLinearGradient(vg_, left, top, left + width, top + height,
                                        nvgRGBf(0.42f, 0.46f, 0.52f), nvgRGBf(0.2f, 0.22f, 0.26f)));
    nvgFill(vg_);
    nvgSave(vg_);
    nvgScissor(vg_, left, top, width, height);
    draw_subtitle_text(vg_, subtitle_style_, "Subtitles look like this,\nover whatever is playing.",
                       left + width / 2, top + height - units(1.2f), width - units(2.0f), 0.7f);
    nvgRestore(vg_);
}

void App::draw_settings()
{
    const bool active = zone_ == Zone::Content;
    const float left = kNavWidth + units(2.5f);
    const auto heading = [&](float x, float y, const char *text) {
        nvgFontFace(vg_, "medium");
        nvgFontSize(vg_, kRowTitleSize);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground());
        nvgText(vg_, x, y, text, nullptr);
        return kRowTitleSize * 1.2f + units(1.0f);
    };
    const float width = units(34.0f);
    const float bottom = kScreenHeight - units(2.0f);
    // Where the focused setting falls in the list, to keep it in view.
    float y = 0, focus_y = 0;
    for (int section = 0; section < kSectionCount; ++section)
    {
        y += kRowTitleSize * 1.2f + units(1.0f);
        for (int row = 0; row < kRows[section]; ++row)
        {
            if (section == settings_section_ && row == settings_row_)
                focus_y = y;
            y += kRowHeight + kSettingGap;
        }
        y += units(1.6f);
    }
    const float window = bottom - kTop;
    float wanted = settings_scroll_target_;
    if (focus_y - units(5.0f) < wanted)
        wanted = std::max(0.0f, focus_y - units(5.0f));
    else if (focus_y + kRowHeight + units(3.0f) > wanted + window)
        wanted = focus_y + kRowHeight + units(3.0f) - window;
    settings_scroll_target_ = wanted;
    settings_scroll_ = eased(settings_scroll_, wanted, frame_seconds_);

    nvgSave(vg_);
    nvgScissor(vg_, kNavWidth, kTopBarHeight, kScreenWidth - kNavWidth, kScreenHeight - kTopBarHeight);
    y = kTop - settings_scroll_;
    for (int section = 0; section < kSectionCount; ++section)
    {
        y += heading(left, y, kSections[section]);
        for (int row = 0; row < kRows[section]; ++row)
        {
            const bool focused = active && section == settings_section_ && row == settings_row_;
            if (section == 0)
            {
                // The account's row: who is signed in, and what pressing it does.
                const char *does = account_.signed_in ? "Sign out"
                                   : account_.link == Account::Link::Idle ? "Sign in"
                                   : account_.link == Account::Link::Error ? "Try again"
                                                                           : "Cancel";
                nvgBeginPath(vg_);
                nvgRoundedRect(vg_, left, y, width, kRowHeight, kRadius);
                nvgFillColor(vg_, overlay(focused ? 3.0f : 1.6f));
                nvgFill(vg_);
                if (focused)
                    focus_ring(vg_, left, y, width, kRowHeight, kRadius);
                nvgFontFace(vg_, "regular");
                nvgFontSize(vg_, units(1.15f));
                nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgFillColor(vg_, foreground(0.75f));
                fitted_text(vg_, left + units(1.2f), y + kRowHeight / 2, width - units(10.0f),
                            account_.signed_in ? account_.email : std::string{"Not signed in"});
                nvgFontFace(vg_, "semibold");
                nvgTextAlign(vg_, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                nvgFillColor(vg_, foreground(1.0f));
                nvgText(vg_, left + width - units(1.2f), y + kRowHeight / 2, does, nullptr);
            }
            else
                draw_setting(section, row, left, y, width, focused);
            y += kRowHeight + kSettingGap;
        }
        y += units(1.6f);
    }
    nvgRestore(vg_);

    // Beside the list, what goes with the section the focus is in.
    const float side = left + width + units(4.0f), side_width = kScreenWidth - side - units(4.0f);
    float side_top = kTop;
    // It fades out, changes, and fades in as the focus goes from section to section.
    if (settings_side_ != settings_section_)
    {
        settings_side_alpha_ = std::max(0.0f, settings_side_alpha_ - frame_seconds_ / 0.1f);
        if (settings_side_alpha_ <= 0.0f)
            settings_side_ = settings_section_;
    }
    else
        settings_side_alpha_ = std::min(1.0f, settings_side_alpha_ + frame_seconds_ / 0.22f);
    nvgSave(vg_);
    nvgGlobalAlpha(vg_, settings_side_alpha_);
    nvgTranslate(vg_, (1.0f - settings_side_alpha_) * units(0.8f), 0);
    if (settings_side_ == 0)
    {
        side_top += heading(side, side_top, "Account");
        draw_account(side, side_top, side_width);
    }
    else if (settings_side_ == 2)
    {
        side_top += heading(side, side_top, "Preview");
        draw_subtitle_sample(side, side_top, side_width, units(9.0f));
    }
    else if (settings_side_ == 3)
    {
        side_top += heading(side, side_top, "Auto-calibrate");
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.1f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground(0.6f));
        nvgTextBox(vg_, side, side_top, side_width,
                   "In the player, hold triangle to time the subtitles to what is being said.", nullptr);
    }
    nvgRestore(vg_);
}
} // namespace ui
