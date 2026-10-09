#include "sounds.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

void log_line(const char *format, ...);

namespace nx
{
namespace
{
constexpr int kRate = 48000;

// A tone gliding from one pitch to another that starts at once and dies away. (The same
// tones as the PS5 app's.)
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
        wave[index] = static_cast<std::int16_t>(std::sin(phase) * attack * decay * loudness * 32767.0f);
    }
    return wave;
}
} // namespace

Sounds::~Sounds()
{
    if (!ready_)
        return;
    audrvClose(&driver_);
    audrenExit();
    std::free(pool_);
}

bool Sounds::start()
{
    const std::vector<std::int16_t> tones[kKinds] = {
        tone(620.0f, 560.0f, 0.045f, 0.07f),  // Move
        tone(700.0f, 1050.0f, 0.110f, 0.09f), // Select
        tone(560.0f, 400.0f, 0.100f, 0.08f),  // Back
    };
    // The renderer plays from memory it has been given whole pages of; each tone starts
    // on a page of its own.
    const auto pages = [](std::size_t bytes) { return (bytes + 0xFFF) & ~static_cast<std::size_t>(0xFFF); };
    std::size_t pool_size = 0;
    for (const auto &wave : tones)
        pool_size += pages(wave.size() * sizeof(std::int16_t));
    pool_ = std::aligned_alloc(0x1000, pool_size);
    if (pool_ == nullptr)
        return false;
    std::memset(pool_, 0, pool_size);
    auto *at = static_cast<std::uint8_t *>(pool_);
    for (int kind = 0; kind < kKinds; ++kind)
    {
        const std::size_t bytes = tones[kind].size() * sizeof(std::int16_t);
        waves_[kind] = reinterpret_cast<std::int16_t *>(at);
        lengths_[kind] = static_cast<int>(tones[kind].size());
        std::memcpy(at, tones[kind].data(), bytes);
        at += pages(bytes);
    }
    armDCacheFlush(pool_, pool_size);

    static const AudioRendererConfig config = {
        .output_rate = AudioRendererOutputRate_48kHz,
        .num_voices = kVoices,
        .num_effects = 0,
        .num_sinks = 1,
        .num_mix_objs = 1,
        .num_mix_buffers = 2,
    };
    Result result = audrenInitialize(&config);
    if (R_FAILED(result))
    {
        log_line("sound effects: the audio renderer did not open (0x%x)", result);
        std::free(pool_);
        pool_ = nullptr;
        return false;
    }
    result = audrvCreate(&driver_, &config, 2);
    if (R_FAILED(result))
    {
        log_line("sound effects: no audio driver (0x%x)", result);
        audrenExit();
        std::free(pool_);
        pool_ = nullptr;
        return false;
    }
    const int pool = audrvMemPoolAdd(&driver_, pool_, pool_size);
    audrvMemPoolAttach(&driver_, pool);
    static const u8 kLeftRight[] = {0, 1};
    audrvDeviceSinkAdd(&driver_, AUDREN_DEFAULT_DEVICE_NAME, 2, kLeftRight);
    audrvUpdate(&driver_);
    result = audrenStartAudioRenderer();
    if (R_FAILED(result))
        log_line("sound effects: the audio renderer did not start (0x%x)", result);
    // Each voice plays one channel of sound into both of the output's.
    for (int voice = 0; voice < kVoices; ++voice)
    {
        audrvVoiceInit(&driver_, voice, 1, PcmFormat_Int16, kRate);
        audrvVoiceSetDestinationMix(&driver_, voice, AUDREN_FINAL_MIX_ID);
        audrvVoiceSetMixFactor(&driver_, voice, 1.0f, 0, 0);
        audrvVoiceSetMixFactor(&driver_, voice, 1.0f, 0, 1);
    }
    audrvUpdate(&driver_);
    ready_ = true;
    return true;
}

void Sounds::play(ui::Sound sound)
{
    const int kind = static_cast<int>(sound);
    if (!ready_ || kind < 0 || kind >= kKinds)
        return;
    const int voice = next_voice_;
    next_voice_ = (next_voice_ + 1) % kVoices;
    audrvVoiceStop(&driver_, voice);
    AudioDriverWaveBuf &buffer = playing_[voice];
    buffer = AudioDriverWaveBuf{};
    buffer.data_pcm16 = waves_[kind];
    buffer.size = static_cast<u64>(lengths_[kind]) * sizeof(std::int16_t);
    buffer.start_sample_offset = 0;
    buffer.end_sample_offset = lengths_[kind];
    audrvVoiceAddWaveBuf(&driver_, voice, &buffer);
    audrvVoiceStart(&driver_, voice);
}

void Sounds::frame()
{
    if (ready_)
        audrvUpdate(&driver_);
}
} // namespace nx
