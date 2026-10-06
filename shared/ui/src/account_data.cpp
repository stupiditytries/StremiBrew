#include "account_data.hpp"

#include "nlohmann/json.hpp"

namespace ui
{
namespace
{
std::string text(const nlohmann::json &object, const char *key)
{
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}
} // namespace

bool parse_account(std::string_view json, Account &account)
{
    const auto document = nlohmann::json::parse(json, nullptr, false);
    if (!document.is_object())
        return false;
    Account parsed;
    parsed.signed_in = document.value("signedIn", false);
    parsed.email = text(document, "email");
    parsed.addons = document.value("addons", 0);
    const std::string link = text(document, "link");
    parsed.link = link == "requesting"   ? Account::Link::Requesting
                  : link == "waiting"    ? Account::Link::Waiting
                  : link == "signing_in" ? Account::Link::SigningIn
                  : link == "error"      ? Account::Link::Error
                                         : Account::Link::Idle;
    parsed.code = text(document, "code");
    parsed.link_page = text(document, "linkPage");
    parsed.error = text(document, "error");
    parsed.audio_language = text(document, "audioLanguage");
    parsed.subtitles_language = text(document, "subtitlesLanguage");
    account = std::move(parsed);
    return true;
}
} // namespace ui
