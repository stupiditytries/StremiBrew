// The board's rows as the core bridge delivers them (stremio_core_board_rows).

#pragma once

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
};

struct BoardRow
{
    std::string title; // "Popular - Movie"
    PosterShape shape = PosterShape::Poster;
    bool loading = false;
    std::string error;
    std::vector<BoardItem> items;
};

// Parses the bridge's JSON. Returns false and leaves `rows` empty when it is not valid.
bool parse_board(std::string_view json, std::vector<BoardRow> &rows);
} // namespace ui
