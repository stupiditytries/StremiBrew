// Finding the black bars a video carries above and below its picture (a wide film in a
// 16:9 file), so the picture can be shown without them.
//
// Each picture looked at gives a count of the rows at its top and at its bottom that are
// black all the way across. One picture proves little: a dark scene is black everywhere,
// and a trailer opens and closes on cards that fill the whole frame. So the counts from
// the last few seconds are kept, and the bars are what most of those pictures agree on.
// The answer follows the video: when a full-frame card gives way to the film, the bars
// appear a moment later, and they go again when the film gives way to a card.
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
class BarFinder
{
  public:
    void reset()
    {
        count_ = next_ = 0;
        top_ = bottom_ = 0;
        least_ = 1;
    }

    // Looks at one picture's luma plane. `bytes` is 1 or 2 a sample and `depth` the bits
    // of it in use; `limited` says black is 16 of 255 (as in most video) and not 0.
    void look(const std::uint8_t *luma, std::ptrdiff_t stride, int width, int height, int bytes,
              int depth, bool limited)
    {
        if (width < 64 || height < 64)
            return;
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
        const int reach = height * 2 / 5; // no bar is taller than this
        int top = 0, bottom = 0;
        while (top < reach && dark(top))
            ++top;
        while (bottom < reach && dark(height - 1 - bottom))
            ++bottom;
        // A dark picture says nothing, and neither does one that is black far further in
        // than any bars go (a title on black): the widest films leave under a quarter of
        // the height above and below.
        if (top > height * 6 / 25 || bottom > height * 6 / 25)
            return;
        tops_[next_] = static_cast<float>(top) / static_cast<float>(height);
        bottoms_[next_] = static_cast<float>(bottom) / static_cast<float>(height);
        next_ = (next_ + 1) % kKept;
        count_ = std::min(count_ + 1, kKept);
        decide();
    }

    // How much of the picture's height is bar at its top and at its bottom (0 to 1 each).
    void bars(float &top, float &bottom) const
    {
        top = top_;
        bottom = bottom_;
    }

  private:
    // How many pictures' counts are kept. The player shows this one picture in every two,
    // so at a film's 24 pictures a second this is about three seconds.
    static constexpr int kKept = 36;

    void decide()
    {
        if (count_ < 12)
            return;
        // Real bars are in every picture, and a picture can only look as if it has more
        // (when its own edge is dark). So the bars are what nearly all of the recent
        // pictures have at least: the count a quarter of the way up from the smallest.
        // Full-frame pictures count 0, so a card of a second or so removes the bars.
        float tops[kKept], bottoms[kKept];
        std::copy(tops_, tops_ + count_, tops);
        std::copy(bottoms_, bottoms_ + count_, bottoms);
        std::nth_element(tops, tops + count_ / 4, tops + count_);
        std::nth_element(bottoms, bottoms + count_ / 4, bottoms + count_);
        float top = tops[count_ / 4], bottom = bottoms[count_ / 4];
        // Bars are a matching pair of some size; anything else is the picture itself
        // being dark along an edge.
        const bool pair = top >= 0.025f && bottom >= 0.025f && std::abs(top - bottom) <= 0.04f;
        if (!pair)
        {
            top_ = bottom_ = 0;
            return;
        }
        // A video's bars are one size throughout, so the smallest pair found so far caps
        // what a darker stretch can make them look like.
        least_ = std::min(least_, std::min(top, bottom));
        top = std::min(top, least_ + 0.004f);
        bottom = std::min(bottom, least_ + 0.004f);
        // Small differences from one look to the next are measuring noise; the answer
        // only moves for more than that.
        if (top_ == 0 || std::abs(top - top_) > 0.012f || std::abs(bottom - bottom_) > 0.012f)
        {
            top_ = top;
            bottom_ = bottom;
        }
    }

    float tops_[kKept] = {}, bottoms_[kKept] = {};
    int count_ = 0, next_ = 0;
    float top_ = 0, bottom_ = 0;
    float least_ = 1; // the smallest bars found in this video so far
};
} // namespace ps5
