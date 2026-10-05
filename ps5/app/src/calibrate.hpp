// Timing subtitles to the dialogue. A stretch of the video's sound is put through speech
// recognition (whisper.cpp), which says what was spoken and when each word began; those
// words are found in the subtitles, and the difference between when a word was spoken and
// when its subtitle is timed is the delay the subtitles need.

#pragma once

#include <string>
#include <vector>

#include "player.hpp"

namespace ps5
{
struct CalibrationResult
{
    bool found = false;
    double delay = 0;    // seconds to show the subtitles later (earlier when negative)
    int matches = 0;     // words that agreed on it
    std::string message; // why not, when not found
};

// `samples` is mono sound at 16,000 samples a second that began `start` seconds into the
// video; `cues` are the subtitles as timed. `model` is a whisper.cpp model file and
// `language` the audio's language when known (two letters), empty to let the model tell.
// Takes seconds; call it off the drawing thread.
CalibrationResult calibrate(const std::string &model, const std::vector<float> &samples, double start,
                            const std::vector<Cue> &cues, const std::string &language);
// What was said in `samples` (mono, 16,000 samples a second), as plain text; empty when
// nothing was, or the model could not be loaded. `language` as for calibrate. Takes a
// second or two; call it off the drawing thread.
std::string transcribe(const std::string &model, const std::vector<float> &samples,
                       const std::string &language);
} // namespace ps5
