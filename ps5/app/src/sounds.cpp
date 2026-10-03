#include "sounds.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

extern "C"
{
int sceAudioOutInit(void);
int sceAudioOutOpen(int user, int type, int index, std::uint32_t length, std::uint32_t frequency,
                    std::uint32_t format);
int sceAudioOutClose(int handle);
int sceAudioOutOutput(int handle, const void *samples);
}

namespace ps5
{
namespace
{
constexpr int kRate = 48000;
constexpr std::size_t kGrain = 256; // frames the output takes at a time
constexpr std::uint32_t kStereo16 = 1;
constexpr int kSystemUser = 0xff;
constexpr int kAlreadyStarted = static_cast<int>(0x8026000e);

// A tone gliding from one pitch to another that starts at once and dies away.
std::vector<std::int16_t> tone(float from_hz, float to_hz, float seconds, float loudness)
{
    const auto count = static_cast<std::size_t>(seconds * kRate);
    std::vector<std::int16_t> wave(count);
    float phase = 0;
    for (std::size_t index = 0; index < count; ++index)
    {
        const float along = static_cast<float>(index) / static_cast<float>(count);
        phase += 2.0f * 3.14159265f * (from_hz + (to_hz - from_hz) * along) / kRate;
        // A few milliseconds to come in (no click), then an even decay to silence.
        const float attack = std::min(1.0f, static_cast<float>(index) / (0.003f * kRate));
        const float decay = (1.0f - along) * std::exp(-4.0f * along);
        wave[index] =
            static_cast<std::int16_t>(std::sin(phase) * attack * decay * loudness * 32767.0f);
    }
    return wave;
}
} // namespace

Sounds::~Sounds()
{
    stopping_ = true;
    if (thread_.joinable())
        thread_.join();
    if (handle_ >= 0)
        sceAudioOutClose(handle_);
}

bool Sounds::start()
{
    waves_[static_cast<int>(ui::Sound::Move)] = tone(620.0f, 560.0f, 0.045f, 0.07f);
    waves_[static_cast<int>(ui::Sound::Select)] = tone(700.0f, 1050.0f, 0.110f, 0.09f);
    waves_[static_cast<int>(ui::Sound::Back)] = tone(560.0f, 400.0f, 0.100f, 0.08f);

    const int started = sceAudioOutInit();
    if (started < 0 && started != kAlreadyStarted)
        return false;
    handle_ = sceAudioOutOpen(kSystemUser, 0, 0, kGrain, kRate, kStereo16);
    if (handle_ < 0)
        return false;
    thread_ = std::thread{[this] { run(); }};
    return true;
}

void Sounds::play(ui::Sound sound)
{
    wanted_[static_cast<int>(sound)].fetch_add(1, std::memory_order_relaxed);
}

void Sounds::run()
{
    struct Voice
    {
        int kind;
        std::size_t at;
    };
    std::vector<Voice> voices;
    std::int16_t block[kGrain * 2];
    while (!stopping_)
    {
        for (int kind = 0; kind < kKinds; ++kind)
            if (wanted_[kind].exchange(0, std::memory_order_relaxed) > 0 && voices.size() < 8)
                voices.push_back({kind, 0});
        for (std::size_t frame = 0; frame < kGrain; ++frame)
        {
            int mixed = 0;
            for (Voice &voice : voices)
                if (voice.at < waves_[voice.kind].size())
                    mixed += waves_[voice.kind][voice.at++];
            const auto sample = static_cast<std::int16_t>(std::clamp(mixed, -32767, 32767));
            block[frame * 2] = block[frame * 2 + 1] = sample;
        }
        std::erase_if(voices,
                      [this](const Voice &voice) { return voice.at >= waves_[voice.kind].size(); });
        // Waits until the output has room for the block, which paces this loop.
        if (sceAudioOutOutput(handle_, block) < 0)
            break;
    }
}
} // namespace ps5
