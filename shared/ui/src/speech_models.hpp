// The speech models auto-calibrate can use (whisper.cpp's), and what the host reports
// about them: which are on the console, and a download in progress.

#pragma once

#include <string>

namespace ui
{
struct SpeechModel
{
    const char *name;
    const char *file; // its name at huggingface.co/ggerganov/whisper.cpp, and on the console
    const char *size;
};

inline constexpr SpeechModel kSpeechModels[] = {
    {"Tiny", "ggml-tiny.bin", "78 MB"},                 // any language; the quickest
    {"Base", "ggml-base.bin", "148 MB"},                // any language; hears words better, slower
    {"Tiny English", "ggml-tiny.en.bin", "78 MB"},      // English audio only
};
inline constexpr int kSpeechModelCount = 3;

struct SpeechModels
{
    int chosen = 0;                         // which model auto-calibrate uses
    bool ready[kSpeechModelCount] = {};     // on the console
    int downloading = -1;                   // the one being downloaded, or -1
    int progress = 0;                       // of that download, percent
    std::string error;                      // why the last download failed
};
} // namespace ui
