// The board's rows as the core bridge delivers them (stremio_core_board_rows).

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ui
{
enum class PosterShape
{
    Poster,
    Square,
    Landscape,
};

struct BoardItem
{
    std::string id;
    std::string type;
    std::string name;
    std::string poster; // image address; empty when the add-on gave none
    // Shown in the featured area while the item has the focus. Any may be empty.
    std::string background;
    std::string logo;
    std::string description;
    std::string release_info;
    std::string runtime;
    std::string imdb_rating;
    std::vector<std::string> genres;
};

struct BoardRow
{
    // The row's position among all of the board's catalogs, including those not shown
    // (a catalog that returned nothing has no row). The host requests rows by this number.
    std::size_t index = 0;
    std::string title; // "Popular - Movie"
    PosterShape shape = PosterShape::Poster;
    bool loading = false;
    std::string error;
    std::vector<BoardItem> items;
};

// Parses the bridge's JSON. Returns false and leaves `rows` empty when it is not valid.
bool parse_board(std::string_view json, std::vector<BoardRow> &rows);
} // namespace ui
