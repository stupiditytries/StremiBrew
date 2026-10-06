// The calendar's month and the installed add-ons as the core bridge delivers them
// (stremio_core_calendar, stremio_core_addons).

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "board_data.hpp"

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

// Something Discover offers to browse by: a kind of title, a catalog, a genre.
struct Choice
{
    std::string name;
    bool selected = false;
};

struct DiscoverData
{
    std::vector<Choice> types, catalogs, genres; // genres is empty for a catalog without any
    bool loading = false;                        // the catalog's first page is on its way
    bool more = false;                           // it has further pages
    std::string error;
    PosterShape shape = PosterShape::Poster;
    std::vector<BoardItem> items;
};

// Each parses the bridge's JSON, returning false (and leaving its result alone) when it
// is not valid.
bool parse_calendar(std::string_view json, CalendarMonth &month);
bool parse_addons(std::string_view json, std::vector<Addon> &addons);
bool parse_discover(std::string_view json, DiscoverData &discover);
} // namespace ui
