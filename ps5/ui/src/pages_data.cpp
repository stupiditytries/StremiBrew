#include "pages_data.hpp"

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

std::vector<std::string> texts(const nlohmann::json &object, const char *key)
{
    std::vector<std::string> list;
    if (const auto found = object.find(key); found != object.end() && found->is_array())
        for (const auto &entry : *found)
            if (entry.is_string())
                list.push_back(entry.get<std::string>());
    return list;
}
} // namespace

bool parse_calendar(std::string_view json, CalendarMonth &month)
{
    const auto document = nlohmann::json::parse(json, nullptr, false);
    if (!document.is_object())
        return false;
    CalendarMonth parsed;
    parsed.year = document.value("year", 0);
    parsed.month = document.value("month", 0);
    parsed.today = document.value("today", 0);
    parsed.days = document.value("days", 0);
    parsed.first_weekday = document.value("firstWeekday", 0);
    parsed.loading = document.value("loading", false);
    if (const auto days = document.find("items"); days != document.end() && days->is_array())
    {
        for (const auto &source : *days)
        {
            CalendarDay day;
            day.day = source.value("day", 0);
            if (const auto items = source.find("items"); items != source.end() && items->is_array())
            {
                for (const auto &item : *items)
                {
                    CalendarEntry entry;
                    entry.id = text(item, "id");
                    entry.type = text(item, "type");
                    entry.name = text(item, "name");
                    entry.poster = text(item, "poster");
                    entry.video = text(item, "video");
                    entry.title = text(item, "title");
                    entry.season = item.value("season", 0);
                    entry.episode = item.value("episode", 0);
                    day.items.push_back(std::move(entry));
                }
            }
            if (!day.items.empty())
                parsed.items.push_back(std::move(day));
        }
    }
    month = std::move(parsed);
    return true;
}

bool parse_addons(std::string_view json, std::vector<Addon> &addons)
{
    const auto document = nlohmann::json::parse(json, nullptr, false);
    if (!document.is_array())
        return false;
    std::vector<Addon> parsed;
    for (const auto &source : document)
    {
        Addon addon;
        addon.name = text(source, "name");
        addon.version = text(source, "version");
        addon.description = text(source, "description");
        addon.logo = text(source, "logo");
        addon.host = text(source, "host");
        addon.types = texts(source, "types");
        addon.resources = texts(source, "resources");
        addon.official = source.value("official", false);
        parsed.push_back(std::move(addon));
    }
    addons = std::move(parsed);
    return true;
}
} // namespace ui
