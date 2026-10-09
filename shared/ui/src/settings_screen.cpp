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
// (The last section's settings after the first are offered only where the host has a
// screen that can be held in the hand: see App::set_display_options.)
constexpr const char *kSections[] = {"Account", "Playback", "Subtitles", "Auto-calibrate", "Interface"};
constexpr int kSectionCount = 5;
// How many settings each section has.
constexpr int kRows[kSectionCount] = {1, 4, 4, 3, 3};
constexpr const char *kLabels[kSectionCount][4] = {
    {"", "", "", ""},
    {"Audio language", "Subtitles", "Trailers on the home screen", "Maximum stream quality"},
    {"Size", "Background", "Colour", "Weight"},
    {"Speech model", "Download Status", "Calibration Offset", ""},
    {"Sound effects", "Switch UI automatically", "Handheld UI", ""},
};

constexpr float kRowHeight = units(3.25f), kSettingGap = units(0.6f);
constexpr float kTop = kTopBarHeight + units(1.2f);
} // namespace

// Moves the setting at (section, row) to its next or previous choice.
void App::change_setting(int section, int row, int step)
{
    if (section == 4 && row == 0)
    {
        sound_effects_ = !sound_effects_;
        if (subtitle_style_handler_)
            subtitle_style_handler_(subtitle_style_); // the host saves all of these together
    }
    else if (section == 4)
    {
        // Choosing the handheld UI (or not) by hand ends the automatic choosing.
        if (row == 1)
            handheld_auto_ = !handheld_auto_;
        else
        {
            handheld_ui_ = handheld_auto_ ? docked_ : !handheld_ui_;
            handheld_auto_ = false;
        }
        if (display_handler_)
            display_handler_(handheld_ui_, handheld_auto_);
    }
    else if (section == 1 && row < 2)
        change_language(row == 1, step);
    else if (section == 1 && row == 3)
    {
        // No limit, then the picture heights streams may have at most.
        constexpr int kLimits[] = {0, 1080, 720};
        constexpr int kCount = static_cast<int>(std::size(kLimits));
        int at = 0;
        for (int index = 0; index < kCount; ++index)
            if (kLimits[index] == quality_limit_)
                at = index;
        quality_limit_ = kLimits[((at + step) % kCount + kCount) % kCount];
        if (quality_handler_)
            quality_handler_(quality_limit_);
    }
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

// Whether this host has a setting: not the ones it has no use for.
bool App::setting_shown(int section, int row) const
{
    if (row < 0 || row >= kRows[section])
        return false;
    if (section == 3)
        return features_.calibration;
    if (section == 4)
        return row == 0 || display_options_;
    if (section == 1 && row == 2)
        return features_.trailers;
    if (section == 1 && row == 3)
        return features_.quality_limit;
    return true;
}

// How many of a section's settings this host has (a section with none is not shown).
int App::settings_rows(int section) const
{
    int count = 0;
    for (int row = 0; row < kRows[section]; ++row)
        count += setting_shown(section, row) ? 1 : 0;
    return count;
}

void App::press_settings(Button button)
{
    int &section = settings_section_, &row = settings_row_;
    // The nearest section in a direction that has settings, or -1.
    const auto neighbour = [this](int from, int step) {
        for (int other = from + step; other >= 0 && other < kSectionCount; other += step)
            if (settings_rows(other) > 0)
                return other;
        return -1;
    };
    // The nearest setting of a section in a direction that this host has, or -1.
    const auto beside = [this](int of, int from, int step) {
        for (int other = from + step; other >= 0 && other < kRows[of]; other += step)
            if (setting_shown(of, other))
                return other;
        return -1;
    };
    if (button == Button::Up)
    {
        if (const int before = beside(section, row, -1); before >= 0)
            row = before;
        else if (const int above = neighbour(section, -1); above >= 0)
        {
            section = above;
            row = beside(section, kRows[section], -1);
        }
        else
            zone_ = Zone::Search;
    }
    else if (button == Button::Down)
    {
        if (const int after = beside(section, row, 1); after >= 0)
            row = after;
        else if (const int below = neighbour(section, 1); below >= 0)
        {
            section = below;
            row = beside(section, -1, 1);
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
    if (section == 4)
        return (row == 0 ? sound_effects_ : row == 1 ? handheld_auto_ : handheld_now()) ? "On" : "Off";
    if (section == 1)
    {
        if (row == 0)
            return language_name(account_.audio_language);
        if (row == 1)
            return account_.subtitles_language.empty() ? std::string{"Off"}
                                                       : language_name(account_.subtitles_language);
        if (row == 3)
            return quality_limit_ == 0 ? std::string{"No limit"} : std::to_string(quality_limit_) + "p";
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
    const float bottom = kScreenHeight() - units(2.0f);
    // Where the focused setting falls in the list, to keep it in view.
    float y = 0, focus_y = 0;
    for (int section = 0; section < kSectionCount; ++section)
    {
        if (settings_rows(section) == 0)
            continue;
        y += kRowTitleSize * 1.2f + units(1.0f);
        for (int row = 0; row < kRows[section]; ++row)
        {
            if (!setting_shown(section, row))
                continue;
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
    nvgScissor(vg_, kNavWidth, kTopBarHeight, kScreenWidth() - kNavWidth, kScreenHeight() - kTopBarHeight);
    y = kTop - settings_scroll_;
    for (int section = 0; section < kSectionCount; ++section)
    {
        if (settings_rows(section) == 0)
            continue;
        y += heading(left, y, kSections[section]);
        for (int row = 0; row < kRows[section]; ++row)
        {
            if (!setting_shown(section, row))
                continue;
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
    const float side = left + width + units(4.0f), side_width = kScreenWidth() - side - units(4.0f);
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
    else if (settings_side_ == 4)
    {
        side_top += heading(side, side_top, "Interface");
        nvgFontFace(vg_, "regular");
        nvgFontSize(vg_, units(1.1f));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, foreground(0.6f));
        nvgTextLineHeight(vg_, 1.4f);
        std::string about = "Sound effects are the quiet tones as the focus moves and things are chosen.";
        if (display_options_)
            about += "\n\nThe handheld UI makes everything a little larger, for the console's own screen. "
                     "Switching automatically uses it in the hand and the TV UI when docked.";
        nvgTextBox(vg_, side, side_top, side_width, about.c_str(), nullptr);
        nvgTextLineHeight(vg_, 1.0f);
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
