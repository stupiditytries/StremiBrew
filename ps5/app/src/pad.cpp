#include "pad.hpp"

#include <cstring>

extern "C"
{
    int sceUserServiceInitialize(void *parameters);
    int sceUserServiceGetInitialUser(std::int32_t *user);
    int scePadInit(void);
    int scePadOpen(std::int32_t user, std::int32_t type, std::int32_t index, void *parameters);
    int scePadReadState(int handle, void *state);
}

namespace ps5
{
namespace
{
// The controller state the console fills in is 120 bytes; only its first fields are used:
// a 32-bit button mask, then the left stick's x and y as bytes (128 is centred).
constexpr std::size_t kStateSize = 120;
constexpr std::uint32_t kUp = 0x0010, kRight = 0x0020, kDown = 0x0040, kLeft = 0x0080;
constexpr std::uint32_t kCircle = 0x2000, kCross = 0x4000;
constexpr std::uint32_t kL1 = 0x0400, kR1 = 0x0800;
constexpr std::uint32_t kTriangle = 0x1000;
constexpr std::uint32_t kOptions = 0x0008;
constexpr float kHoldTime = 0.8f; // seconds the triangle is held before it acts
constexpr std::uint32_t kDirections = kUp | kRight | kDown | kLeft;
constexpr int kStickThreshold = 70; // how far from centre the stick counts as a direction

constexpr float kRepeatDelay = 0.40f;    // before a held direction starts repeating
constexpr float kRepeatInterval = 0.11f; // between repeats
} // namespace

bool Pad::open()
{
    std::int32_t user = 0;
    sceUserServiceInitialize(nullptr); // harmless when it is already initialised
    if (sceUserServiceGetInitialUser(&user) < 0 || scePadInit() < 0)
        return false;
    handle_ = scePadOpen(user, 0, 0, nullptr);
    return handle_ >= 0;
}

void Pad::poll(float seconds, const std::function<void(ui::Button)> &press)
{
    if (handle_ < 0)
        return;
    alignas(8) unsigned char state[kStateSize + 8] = {};
    if (scePadReadState(handle_, state) < 0)
        return;
    std::uint32_t buttons = 0;
    std::memcpy(&buttons, state, sizeof buttons);
    const int stick_x = static_cast<int>(state[4]) - 128;
    const int stick_y = static_cast<int>(state[5]) - 128;

    std::uint32_t down = buttons & (kDirections | kCross | kCircle | kL1 | kR1 | kOptions);
    if (stick_x < -kStickThreshold)
        down |= kLeft;
    else if (stick_x > kStickThreshold)
        down |= kRight;
    if (stick_y < -kStickThreshold)
        down |= kUp;
    else if (stick_y > kStickThreshold)
        down |= kDown;

    const auto send = [&](std::uint32_t bits) {
        if (bits & kUp)
            press(ui::Button::Up);
        if (bits & kDown)
            press(ui::Button::Down);
        if (bits & kLeft)
            press(ui::Button::Left);
        if (bits & kRight)
            press(ui::Button::Right);
        if (bits & kCross)
            press(ui::Button::Accept);
        if (bits & kCircle)
            press(ui::Button::Back);
        if (bits & kL1)
            press(ui::Button::SkipBack);
        if (bits & kR1)
            press(ui::Button::SkipForward);
        if (bits & kOptions)
            press(ui::Button::Options);
    };

    if (buttons & kTriangle)
    {
        triangle_for_ += seconds;
        if (triangle_for_ >= kHoldTime && !triangle_sent_)
        {
            triangle_sent_ = true;
            press(ui::Button::Calibrate);
        }
    }
    else
    {
        triangle_for_ = 0;
        triangle_sent_ = false;
    }

    const std::uint32_t pressed = down & ~held_;
    send(pressed);
    if (pressed & kDirections)
    {
        // A newly pressed direction becomes the one that repeats.
        repeating_ = pressed & kDirections;
        repeat_in_ = kRepeatDelay;
    }
    else if ((down & repeating_) != repeating_)
    {
        repeating_ = 0;
    }
    else if (repeating_ != 0)
    {
        repeat_in_ -= seconds;
        if (repeat_in_ <= 0)
        {
            send(repeating_);
            repeat_in_ = kRepeatInterval;
        }
    }
    held_ = down;
}
float Pad::hold_progress() const
{
    return triangle_sent_ ? 0.0f : triangle_for_ / kHoldTime;
}
} // namespace ps5
