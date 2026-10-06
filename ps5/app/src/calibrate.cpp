#include "calibrate.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>

#include "whisper.h"

void log_line(const char *format, ...);

namespace ps5
{
namespace
{
// How far out of time the subtitles may be and still be found.
constexpr double kReach = 45.0;
// Delays are gathered into bins this wide to find the one most words agree on.
constexpr double kBin = 0.2;

double now_seconds()
{
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) * 1e-9;
}

struct Word
{
    std::string text; // letters and digits only, lower case
    double time;      // when it begins, in seconds into the video
    bool leads;       // a subtitle's first word, whose time is exact
};

// The words of `text`, each with a time: `start` for the first, spread evenly to `end`.
void add_words(const std::string &text, double start, double end, std::vector<Word> &words)
{
    std::vector<std::string> found;
    std::string word;
    const auto flush = [&] {
        if (!word.empty())
            found.push_back(word);
        word.clear();
    };
    for (const char letter : text)
    {
        const auto byte = static_cast<unsigned char>(letter);
        if (byte >= 0x80 || (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z'))
            word += letter;
        else if (byte >= 'A' && byte <= 'Z')
            word += static_cast<char>(byte + 32);
        else if (byte == '\'')
            continue; // "don't" and "dont" are the same word
        else
            flush();
    }
    flush();
    for (std::size_t index = 0; index < found.size(); ++index)
        words.push_back({found[index],
                         start + (end - start) * static_cast<double>(index) / static_cast<double>(found.size()),
                         index == 0});
}

// The model stays loaded between calibrations; loading it takes longer than using it.
std::mutex engine_mutex;
whisper_context *engine = nullptr;
std::string engine_model;

void quiet(ggml_log_level, const char *, void *)
{
}
} // namespace

CalibrationResult calibrate(const std::string &model, const std::vector<float> &samples, double start,
                            const std::vector<Cue> &cues, const std::string &language)
{
    CalibrationResult result;
    std::lock_guard lock{engine_mutex};
    const double began = now_seconds();
    if (engine == nullptr || engine_model != model)
    {
        if (engine != nullptr)
            whisper_free(engine);
        whisper_log_set(quiet, nullptr);
        whisper_context_params settings = whisper_context_default_params();
        settings.use_gpu = false;
        engine = whisper_init_from_file_with_params(model.c_str(), settings);
        engine_model = model;
        if (engine == nullptr)
        {
            engine_model.clear();
            result.message = "The speech model could not be loaded. Download it again in Settings.";
            return result;
        }
    }
    const double loaded = now_seconds();

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = 6;
    params.no_context = true;
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.print_special = false;
    params.token_timestamps = true; // when each word began, not only each sentence
    params.suppress_nst = true;
    params.language = whisper_is_multilingual(engine) ? (language.empty() ? "auto" : language.c_str()) : "en";
    if (whisper_full(engine, params, samples.data(), static_cast<int>(samples.size())) != 0)
    {
        result.message = "The dialogue could not be worked out.";
        return result;
    }

    // What was heard, word by word. A word is one or more of the model's tokens; a token
    // that begins with a space begins a word.
    std::vector<Word> heard;
    std::string said;
    const whisper_token last_text = whisper_token_eot(engine);
    for (int segment = 0; segment < whisper_full_n_segments(engine); ++segment)
    {
        std::string word;
        double word_time = 0;
        const auto flush = [&] {
            if (!word.empty())
                add_words(word, word_time, word_time, heard);
            word.clear();
        };
        for (int token = 0; token < whisper_full_n_tokens(engine, segment); ++token)
        {
            const whisper_token_data data = whisper_full_get_token_data(engine, segment, token);
            if (data.id >= last_text)
                continue; // a marker, not text
            const char *text = whisper_full_get_token_text(engine, segment, token);
            if (text == nullptr || *text == 0)
                continue;
            if (*text == ' ' || word.empty())
            {
                flush();
                word_time = start + static_cast<double>(data.t0) / 100.0;
            }
            word += text;
        }
        flush();
        said += whisper_full_get_segment_text(engine, segment);
    }
    const double recognised = now_seconds();

    // The subtitles within reach of the stretch that was heard, word by word.
    const double length = static_cast<double>(samples.size()) / 16000.0;
    std::vector<Word> written;
    for (const Cue &cue : cues)
        if (cue.end > start - kReach && cue.start < start + length + kReach)
            add_words(cue.text, cue.start, cue.end, written);

    // Every heard word that also appears in the subtitles suggests a delay: when it was
    // heard less when it is timed. The wrong pairings scatter; the right ones pile up on
    // the true delay. Words of a letter or two pair with too much to count.
    std::map<long, int> votes;
    std::vector<std::pair<double, bool>> suggestions;
    for (const Word &spoken : heard)
    {
        if (spoken.text.size() < 3)
            continue;
        for (const Word &line : written)
            if (line.text == spoken.text)
            {
                const double delay = spoken.time - line.time;
                if (std::abs(delay) > kReach)
                    continue;
                ++votes[std::lround(delay / kBin)];
                suggestions.push_back({delay, line.leads});
            }
    }
    long best = 0;
    int best_votes = 0;
    for (const auto &[bin, count] : votes)
    {
        // A pile straddles neighbouring bins.
        const auto next = votes.find(bin + 1);
        const int both = count + (next != votes.end() ? next->second : 0);
        if (both > best_votes)
        {
            best_votes = both;
            best = bin;
        }
    }
    log_line("calibrate: model %.1f s, recognition %.1f s; heard %zu words (\"%.80s\"), %zu subtitle words in reach, best delay has %d votes",
             loaded - began, recognised - loaded, heard.size(), said.c_str(), written.size(), best_votes);
    if (heard.size() < 3)
    {
        result.message = "No dialogue was heard. Try again while someone is speaking.";
        return result;
    }
    if (best_votes < 3)
    {
        result.message = "The dialogue did not match these subtitles.";
        return result;
    }
    // Within the pile, the suggestions from a subtitle's first word are the exact ones
    // (the others' times are estimates), so they decide when there are enough of them.
    const double centre = (static_cast<double>(best) + 0.5) * kBin;
    std::vector<double> exact, all;
    for (const auto &[delay, leads] : suggestions)
        if (std::abs(delay - centre) <= 0.6)
        {
            all.push_back(delay);
            if (leads)
                exact.push_back(delay);
        }
    std::vector<double> &chosen = exact.size() >= 2 ? exact : all;
    std::sort(chosen.begin(), chosen.end());
    result.found = true;
    result.matches = best_votes;
    result.delay = chosen[chosen.size() / 2];
    return result;
}
} // namespace ps5
