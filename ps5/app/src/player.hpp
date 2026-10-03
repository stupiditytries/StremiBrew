// The video player: reads a video from a web address, decodes it in software (FFmpeg),
// plays its sound through the console's audio output and draws its picture with OpenGL.
//
// Three threads do the work. One reads the file and sorts its packets into a video and an
// audio queue (and carries out seeks). One decodes video into a short queue of pictures.
// One decodes audio and feeds the audio output; what it has fed is the clock the picture
// is shown against, so sound and picture stay together, and when the network cannot keep
// up both simply wait.
//
// HDR video (PQ or HLG) is tone-mapped to SDR when drawn.

#pragma once

#include <memory>
#include <string>

#include "player_screen.hpp"

namespace ps5
{
class Player
{
  public:
    Player();
    ~Player();

    // Starts playing `url`. Returns at once; status() reports how it is going.
    void open(const std::string &url);
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
    // On the drawing thread, with the framebuffer to draw into bound: takes the picture
    // that is due and draws it, fitted to `width` x `height` pixels.
    void draw(int width, int height);

  private:
    struct Session;
    bool create_program();
    bool upload(const void *frame);

    std::shared_ptr<Session> session_;
    unsigned program_ = 0, vertex_array_ = 0;
    unsigned planes_[3] = {};
    int plane_format_ = -1, plane_width_ = 0, plane_height_ = 0;
    bool has_picture_ = false;
    int shown_serial_ = -1;
    float picture_aspect_ = 16.0f / 9.0f;
    double slow_logged_ = 0;
    unsigned long skipped_ = 0;
};
} // namespace ps5
