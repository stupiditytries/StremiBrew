// Language codes as add-ons, video files and the Stremio profile write them, and the
// names shown for them.

#pragma once

#include <string>
#include <string_view>

namespace ui
{
struct Language
{
    const char *code;  // three letters (ISO 639-2), as the Stremio profile stores it
    const char *other; // the other three-letter form, when there is one
    const char *brief; // two letters (ISO 639-1)
    const char *name;
};

// The languages offered in Settings, in the order they are offered.
inline constexpr Language kLanguages[] = {
    {"eng", "", "en", "English"},      {"spa", "", "es", "Spanish"},
    {"fre", "fra", "fr", "French"},    {"ger", "deu", "de", "German"},
    {"ita", "", "it", "Italian"},      {"por", "pob", "pt", "Portuguese"},
    {"dut", "nld", "nl", "Dutch"},     {"swe", "", "sv", "Swedish"},
    {"nor", "nob", "no", "Norwegian"}, {"dan", "", "da", "Danish"},
    {"fin", "", "fi", "Finnish"},      {"pol", "", "pl", "Polish"},
    {"cze", "ces", "cs", "Czech"},     {"slo", "slk", "sk", "Slovak"},
    {"hun", "", "hu", "Hungarian"},    {"rum", "ron", "ro", "Romanian"},
    {"bul", "", "bg", "Bulgarian"},    {"gre", "ell", "el", "Greek"},
    {"hrv", "", "hr", "Croatian"},     {"srp", "", "sr", "Serbian"},
    {"slv", "", "sl", "Slovenian"},    {"rus", "", "ru", "Russian"},
    {"ukr", "", "uk", "Ukrainian"},    {"tur", "", "tr", "Turkish"},
    {"ara", "", "ar", "Arabic"},       {"heb", "", "he", "Hebrew"},
    {"per", "fas", "fa", "Persian"},   {"hin", "", "hi", "Hindi"},
    {"jpn", "", "ja", "Japanese"},     {"kor", "", "ko", "Korean"},
    {"chi", "zho", "zh", "Chinese"},   {"tha", "", "th", "Thai"},
    {"vie", "", "vi", "Vietnamese"},   {"ind", "", "id", "Indonesian"},
    {"may", "msa", "ms", "Malay"},     {"est", "", "et", "Estonian"},
    {"lav", "", "lv", "Latvian"},      {"lit", "", "lt", "Lithuanian"},
    {"ice", "isl", "is", "Icelandic"}, {"cat", "", "ca", "Catalan"},
};

// The entry for a code in any of its forms (letter case is ignored), or null.
inline const Language *find_language(std::string_view code)
{
    const auto same = [](std::string_view left, std::string_view right) {
        if (left.size() != right.size() || left.empty())
            return false;
        for (std::size_t index = 0; index < left.size(); ++index)
            if ((left[index] | 0x20) != (right[index] | 0x20))
                return false;
        return true;
    };
    // Some sources add a region ("pt-BR"); only the language part is looked up.
    if (const auto cut = code.find_first_of("-_"); cut != std::string_view::npos)
        code = code.substr(0, cut);
    for (const Language &language : kLanguages)
        if (same(code, language.code) || same(code, language.other) || same(code, language.brief) ||
            same(code, language.name))
            return &language;
    return nullptr;
}

// The name to show for a code; the code itself when it is not one of the known ones.
inline std::string language_name(std::string_view code)
{
    if (const Language *language = find_language(code))
        return language->name;
    return code.empty() ? std::string{"Unknown"} : std::string{code};
}

// Whether two codes name the same language.
inline bool same_language(std::string_view left, std::string_view right)
{
    const Language *first = find_language(left);
    return first != nullptr && first == find_language(right);
}
} // namespace ui
