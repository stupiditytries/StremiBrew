#include "board_data.hpp"

#include <cctype>

#include "nlohmann/json.hpp"

namespace ui
{
namespace
{
// "movie" -> "Movie", as Stremio titles its rows.
std::string capitalised(std::string text)
{
    if (!text.empty())
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

PosterShape shape_of(const std::string &name)
{
    if (name == "square")
        return PosterShape::Square;
    if (name == "landscape")
        return PosterShape::Landscape;
    return PosterShape::Poster;
}
} // namespace

bool parse_board(std::string_view json, std::vector<BoardRow> &rows)
{
    rows.clear();
    const auto document = nlohmann::json::parse(json, nullptr, false);
    if (!document.is_array())
        return false;
    for (const auto &entry : document)
    {
        BoardRow row;
        row.index = entry.value("index", rows.size());
        // A catalog row is titled with its kind ("Popular - Movie"); others just by name.
        const std::string kind = entry.value("type", std::string{});
        row.title = entry.value("name", std::string{}) + (kind.empty() ? "" : " - " + capitalised(kind));
        const std::string state = entry.value("state", std::string{"loading"});
        row.loading = state == "loading";
        if (state == "error")
            row.error = entry.value("error", std::string{"Failed to load"});
        if (const auto items = entry.find("items"); items != entry.end() && items->is_array())
        {
            for (const auto &source : *items)
            {
                BoardItem item;
                item.id = source.value("id", std::string{});
                item.type = source.value("type", std::string{});
                item.name = source.value("name", std::string{});
                const auto text = [&](const char *key) {
                    const auto found = source.find(key);
                    return found != source.end() && found->is_string()
                               ? found->get<std::string>()
                               : std::string{};
                };
                item.poster = text("poster");
                item.background = text("background");
                item.logo = text("logo");
                item.description = text("description");
                item.release_info = text("releaseInfo");
                item.runtime = text("runtime");
                item.imdb_rating = text("imdbRating");
                item.video = text("video");
                if (const auto progress = source.find("progress");
                    progress != source.end() && progress->is_number())
                    item.progress = progress->get<float>() / 1000.0f;
                if (const auto genres = source.find("genres");
                    genres != source.end() && genres->is_array())
                    for (const auto &genre : *genres)
                        if (genre.is_string())
                            item.genres.push_back(genre.get<std::string>());
                if (row.items.empty())
                    row.shape = shape_of(source.value("posterShape", std::string{"poster"}));
                row.items.push_back(std::move(item));
            }
        }
        rows.push_back(std::move(row));
    }
    return true;
}
} // namespace ui
