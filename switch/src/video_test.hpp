// A first, plain video player for the Switch: enough to prove that a stream can be read,
// decoded by the console's hardware decoder (through FFmpeg), heard and seen in time with
// each other. It plays from the start, pauses, and stops; it does not seek, switch
// tracks or show subtitles. The player proper replaces it once this much is known to
// work on the console.
//
// What it does and how well goes to the app's log: the video's format, whether the
// hardware decoder took it, and every few seconds how many pictures were decoded, shown
// and dropped and how long decoding and drawing took.

#pragma once

#include <memory>
#include <string>

#include "player_screen.hpp"

namespace nx
{
class VideoTest
{
  public:
    VideoTest();
    ~VideoTest();

    // Starts playing `url`. Returns at once; status() reports how it goes.
    void open(const std::string &url);
    // Stops, and waits for the threads to finish.
    void close();
    bool active() const
    {
        return session_ != nullptr;
    }
    void set_paused(bool paused);
    ui::Playback status() const;
    // On the drawing thread: draws the picture that is due into the area of the
    // framebuffer given (in pixels, counted from its lower left), fitted to it.
    void draw(int left, int bottom, int width, int height);

  private:
    struct Session;
    bool create_program();

    std::unique_ptr<Session> session_;
    unsigned program_ = 0, vertex_array_ = 0, planes_[3] = {};
    int plane_width_[3] = {}, plane_height_[3] = {}, plane_format_[3] = {};
    // The picture on screen: its size, how its samples are laid out, and its time.
    int width_ = 0, height_ = 0;
    bool planar_ = false;
    float scale_ = 1.0f;
    bool has_picture_ = false;
};
} // namespace nx
