// The Stremio account's state as the core bridge delivers it (stremio_core_account).

#pragma once

#include <string>
#include <string_view>

namespace ui
{
struct Account
{
    enum class Link
    {
        Idle,       // no sign-in in progress
        Requesting, // asking Stremio for a code
        Waiting,    // a code is on screen, waiting for it to be entered
        SigningIn,  // the code was entered; the account is loading
        Error,
    };

    bool signed_in = false;
    std::string email;
    int addons = 0;
    Link link = Link::Idle;
    std::string code;      // the code to enter
    std::string link_page; // the page to enter it on
    std::string error;
    // The preferred languages (three-letter codes); no subtitle language means off.
    std::string audio_language;
    std::string subtitles_language;
};

// Parses the bridge's JSON. Returns false and leaves `account` untouched when it is not valid.
bool parse_account(std::string_view json, Account &account);
} // namespace ui
