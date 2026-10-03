// A title's details as the core bridge delivers them (stremio_core_details).

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ui
{
struct Episode
{
    std::string id;
    std::string title;
    int season = 0;
    int episode = 0;
    std::string released; // YYYY-MM-DD, or empty
    std::string thumbnail;
    std::string overview;
};

struct Stream
{
    std::string addon;
    std::string name;
    std::string description;
    std::string url;         // empty when this app cannot play the stream
    std::string unsupported; // why not, when `url` is empty
};

struct Details
{
    enum class State
    {
        Loading,
        Ready,
        Error,
    };

    State state = State::Loading;
    std::string id;
    std::string type;
    std::string name;
    std::string description;
    std::string background;
    std::string logo;
    std::string release_info;
    std::string runtime;
    std::string imdb_rating;
    std::vector<std::string> genres;
    std::vector<std::string> cast;
    std::vector<Episode> episodes;
    // The video the streams below are for (an episode's id, or the title's own for a
    // film); empty when none has been chosen.
    std::string stream_video;
    std::vector<Stream> streams;
    // Add-ons that have not answered yet for `stream_video`.
    int streams_loading = 0;
};

// Parses the bridge's JSON. Returns false and leaves `details` untouched when it is not valid.
bool parse_details(std::string_view json, Details &details);
} // namespace ui
