#include "voice_search.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "calibrate.hpp"

extern "C"
{
int sceKernelLoadStartModule(const char *path, std::size_t args, const void *argp, std::uint32_t flags,
                             void *option, int *result);
int sceKernelDlsym(int handle, const char *symbol, void **address);
const char *sceKernelGetFsSandboxRandomWord(void);
int sceUserServiceGetInitialUser(std::int32_t *user);
}

void log_line(const char *format, ...);

namespace ps5
{
namespace
{
// Listening stops after this long whatever is being said, and sooner once whoever is
// speaking has stopped for a moment.
constexpr double kLongest = 7.0;
constexpr double kQuietToStop = 1.1;
constexpr double kShortest = 1.5;

// The console's microphone library is not one an app is started with; it is loaded when
// first wanted, and its functions looked up by name.
using OpenFunction = int (*)(std::int32_t user, std::uint32_t type, std::uint32_t index, std::uint32_t length,
                             std::uint32_t frequency, std::uint32_t format);
using InputFunction = int (*)(int handle, void *samples);
using CloseFunction = int (*)(int handle);

struct Microphone
{
    OpenFunction open = nullptr;
    InputFunction input = nullptr;
    CloseFunction close = nullptr;
    bool looked = false;
};

Microphone &microphone()
{
    static Microphone found;
    if (found.looked)
        return found;
    found.looked = true;
    char path[128];
    std::snprintf(path, sizeof path, "/%s/common/lib/libSceAudioIn.sprx", sceKernelGetFsSandboxRandomWord());
    const int handle = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
    if (handle < 0)
    {
        log_line("voice: the microphone library did not load (0x%x)", handle);
        return found;
    }
    void *open = nullptr, *input = nullptr, *close = nullptr;
    const int opened = sceKernelDlsym(handle, "sceAudioInOpen", &open);
    const int read = sceKernelDlsym(handle, "sceAudioInInput", &input);
    const int closed = sceKernelDlsym(handle, "sceAudioInClose", &close);
    if (open == nullptr || input == nullptr || close == nullptr)
    {
        log_line("voice: the microphone library loaded (handle %d) but its functions were not found "
                 "(open 0x%x, input 0x%x, close 0x%x)",
                 handle, opened, read, closed);
        return found;
    }
    found.open = reinterpret_cast<OpenFunction>(open);
    found.input = reinterpret_cast<InputFunction>(input);
    found.close = reinterpret_cast<CloseFunction>(close);
    return found;
}

// What the model wrote, as words to search for: without the notes it makes of noises
// ("[music]", "(laughs)"), quotation marks or the full stop it likes to end on.
std::string as_query(const std::string &said)
{
    std::string query;
    char closing = 0;
    for (const char letter : said)
    {
        if (closing != 0)
            closing = letter == closing ? 0 : closing;
        else if (letter == '[')
            closing = ']';
        else if (letter == '(')
            closing = ')';
        else if (letter != '"' && letter != '\n')
            query += letter;
    }
    while (!query.empty() && (query.back() == ' ' || query.back() == '.' || query.back() == '!' || query.back() == '?'))
        query.pop_back();
    query.erase(0, std::min(query.find_first_not_of(' '), query.size()));
    return query;
}
} // namespace

struct VoiceSearch::Shared
{
    std::mutex mutex;
    bool busy = false;
    bool working = false;  // listening is over; the words are being worked out
    bool finished = false; // `query` or `failure` is ready
    std::string query, failure;
};

VoiceSearch::VoiceSearch(ui::App &app, std::function<std::string()> model)
    : app_{app}, model_{std::move(model)}, shared_{std::make_shared<Shared>()}
{
    // Offered only when the console lets the app use the microphone.
    if (microphone().open != nullptr)
        app_.set_voice_handler([this] { start(); });
}

void VoiceSearch::start()
{
    {
        std::lock_guard lock{shared_->mutex};
        if (shared_->busy)
            return;
    }
    const std::string model = model_ ? model_() : std::string{};
    if (model.empty())
    {
        app_.set_voice(ui::VoicePhase::Failed, "Download a speech model in Settings to search by voice");
        return;
    }
    {
        std::lock_guard lock{shared_->mutex};
        shared_->busy = true;
        shared_->working = shared_->finished = false;
        shared_->query.clear();
        shared_->failure.clear();
    }
    app_.set_voice(ui::VoicePhase::Listening, "Listening\xE2\x80\xA6 say what to search for");
    std::thread{[shared = shared_, model] {
        const auto fail = [&](const char *why) {
            std::lock_guard lock{shared->mutex};
            shared->failure = why;
            shared->finished = true;
        };
        Microphone &mic = microphone();
        if (mic.open == nullptr)
            return fail("The microphone could not be used");
        std::int32_t user = 0;
        sceUserServiceGetInitialUser(&user);
        // 256 samples at a time, one channel of 16-bit samples at 48,000 a second.
        constexpr std::uint32_t kGrain = 256, kRate = 48000;
        int handle = mic.open(user, 1, 0, kGrain, kRate, 0);
        if (handle < 0)
        {
            log_line("voice: the microphone did not open (0x%x)", handle);
            return fail("The microphone could not be opened. Is the controller's mic muted?");
        }
        std::vector<float> samples; // 16,000 a second
        std::int16_t block[kGrain];
        float sum = 0;
        int count = 0;
        double heard_at = -1, loudest = 0;
        for (;;)
        {
            if (mic.input(handle, block) < 0)
                break;
            double energy = 0;
            for (const std::int16_t sample : block)
            {
                energy += static_cast<double>(sample) * sample;
                sum += sample;
                if (++count == 3)
                {
                    samples.push_back(sum / (3.0f * 32768.0f));
                    sum = 0;
                    count = 0;
                }
            }
            const double level = std::sqrt(energy / kGrain);
            loudest = std::max(loudest, level);
            const double now = static_cast<double>(samples.size()) / 16000.0;
            // Speech is anything well above a quiet room; listening ends a moment after
            // the last of it, or at the limit.
            if (level > 900.0)
                heard_at = now;
            if (now >= kLongest || (heard_at >= 0 && now >= kShortest && now - heard_at >= kQuietToStop))
                break;
        }
        mic.close(handle);
        log_line("voice: heard %.1f s, loudest level %.0f", static_cast<double>(samples.size()) / 16000.0, loudest);
        if (heard_at < 0)
            return fail("Nothing was heard. Is the controller's mic muted?");
        {
            std::lock_guard lock{shared->mutex};
            shared->working = true;
        }
        const std::string query = as_query(transcribe(model, samples, {}));
        log_line("voice: \"%s\"", query.c_str());
        if (query.empty())
            return fail("No words were made out. Try again.");
        std::lock_guard lock{shared->mutex};
        shared->query = query;
        shared->finished = true;
    }}.detach();
}

void VoiceSearch::frame()
{
    bool working = false, finished = false;
    std::string query, failure;
    {
        std::lock_guard lock{shared_->mutex};
        if (!shared_->busy)
            return;
        working = shared_->working;
        finished = shared_->finished;
        if (finished)
        {
            query = shared_->query;
            failure = shared_->failure;
            shared_->busy = false;
        }
        else if (working)
        {
            shared_->working = false; // said once
        }
    }
    if (finished && !query.empty())
    {
        app_.set_voice(ui::VoicePhase::Idle, {});
        app_.set_search_query(query);
    }
    else if (finished)
    {
        app_.set_voice(ui::VoicePhase::Failed, failure);
    }
    else if (working)
    {
        app_.set_voice(ui::VoicePhase::Working, "Working out what you said\xE2\x80\xA6");
    }
}
} // namespace ps5
