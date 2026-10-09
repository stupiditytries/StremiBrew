// The UI's sound effects: short, quiet tones made at start-up and played through the
// console's audio renderer, which mixes them (with each other, and with whatever else
// the console is playing) by itself.

#pragma once

#include <cstddef>
#include <cstdint>

#include <switch.h>

#include "app.hpp"

namespace nx
{
class Sounds
{
  public:
    ~Sounds();
    // Makes the tones and opens the audio renderer. False when the console refuses.
    bool start();
    // Plays a sound. (On the thread that calls frame.)
    void play(ui::Sound sound);
    // Once a frame: tells the renderer what has been asked of it.
    void frame();

  private:
    static constexpr int kKinds = 3;
    // How many sounds can be heard at once (the newest takes the place of the oldest).
    static constexpr int kVoices = 4;

    AudioDriver driver_{};
    bool ready_ = false;
    void *pool_ = nullptr; // the tones' samples, in memory laid out as the renderer wants
    std::int16_t *waves_[kKinds] = {};
    int lengths_[kKinds] = {};
    AudioDriverWaveBuf playing_[kVoices] = {};
    int next_voice_ = 0;
};
} // namespace nx
