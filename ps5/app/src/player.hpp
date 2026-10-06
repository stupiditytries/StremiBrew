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

#include "black_bars.hpp"
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

    struct Options
    {
        // A trailer on the home screen rather than something being watched: no scrubbing
        // pictures are made and fewer threads decode it.
        bool preview = false;
        float volume = 1.0f; // how loud the sound is played, 0 to 1
        bool paused = false; // opened and made ready, but held at its start
    };
    // Starts playing `url` from `start` seconds in, with the audio track in
    // `audio_language` when there is one. Returns at once; status() reports how it goes.
    void open(const std::string &url, double start, const std::string &audio_language,
              const Options &options);
    void open(const std::string &url, double start, const std::string &audio_language)
    {
        open(url, start, audio_language, Options{});
    }
    // Stops. The threads are wound down in the background, so this does not wait on a
    // stalled network read.
    void close();
    bool active() const
    {
        return session_ != nullptr;
    }
    // How many videos, across all players, are open or closed but still winding down
    // (each holds a dozen threads until its network reads return).
    static int sessions();
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
    // Shows subtitles this many seconds later than they are timed (earlier when negative).
    void set_subtitle_delay(double seconds);
    // The subtitle lines on hand, as timed (without the delay).
    std::vector<Cue> subtitles() const;
    // The audio track's language, as the file gives it.
    std::string audio_language() const;

    // Takes a copy of the next `seconds` of sound as it is played: mono, 16,000 samples
    // a second (what speech recognition wants). capture_state says how it is going: 0 to
    // 1 while it runs, 2 when it is complete and take_capture has it, -1 when it was cut
    // short (a pause or a seek) or there is no sound to take.
    void capture_audio(double seconds);
    float capture_state() const;
    bool take_capture(std::vector<float> &samples, double &start);

    // The small picture of the video nearest `seconds`, of those made so far (they are
    // made in the background from when the video opens). `index` says which picture the
    // caller already has: `pixels` (RGBA, top row first) is only filled when the picture
    // is a different one, and `index` is then updated. `time` is the moment it shows.
    bool preview(double seconds, std::vector<std::uint8_t> &pixels, int &width, int &height,
                 int &index, double &time);

    // The picture's shape (width over height).
    float picture_aspect() const
    {
        return picture_aspect_;
    }
    // For a trailer (Options::preview): whether it has been decided if the video carries
    // black bars, and how much of its height they are at the top and bottom (0 to 1 each).
    // The decision is made once, from pictures across the video, soon after it opens.
    bool bars_decided(float &top, float &bottom) const;

    // On the drawing thread, with the framebuffer to draw into bound: takes the picture
    // that is due and draws it, fitted to `width` x `height` pixels.
    void draw(int width, int height);

  private:
    struct Session;
    bool create_program();
    bool describe(const void *frame);
    void upload(const void *frame);

    std::shared_ptr<Session> session_;
    unsigned program_ = 0, vertex_array_ = 0, sheet_ = 0;
    // The pictures' layout: pixel format, size and bytes per sample; and the sheet their
    // planes are packed into, with each plane's area of it (left, top, width, height).
    int format_ = -1, width_ = 0, height_ = 0, bytes_ = 1;
    int sheet_width_ = 0, sheet_height_ = 0;
    int area_[3][4] = {};
    std::vector<std::uint16_t> packed_;
    bool sheet_sized_ = false;

    bool has_picture_ = false;
    int shown_serial_ = -1;
    float picture_aspect_ = 16.0f / 9.0f;
    unsigned long skipped_ = 0, shown_ = 0;
    double reported_ = 0, longest_ = 0;
};
} // namespace ps5
