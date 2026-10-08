// Timing subtitles to the dialogue (see the PS5 app's calibrate.hpp) takes speech
// recognition, which this console's processor is too slow for. The play control is the
// PS5's, which expects it; here it is told there is none. (Nothing in the UI offers it:
// see ui::App::Features.)

#pragma once

#include <string>
#include <vector>

#include "player.hpp"

namespace nx
{
struct CalibrationResult
{
    bool found = false;
    double delay = 0;
    int matches = 0;
    std::string message;
};

inline CalibrationResult calibrate(const std::string &, const std::vector<float> &, double,
                                   const std::vector<Cue> &, const std::string &)
{
    CalibrationResult result;
    result.message = "Auto-calibrate is not available on this console";
    return result;
}
} // namespace nx
