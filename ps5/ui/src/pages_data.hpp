// The calendar's month and the installed add-ons as the core bridge delivers them
// (stremio_core_calendar, stremio_core_addons).

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ui
{
// An episode released on a day: the series it belongs to, and the episode itself.
struct CalendarEntry
{
    std::string id, type, name, poster;
    std::string video, title;
    int season = 0, episode = 0;
};

struct CalendarDay
{
    int day = 0;
    std::vector<CalendarEntry> items;
};

struct CalendarMonth
{
    int year = 0, month = 0; // month 1 to 12; year 0 until a month has been loaded
    int today = 0;           // the day of the month, or 0 when this is not the present month
    int days = 0;
    int first_weekday = 0;   // of the month's first day: 0 is Monday
    bool loading = false;
    std::vector<CalendarDay> items; // only the days that have something
};

struct Addon
{
    std::string name, version, description, logo, host;
    std::vector<std::string> types, resources;
    bool official = false;
};

// Each parses the bridge's JSON, returning false (and leaving its result alone) when it
// is not valid.
bool parse_calendar(std::string_view json, CalendarMonth &month);
bool parse_addons(std::string_view json, std::vector<Addon> &addons);
} // namespace ui
