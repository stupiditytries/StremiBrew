// The video player: reads a video from a web address, decodes it in software (FFmpeg),
// plays its sound through the console's audio output and draws its picture with OpenGL.
//
// Three threads do the work. One reads the file and sorts its packets into a video and an
// audio queue (and carries out seeks, track changes and the video's own subtitles). One
// decodes video into a short queue of pictures. One decodes audio and feeds the audio
// output; what it has fed is the clock the picture is shown against, so sound and picture
// stay together, and when the network cannot keep up both simply wait.
//
// A fourth thread, started the first time it is needed, makes the small pictures shown
// while scrubbing: it reads the same address on its own and decodes single key frames.
//
// HDR video (PQ or HLG) is tone-mapped to SDR when drawn.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "player_screen.hpp"

namespace ps5
{
// One of a video's audio or subtitle tracks.
struct TrackInfo
{
    std::string language; // as the file gives it (usually three letters); may be empty
    std::string detail;   // format and channels, and the track's own title if it has one
};

// One subtitle: when it is on screen, and what it says.
struct Cue
{
    double start = 0, end = 0;
    std::string text;
};

class Player
{
  public:
    Player();
    ~Player();

    // Starts playing `url` from `start` seconds in, with the audio track in
    // `audio_language` when there is one. Returns at once; status() reports how it goes.
    void open(const std::string &url, double start, const std::string &audio_language);
    // Stops. The threads are wound down in the background, so this does not wait on a
    // stalled network read.
    void close();
    bool active() const
    {
        return session_ != nullptr;
    }
    void set_paused(bool paused);
    void seek(double seconds);
    ui::Playback status() const;

    // The video's tracks, known once it has opened (see tracks_ready).
    bool tracks_ready() const;
    std::vector<TrackInfo> audio_tracks() const;
    int audio_track() const;
    void set_audio_track(int index);
    // The video's own text subtitles. -1 shows none of them.
    std::vector<TrackInfo> subtitle_tracks() const;
    void set_subtitle_track(int index);
    // Shows subtitles from elsewhere instead of the video's own.
    void set_external_subtitles(std::vector<Cue> cues);

    // Asks for a small picture of the video at `seconds`; take_preview hands over the
    // newest one made (RGBA, top row first) and the time that was asked for.
    void request_preview(double seconds);
    bool take_preview(std::vector<std::uint8_t> &pixels, int &width, int &height, double &seconds);

    // On the drawing thread, with the framebuffer to draw into bound: takes the picture
    // that is due and draws it, fitted to `width` x `height` pixels.
    void draw(int width, int height);

  private:
    struct Session;
    // How pictures get to the graphics card.
    enum class Route
    {
        Untested,
        Buffers,  // copied into memory the card reads directly: no driver call per picture
        Textures, // uploaded as textures, which this console's driver does slowly
    };
    bool create_programs();
    void set_colours(unsigned program, const void *frame);
    bool describe(const void *frame);
    bool upload_textures(const void *frame);
    bool create_buffers();
    void copy_to_buffers(const void *frame);
    void draw_picture(bool buffers, int width, int height);
    void show(const void *frame, int width, int height);

    std::shared_ptr<Session> session_;
    unsigned texture_program_ = 0, buffer_program_ = 0, vertex_array_ = 0;
    unsigned planes_[3] = {};
    // The pictures' layout: pixel format, size, bytes per sample and each plane's size.
    int format_ = -1, width_ = 0, height_ = 0, bytes_ = 1;
    int plane_width_[3] = {}, plane_height_[3] = {};
    bool textures_sized_ = false;
    std::vector<std::uint16_t> widened_; // an 8-bit plane as 16-bit samples, for the upload
    // The buffer route: three sets of planes used in turn, so the card is never reading
    // the one being written.
    struct Slot
    {
        unsigned buffer = 0;
        unsigned char *memory = nullptr;
        unsigned textures[3] = {};
    };
    static constexpr int kSlots = 3;
    Slot slots_[kSlots];
    std::size_t plane_offset_[3] = {};
    bool buffers_sized_ = false;
    int slot_ = 0;
    Route route_[2] = {Route::Untested, Route::Untested}; // by bytes per sample
    bool shown_from_buffers_ = false;

    bool has_picture_ = false;
    int shown_serial_ = -1;
    float picture_aspect_ = 16.0f / 9.0f;
    double slow_logged_ = 0;
    unsigned long skipped_ = 0;
};
} // namespace ps5
