// Deciding whether a video carries black bars above and below its picture (a wide film in
// a 16:9 file), and how big, so the picture can be shown without them.
//
// The decision is made once, before the video is shown, from a handful of pictures taken
// from across it. One picture proves little: a dark scene is black everywhere, and a
// trailer opens and closes on cards that fill the whole frame. Each picture gives a count
// of the rows at its top and at its bottom that are black all the way across; the video
// has bars when most of the pictures that show anything have a matching pair of them.
//
// Nothing here depends on the player, so the same code is run on the PC against real
// trailers (see tools/bars_test.cpp).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace ps5
{
// What one picture shows: how much of its height is black at the top and at the bottom
// (0 to 1 each), or that it is too dark to tell anything.
struct BarSample
{
    bool dark = true;
    float top = 0, bottom = 0;
};

// Looks at one picture's luma plane. `bytes` is 1 or 2 a sample and `depth` the bits of
// it in use; `limited` says black is 16 of 255 (as in most video) and not 0.
inline BarSample measure_bars(const std::uint8_t *luma, std::ptrdiff_t stride, int width, int height,
                              int bytes, int depth, bool limited)
{
    BarSample sample;
    if (width < 64 || height < 64)
        return sample;
    // A little above black still counts as black: encoders leave noise in the bars.
    const int black = ((limited ? 16 : 0) + 20) << (depth - 8);
    const int from = width / 12, to = width - width / 12;
    const auto dark = [&](int y) {
        const std::uint8_t *row = luma + static_cast<std::ptrdiff_t>(y) * stride;
        int bright = 0;
        // Every fourth sample across, away from the very edges. A handful of bright
        // samples is allowed for: a logo or a line of small print sitting in a bar.
        for (int x = from; x < to; x += 4)
        {
            int value;
            if (bytes == 2)
            {
                std::uint16_t wide;
                std::memcpy(&wide, row + x * 2, 2);
                value = wide;
            }
            else
            {
                value = row[x];
            }
            if (value > black && ++bright > (to - from) / 4 / 50)
                return false;
        }
        return true;
    };
    const int reach = height * 2 / 5;
    int top = 0, bottom = 0;
    while (top < reach && dark(top))
        ++top;
    while (bottom < reach && dark(height - 1 - bottom))
        ++bottom;
    // Black far further in than any bars go is a dark picture, or a title on black: the
    // widest films leave under a quarter of the height above and below.
    if (top > height * 6 / 25 || bottom > height * 6 / 25)
        return sample;
    sample.dark = false;
    sample.top = static_cast<float>(top) / static_cast<float>(height);
    sample.bottom = static_cast<float>(bottom) / static_cast<float>(height);
    return sample;
}

// The bars a video has, from pictures taken across it: `top` and `bottom` as shares of the
// height, both 0 when it has none (or nothing could be told).
inline void decide_bars(const BarSample *samples, int count, float &top, float &bottom)
{
    top = bottom = 0;
    constexpr int kMost = 32;
    float tops[kMost], bottoms[kMost];
    int telling = 0, barred = 0;
    for (int index = 0; index < count && barred < kMost; ++index)
    {
        const BarSample &sample = samples[index];
        if (sample.dark)
            continue;
        ++telling;
        // Bars are a matching pair of some size; anything else is a picture that fills
        // its frame (or is merely dark along one edge).
        if (sample.top >= 0.025f && sample.bottom >= 0.025f && std::abs(sample.top - sample.bottom) <= 0.04f)
        {
            tops[barred] = sample.top;
            bottoms[barred] = sample.bottom;
            ++barred;
        }
    }
    if (barred == 0 || barred * 2 <= telling)
        return;
    // Their size is the smallest pair's: real bars are in every picture, and a picture
    // can only look as if it has more (when its own edge is dark).
    top = *std::min_element(tops, tops + barred);
    bottom = *std::min_element(bottoms, bottoms + barred);
}
} // namespace ps5
