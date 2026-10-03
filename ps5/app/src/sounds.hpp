// The UI's sound effects: short, quiet tones made at start-up and mixed on a thread of
// their own that feeds the console's audio output.

#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "app.hpp"

namespace ps5
{
class Sounds
{
  public:
    ~Sounds();
    // Opens the audio output and starts the thread. False when the console refuses.
    bool start();
    // Plays a sound, from any thread.
    void play(ui::Sound sound);

  private:
    static constexpr int kKinds = 3;
    void run();

    std::vector<std::int16_t> waves_[kKinds]; // mono samples
    std::atomic<int> wanted_[kKinds] = {};
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    int handle_ = -1;
};
} // namespace ps5
