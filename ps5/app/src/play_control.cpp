#include "play_control.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <thread>

#include "languages.hpp"
#include "nanovg.h"
#include "nlohmann/json.hpp"

extern "C"
{
std::int32_t stremio_core_player_load(std::uint32_t index);
void stremio_core_player_time(std::uint64_t time, std::uint64_t duration, bool seek);
void stremio_core_player_paused(bool paused);
void stremio_core_player_ended(void);
void stremio_core_player_unload(void);
std::uint64_t stremio_core_resume_offset(const char *title, const char *video);
std::size_t stremio_core_subtitles(const char *kind, const char *id, char *out, std::size_t capacity);
std::int64_t stremio_http_get(const char *url, std::uint8_t *out, std::size_t capacity);
}

void log_line(const char *format, ...);

namespace ps5
{
namespace
{
double now_seconds()
{
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) * 1e-9;
}

// Text that is not UTF-8 is taken to be Latin-1, the usual other encoding of subtitles.
std::string as_utf8(const std::string &text)
{
    bool valid = true;
    for (std::size_t index = 0; index < text.size() && valid;)
    {
        const auto lead = static_cast<unsigned char>(text[index]);
        const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        valid = length != 0 && index + length <= text.size();
        for (std::size_t tail = 1; valid && tail < length; ++tail)
            valid = (static_cast<unsigned char>(text[index + tail]) & 0xC0) == 0x80;
        index += length;
    }
    if (valid)
        return text;
    std::string converted;
    for (const char letter : text)
    {
        const auto byte = static_cast<unsigned char>(letter);
        if (byte < 0x80)
            converted += letter;
        else
        {
            converted += static_cast<char>(0xC0 | (byte >> 6));
            converted += static_cast<char>(0x80 | (byte & 0x3F));
        }
    }
    return converted;
}

// "01:02:03,456" or "02:03.456" as seconds; negative when it is not a time.
double parse_time(const std::string &text)
{
    double parts[3] = {};
    int count = 0;
    std::size_t at = 0;
    while (at < text.size() && count < 3)
    {
        char *end = nullptr;
        std::string piece = text.substr(at, text.find(':', at) - at);
        std::replace(piece.begin(), piece.end(), ',', '.');
        parts[count] = std::strtod(piece.c_str(), &end);
        if (end == piece.c_str())
            return -1;
        ++count;
        const std::size_t colon = text.find(':', at);
        if (colon == std::string::npos)
            break;
        at = colon + 1;
    }
    return count == 3 ? parts[0] * 3600 + parts[1] * 60 + parts[2] : count == 2 ? parts[0] * 60 + parts[1] : -1;
}

// Reads a subtitle file (SubRip or WebVTT): blocks of a time line "start --> end"
// followed by the text, separated by blank lines. Styling tags are dropped.
std::vector<Cue> parse_subtitles(const std::string &raw)
{
    std::string text = as_utf8(raw);
    if (text.rfind("\xEF\xBB\xBF", 0) == 0)
        text.erase(0, 3);
    std::vector<std::string> lines;
    for (std::size_t at = 0; at <= text.size();)
    {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        std::string line = text.substr(at, end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        lines.push_back(std::move(line));
        at = end + 1;
    }
    std::vector<Cue> cues;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const std::size_t arrow = lines[index].find("-->");
        if (arrow == std::string::npos)
            continue;
        const auto trimmed = [](std::string piece) {
            const std::size_t first = piece.find_first_not_of(' ');
            piece.erase(0, std::min(first, piece.size()));
            // WebVTT may put position settings after the end time.
            piece.erase(std::min(piece.find(' '), piece.size()));
            return piece;
        };
        Cue cue;
        cue.start = parse_time(trimmed(lines[index].substr(0, arrow)));
        cue.end = parse_time(trimmed(lines[index].substr(arrow + 3)));
        if (cue.start < 0 || cue.end <= cue.start)
            continue;
        for (++index; index < lines.size() && !lines[index].empty(); ++index)
        {
            // Without the <i>...</i> and {\an8} kinds of styling.
            std::string plain;
            char closing = 0;
            for (const char letter : lines[index])
            {
                if (closing != 0)
                    closing = letter == closing ? 0 : closing;
                else if (letter == '<')
                    closing = '>';
                else if (letter == '{')
                    closing = '}';
                else
                    plain += letter;
            }
            if (!plain.empty())
                cue.text += (cue.text.empty() ? "" : "\n") + plain;
        }
        if (!cue.text.empty())
            cues.push_back(std::move(cue));
    }
    return cues;
}
} // namespace

struct PlayControl::Shared
{
    std::mutex mutex;
    bool listed = false; // the add-ons have answered
    std::vector<External> list;
    int wanted = 0; // the latest subtitle file asked for; older answers are dropped
    bool loaded = false;
    std::vector<Cue> cues;
};

PlayControl::PlayControl(ui::App &app, CoreLink &core, NVGcontext *vg) : app_{app}, core_{core}, vg_{vg}
{
}

void PlayControl::start(const ui::Stream &stream, const std::string &type, const std::string &id,
                        const std::string &video)
{
    const std::uint64_t left_at = stremio_core_resume_offset(id.c_str(), video.c_str());
    log_line("playing %s %s from %s, resuming at %.0f s", type.c_str(), video.c_str(),
             stream.addon.c_str(), left_at / 1000.0);
    const int index = stream.index;
    core_.post([index] { stremio_core_player_load(static_cast<std::uint32_t>(index)); });
    subtitle_language_ = app_.account().subtitles_language;
    player_.open(stream.url, static_cast<double>(left_at) / 1000.0, app_.account().audio_language);

    shared_ = std::make_shared<Shared>();
    embedded_.clear();
    externals_.clear();
    tracks_known_ = externals_known_ = false;
    subtitle_selected_ = 0;
    subtitle_chosen_ = false;
    preview_time_ = -1e9;
    reported_start_ = reported_paused_ = reported_end_ = false;
    reported_at_ = 0;

    // The add-ons are asked for subtitles while the video opens.
    std::thread{[shared = shared_, type, video] {
        std::vector<char> buffer(std::size_t{2} << 20);
        const std::size_t length = stremio_core_subtitles(type.c_str(), video.c_str(), buffer.data(), buffer.size());
        std::vector<External> list;
        if (length > 0 && length < buffer.size())
        {
            const auto document = nlohmann::json::parse(buffer.data(), buffer.data() + length, nullptr, false);
            if (document.is_array())
                for (const auto &entry : document)
                    list.push_back({entry.value("addon", std::string{}), entry.value("lang", std::string{}),
                                    entry.value("url", std::string{})});
        }
        std::lock_guard lock{shared->mutex};
        shared->list = std::move(list);
        shared->listed = true;
    }}.detach();
}

ui::PlayerHandler PlayControl::handler()
{
    ui::PlayerHandler handler;
    handler.set_paused = [this](bool paused) { player_.set_paused(paused); };
    handler.seek = [this](double seconds) {
        player_.seek(seconds);
        const ui::Playback now = player_.status();
        const auto time = static_cast<std::uint64_t>(seconds * 1000), duration = static_cast<std::uint64_t>(now.duration * 1000);
        core_.post([time, duration] { stremio_core_player_time(time, duration, true); });
    };
    handler.close = [this] { close(); };
    handler.choose_audio = [this](int index) {
        player_.set_audio_track(index);
        rebuild_tracks();
    };
    handler.choose_subtitle = [this](int index) {
        subtitle_chosen_ = true;
        choose_subtitle(index);
    };
    handler.preview = [this](double seconds) { player_.request_preview(seconds); };
    return handler;
}

void PlayControl::close()
{
    log_line("player closed");
    // The position is reported one last time so the core saves exactly where it stopped.
    const ui::Playback last = player_.status();
    if (reported_start_ && !reported_end_ && last.duration > 0)
    {
        const auto time = static_cast<std::uint64_t>(last.position * 1000), duration = static_cast<std::uint64_t>(last.duration * 1000);
        core_.post([time, duration] { stremio_core_player_time(time, duration, false); });
    }
    core_.post([] { stremio_core_player_unload(); });
    player_.close();
    shared_.reset();
}

// The subtitles list is: off, the video's own tracks, then the add-ons'.
void PlayControl::choose_subtitle(int index)
{
    subtitle_selected_ = index;
    const int own = static_cast<int>(embedded_.size());
    if (shared_ != nullptr)
    {
        std::lock_guard lock{shared_->mutex};
        ++shared_->wanted; // whatever file was on its way is no longer wanted
        shared_->loaded = false;
    }
    if (index <= 0)
    {
        player_.set_subtitle_track(-1);
    }
    else if (index <= own)
    {
        player_.set_subtitle_track(index - 1);
    }
    else if (index - own - 1 < static_cast<int>(externals_.size()) && shared_ != nullptr)
    {
        player_.set_subtitle_track(-1);
        const std::string url = externals_[static_cast<std::size_t>(index - own - 1)].url;
        int ticket;
        {
            std::lock_guard lock{shared_->mutex};
            ticket = shared_->wanted;
        }
        std::thread{[shared = shared_, url, ticket] {
            std::vector<std::uint8_t> buffer(std::size_t{4} << 20);
            const std::int64_t length = stremio_http_get(url.c_str(), buffer.data(), buffer.size());
            std::vector<Cue> cues;
            if (length > 0)
                cues = parse_subtitles(std::string{reinterpret_cast<const char *>(buffer.data()),
                                                   static_cast<std::size_t>(length)});
            log_line("subtitles: %zu lines from %s", cues.size(), url.c_str());
            std::lock_guard lock{shared->mutex};
            if (shared->wanted == ticket)
            {
                shared->cues = std::move(cues);
                shared->loaded = true;
            }
        }}.detach();
    }
    rebuild_tracks();
}

void PlayControl::rebuild_tracks()
{
    ui::PlayerTracks tracks;
    for (const TrackInfo &track : player_.audio_tracks())
        tracks.audio.push_back({ui::language_name(track.language), track.detail});
    tracks.audio_selected = player_.audio_track();
    tracks.subtitles.push_back({"Off", ""});
    for (const TrackInfo &track : embedded_)
        tracks.subtitles.push_back(
            {ui::language_name(track.language),
             track.detail.empty() ? std::string{"In the video"} : "In the video  \xC2\xB7  " + track.detail});
    std::string language;
    int number = 0;
    for (const External &external : externals_)
    {
        // Numbered within each language: an add-on usually has several versions.
        number = external.language == language ? number + 1 : 1;
        language = external.language;
        tracks.subtitles.push_back(
            {ui::language_name(external.language), external.addon + " " + std::to_string(number)});
    }
    tracks.subtitle_selected = subtitle_selected_;
    tracks.subtitles_loading = !externals_known_;
    app_.set_player_tracks(std::move(tracks));
}

void PlayControl::frame(int width, int height)
{
    if (!app_.player_open() || !player_.active())
        return;
    player_.draw(width, height);
    ui::Playback status = player_.status();

    // The scrubbing picture, as an image the UI can draw.
    std::vector<std::uint8_t> pixels;
    int picture_width = 0, picture_height = 0;
    double picture_time = 0;
    if (player_.take_preview(pixels, picture_width, picture_height, picture_time))
    {
        if (preview_image_ != 0 && picture_width == preview_width_ && picture_height == preview_height_)
        {
            nvgUpdateImage(vg_, preview_image_, pixels.data());
        }
        else
        {
            if (preview_image_ != 0)
                nvgDeleteImage(vg_, preview_image_);
            preview_image_ = nvgCreateImageRGBA(vg_, picture_width, picture_height, 0, pixels.data());
            preview_width_ = picture_width;
            preview_height_ = picture_height;
        }
        preview_time_ = picture_time;
    }
    status.preview_image = preview_image_;
    status.preview_time = preview_time_;

    // The tracks, once the video has opened; the preferred subtitle language is picked
    // from the video's own tracks when it has one.
    if (!tracks_known_ && player_.tracks_ready())
    {
        tracks_known_ = true;
        embedded_ = player_.subtitle_tracks();
        if (!subtitle_chosen_ && !subtitle_language_.empty())
            for (std::size_t index = 0; index < embedded_.size(); ++index)
                if (ui::same_language(embedded_[index].language, subtitle_language_))
                {
                    subtitle_chosen_ = true;
                    choose_subtitle(static_cast<int>(index) + 1);
                    break;
                }
        rebuild_tracks();
    }
    if (shared_ != nullptr)
    {
        bool listed = false, loaded = false;
        std::vector<Cue> cues;
        {
            std::lock_guard lock{shared_->mutex};
            if (shared_->listed && !externals_known_)
            {
                listed = true;
                externals_ = shared_->list;
            }
            if (shared_->loaded)
            {
                shared_->loaded = false;
                loaded = true;
                cues = std::move(shared_->cues);
            }
        }
        if (listed && tracks_known_)
        {
            externals_known_ = true;
            // The preferred language first, then the rest by language.
            std::stable_sort(externals_.begin(), externals_.end(), [&](const External &left, const External &right) {
                const bool first = ui::same_language(left.language, subtitle_language_);
                const bool second = ui::same_language(right.language, subtitle_language_);
                if (first != second)
                    return first;
                return ui::language_name(left.language) < ui::language_name(right.language);
            });
            log_line("subtitles: %zu offered by add-ons", externals_.size());
            // With nothing chosen yet, the first one in the preferred language is.
            if (!subtitle_chosen_ && !subtitle_language_.empty() && !externals_.empty() &&
                ui::same_language(externals_.front().language, subtitle_language_))
            {
                subtitle_chosen_ = true;
                choose_subtitle(static_cast<int>(embedded_.size()) + 1);
            }
            rebuild_tracks();
        }
        if (loaded)
            player_.set_external_subtitles(std::move(cues));
    }

    // The core is told how playback goes: that it started, each pause and resume, and
    // once a second where it is.
    const bool playing = status.state == ui::Playback::State::Playing;
    const bool paused = status.state == ui::Playback::State::Paused;
    if (playing || paused)
    {
        const double now = now_seconds();
        if (!reported_start_ || reported_paused_ != paused)
        {
            reported_start_ = true;
            reported_paused_ = paused;
            core_.post([paused] { stremio_core_player_paused(paused); });
        }
        if (playing && !status.buffering && now - reported_at_ >= 1.0 && status.duration > 0)
        {
            reported_at_ = now;
            const auto time = static_cast<std::uint64_t>(status.position * 1000), duration = static_cast<std::uint64_t>(status.duration * 1000);
            core_.post([time, duration] { stremio_core_player_time(time, duration, false); });
        }
    }
    if (status.state == ui::Playback::State::Ended && !reported_end_)
    {
        reported_end_ = true;
        const auto duration = static_cast<std::uint64_t>(status.duration * 1000);
        core_.post([duration] {
            stremio_core_player_time(duration, duration, false);
            stremio_core_player_ended();
        });
    }
    app_.set_playback(status);
}
} // namespace ps5
