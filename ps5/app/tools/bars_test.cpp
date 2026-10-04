// Runs the player's black-bar decision on the PC: reads pictures taken from across a real
// video and prints what each shows and what is decided, as the player would decide it.
//
//   bars_test <width> <height> < pictures
//
// The pictures arrive one after another as 8-bit luma, full range (ffmpeg's "gray").

#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "../src/black_bars.hpp"

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    const int width = std::atoi(argv[1]), height = std::atoi(argv[2]);
    std::vector<unsigned char> picture(static_cast<std::size_t>(width) * height);
    std::vector<ps5::BarSample> samples;
    while (std::fread(picture.data(), 1, picture.size(), stdin) == picture.size())
    {
        const ps5::BarSample sample = ps5::measure_bars(picture.data(), width, width, height, 1, 8, false);
        if (sample.dark)
            std::printf("  picture %zu: dark\n", samples.size() + 1);
        else
            std::printf("  picture %zu: %.1f%% top, %.1f%% bottom\n", samples.size() + 1, sample.top * 100,
                        sample.bottom * 100);
        samples.push_back(sample);
    }
    float top = 0, bottom = 0;
    ps5::decide_bars(samples.data(), static_cast<int>(samples.size()), top, bottom);
    if (top > 0)
        std::printf("  decided: bars, %.1f%% top and %.1f%% bottom\n", top * 100, bottom * 100);
    else
        std::printf("  decided: no bars\n");
    return 0;
}
