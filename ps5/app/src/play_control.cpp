#include "play_control.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include <GL/gl.h>

#include "languages.hpp"
#include "nanovg.h"
#define NANOVG_GL3 1
#include "nanovg_gl.h"
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
// The choices remembered for each title: one line a title, its fields separated by tabs.
constexpr char kChoicesFile[] = "/download0/stremio/track-choices.txt";
constexpr std::size_t kChoicesKept = 300;

std::vector<std::string> fields_of(const std::string &line)
{
    std::vector<std::string> fields;
    std::stringstream stream{line};
    for (std::string field; std::getline(stream, field, '\t');)
        fields.push_back(field);
    return fields;
}

std::string one_field(std::string text)
{
    std::replace(text.begin(), text.end(), '\t', ' ');
    std::replace(text.begin(), text.end(), '\n', ' ');
    return text;
}

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

PlayControl::Remembered PlayControl::recall(const std::string &title) const
{
    Remembered found;
    std::ifstream file{kChoicesFile};
    for (std::string line; std::getline(file, line);)
    {
        const std::vector<std::string> fields = fields_of(line);
        if (fields.size() >= 6 && fields[0] == title)
        {
            found.audio_language = fields[1];
            found.audio_detail = fields[2];
            found.subtitles = std::atoi(fields[3].c_str());
            found.subtitle_language = fields[4];
            found.delay = std::atoi(fields[5].c_str());
        }
    }
    return found;
}

// Writes the playing title's choices to the file: its line is replaced (or added at the
// end), and the oldest lines go when there are too many.
void PlayControl::remember()
{
    if (title_.empty())
        return;
    std::vector<std::string> lines;
    {
        std::ifstream file{kChoicesFile};
        for (std::string line; std::getline(file, line);)
            if (line.compare(0, title_.size() + 1, title_ + '\t') != 0)
                lines.push_back(line);
    }
    if (lines.size() >= kChoicesKept)
        lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(lines.size() - kChoicesKept + 1));
    const Remembered &now = remembered_;
    lines.push_back(title_ + '\t' + one_field(now.audio_language) + '\t' + one_field(now.audio_detail) +
                    '\t' + std::to_string(now.subtitles) + '\t' + one_field(now.subtitle_language) +
                    '\t' + std::to_string(now.delay));
    std::ofstream file{kChoicesFile, std::ios::trunc};
    for (const std::string &line : lines)
        file << line << '\n';
}

void PlayControl::start(const ui::Stream &stream, const std::string &type, const std::string &id,
                        const std::string &video)
{
    title_ = id;
    remembered_ = recall(id);
    audio_applied_ = false;
    const std::uint64_t left_at = stremio_core_resume_offset(id.c_str(), video.c_str());
    log_line("playing %s %s from %s, resuming at %.0f s", type.c_str(), video.c_str(),
             stream.addon.c_str(), left_at / 1000.0);
    const int index = stream.index;
    core_.post([index] { stremio_core_player_load(static_cast<std::uint32_t>(index)); });
    subtitle_language_ = app_.account().subtitles_language;
    // The audio language chosen for this title before, or else the preferred one.
    player_.open(stream.url, static_cast<double>(left_at) / 1000.0,
                 remembered_.audio_language.empty() ? app_.account().audio_language
                                                    : remembered_.audio_language);
    player_.set_subtitle_delay(remembered_.delay / 1000.0);

    shared_ = std::make_shared<Shared>();
    embedded_.clear();
    externals_.clear();
    tracks_known_ = externals_known_ = false;
    subtitle_selected_ = 0;
    subtitle_delay_ = remembered_.delay / 1000.0;
    // "Off" chosen before for this title stands: nothing is picked automatically.
    subtitle_chosen_ = remembered_.subtitles == 1;
    preview_index_ = -1;
    preview_asked_ = -1;
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
        const std::vector<TrackInfo> tracks = player_.audio_tracks();
        if (index >= 0 && index < static_cast<int>(tracks.size()))
        {
            remembered_.audio_language = tracks[static_cast<std::size_t>(index)].language;
            remembered_.audio_detail = tracks[static_cast<std::size_t>(index)].detail;
            remember();
        }
        rebuild_tracks();
    };
    handler.choose_subtitle = [this](int index) {
        subtitle_chosen_ = true;
        choose_subtitle(index);
        // What kind of choice it was, and its language, for the next video of the title.
        const int own = static_cast<int>(embedded_.size());
        if (index <= 0)
            remembered_.subtitles = 1;
        else if (index <= own)
        {
            remembered_.subtitles = 2;
            remembered_.subtitle_language = embedded_[static_cast<std::size_t>(index - 1)].language;
        }
        else if (index - own - 1 < static_cast<int>(externals_.size()))
        {
            remembered_.subtitles = 3;
            remembered_.subtitle_language = externals_[static_cast<std::size_t>(index - own - 1)].language;
        }
        remember();
    };
    handler.set_subtitle_delay = [this](double seconds) {
        subtitle_delay_ = seconds;
        player_.set_subtitle_delay(seconds);
        remembered_.delay = static_cast<int>(seconds * 1000.0 + (seconds < 0 ? -0.5 : 0.5));
        remember();
        rebuild_tracks();
    };
    handler.preview = [this](double seconds) { preview_asked_ = seconds; };
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
    tracks.subtitle_delay = subtitle_delay_;
    app_.set_player_tracks(std::move(tracks));
}

void PlayControl::frame(int width, int height)
{
    if (!app_.player_open() || !player_.active())
        return;
    player_.draw(width, height);
    ui::Playback status = player_.status();

    show_preview(status);

    // The tracks, once the video has opened; the preferred subtitle language is picked
    // from the video's own tracks when it has one.
    if (!tracks_known_ && player_.tracks_ready())
    {
        tracks_known_ = true;
        embedded_ = player_.subtitle_tracks();
        // The audio track chosen for this title before: the player has already gone by
        // its language; among several in that language, the same kind is picked.
        if (!remembered_.audio_language.empty())
        {
            const std::vector<TrackInfo> tracks = player_.audio_tracks();
            for (std::size_t index = 0; index < tracks.size(); ++index)
                if (ui::same_language(tracks[index].language, remembered_.audio_language) &&
                    tracks[index].detail == remembered_.audio_detail &&
                    static_cast<int>(index) != player_.audio_track())
                {
                    player_.set_audio_track(static_cast<int>(index));
                    break;
                }
        }
        // Subtitles from the video's own tracks: in the language chosen for this title
        // before (unless that choice was an add-on's), or else the preferred language.
        const std::string &language =
            remembered_.subtitles >= 2 ? remembered_.subtitle_language : subtitle_language_;
        if (!subtitle_chosen_ && !language.empty() && remembered_.subtitles != 3)
            for (std::size_t index = 0; index < embedded_.size(); ++index)
                if (ui::same_language(embedded_[index].language, language))
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
            const std::string language =
                remembered_.subtitles >= 2 ? remembered_.subtitle_language : subtitle_language_;
            std::stable_sort(externals_.begin(), externals_.end(), [&](const External &left, const External &right) {
                const bool first = ui::same_language(left.language, language);
                const bool second = ui::same_language(right.language, language);
                if (first != second)
                    return first;
                return ui::language_name(left.language) < ui::language_name(right.language);
            });
            log_line("subtitles: %zu offered by add-ons", externals_.size());
            // With nothing chosen yet, the first one in the preferred language is.
            if (!subtitle_chosen_ && !language.empty() && !externals_.empty() &&
                ui::same_language(externals_.front().language, language))
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
// While the user scrubs, the picture the player has made for the marker's moment is put
// in a texture the UI draws. The texture is 16 bits a channel: this console's driver takes
// some 40 ms over an 8-bit upload and a few over a 16-bit one.
void PlayControl::show_preview(ui::Playback &status)
{
    if (preview_asked_ < 0)
        return;
    std::vector<std::uint8_t> pixels;
    int width = 0, height = 0;
    double time = 0;
    if (!player_.preview(preview_asked_, pixels, width, height, preview_index_, time))
    {
        preview_index_ = -1;
        return;
    }
    if (!pixels.empty())
    {
        preview_wide_.resize(pixels.size());
        for (std::size_t index = 0; index < pixels.size(); ++index)
            preview_wide_[index] = static_cast<std::uint16_t>(pixels[index] * 257);
        const bool fresh = preview_texture_ == 0 || width != preview_width_ || height != preview_height_;
        if (fresh)
        {
            if (preview_image_ != 0)
                nvgDeleteImage(vg_, preview_image_); // takes the old texture with it
            glGenTextures(1, &preview_texture_);
        }
        glBindTexture(GL_TEXTURE_2D, preview_texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (fresh)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, width, height, 0, GL_RGBA, GL_UNSIGNED_SHORT,
                         preview_wide_.data());
        }
        else
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_SHORT,
                            preview_wide_.data());
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (fresh)
        {
            preview_image_ = nvglCreateImageFromHandleGL3(vg_, preview_texture_, width, height, 0);
            preview_width_ = width;
            preview_height_ = height;
        }
    }
    preview_time_ = time;
    status.preview_image = preview_image_;
    status.preview_time = preview_time_;
}
} // namespace ps5
