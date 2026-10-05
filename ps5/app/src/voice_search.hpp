// Searching by voice: a few seconds from the controller's microphone are put through the
// speech model (the one subtitle calibration uses) and what was said becomes the search.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "app.hpp"

namespace ps5
{
class VoiceSearch
{
  public:
    // `model` gives the speech model's file, or nothing when none is on the console.
    VoiceSearch(ui::App &app, std::function<std::string()> model);

    // Once a frame: hands a finished search's words to the UI.
    void frame();

  private:
    struct Shared; // what the listening thread leaves for the drawing thread

    void start();

    ui::App &app_;
    std::function<std::string()> model_;
    std::shared_ptr<Shared> shared_;
};
} // namespace ps5
