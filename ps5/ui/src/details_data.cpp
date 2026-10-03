#include "details_data.hpp"

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

int number(const nlohmann::json &object, const char *key)
{
    const auto found = object.find(key);
    return found != object.end() && found->is_number_integer() ? found->get<int>() : 0;
}

std::vector<std::string> texts(const nlohmann::json &object, const char *key)
{
    std::vector<std::string> result;
    if (const auto found = object.find(key); found != object.end() && found->is_array())
        for (const auto &entry : *found)
            if (entry.is_string())
                result.push_back(entry.get<std::string>());
    return result;
}
} // namespace

bool parse_details(std::string_view json, Details &details)
{
    const auto document = nlohmann::json::parse(json, nullptr, false);
    if (!document.is_object())
        return false;
    Details parsed;
    const std::string state = text(document, "state");
    parsed.state = state == "ready"   ? Details::State::Ready
                   : state == "error" ? Details::State::Error
                                      : Details::State::Loading;
    parsed.id = text(document, "id");
    parsed.type = text(document, "type");
    parsed.name = text(document, "name");
    parsed.description = text(document, "description");
    parsed.background = text(document, "background");
    parsed.logo = text(document, "logo");
    parsed.release_info = text(document, "releaseInfo");
    parsed.runtime = text(document, "runtime");
    parsed.imdb_rating = text(document, "imdbRating");
    parsed.genres = texts(document, "genres");
    parsed.cast = texts(document, "cast");
    parsed.stream_video = text(document, "streamVideo");

    if (const auto videos = document.find("videos"); videos != document.end() && videos->is_array())
        for (const auto &source : *videos)
        {
            Episode episode;
            episode.id = text(source, "id");
            episode.title = text(source, "title");
            episode.season = number(source, "season");
            episode.episode = number(source, "episode");
            episode.released = text(source, "released");
            episode.thumbnail = text(source, "thumbnail");
            episode.overview = text(source, "overview");
            parsed.episodes.push_back(std::move(episode));
        }

    if (const auto groups = document.find("streamGroups");
        groups != document.end() && groups->is_array())
        for (const auto &group : *groups)
        {
            if (text(group, "state") == "loading")
                ++parsed.streams_loading;
            const std::string addon = text(group, "addon");
            if (const auto streams = group.find("streams");
                streams != group.end() && streams->is_array())
                for (const auto &source : *streams)
                {
                    Stream stream;
                    stream.addon = addon;
                    stream.name = text(source, "name");
                    stream.description = text(source, "description");
                    stream.url = text(source, "url");
                    stream.unsupported = text(source, "unsupported");
                    parsed.streams.push_back(std::move(stream));
                }
        }
    details = std::move(parsed);
    return true;
}
} // namespace ui
