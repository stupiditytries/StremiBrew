// Runs the player's black-bar finder over a real video on the PC and prints what it
// reports as the video goes, the way the player would use it.
//
//   ffmpeg -v error -i <video> -f rawvideo -pix_fmt gray - | bars_test <width> <height> <fps>
//
// The pictures arrive as 8-bit luma, full range (ffmpeg's "gray"), one after another.

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
    if (argc < 4)
        return 2;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    const int width = std::atoi(argv[1]), height = std::atoi(argv[2]);
    const double rate = std::atof(argv[3]);
    std::vector<unsigned char> picture(static_cast<std::size_t>(width) * height);
    ps5::BarFinder finder;
    // As the player does: bars are assumed for a file of ordinary shape.
    finder.reset(static_cast<double>(width) / height < 1.9 ? 0.128f : 0.0f);
    float shown_top = -1, shown_bottom = -1;
    long count = 0, with_bars = 0;
    while (std::fread(picture.data(), 1, picture.size(), stdin) == picture.size())
    {
        // The player looks at every second picture it shows.
        if ((count & 1) == 0)
            finder.look(picture.data(), width, width, height, 1, 8, false);
        float top, bottom;
        finder.bars(top, bottom);
        if (top != shown_top || bottom != shown_bottom)
        {
            std::printf("  %6.1f s: bars %.1f%% top, %.1f%% bottom\n", count / rate, top * 100, bottom * 100);
            shown_top = top;
            shown_bottom = bottom;
        }
        with_bars += top > 0;
        ++count;
    }
    std::printf("  %ld pictures (%.0f s), bars reported for %.0f%% of them\n", count, count / rate,
                count > 0 ? 100.0 * with_bars / count : 0.0);
    return 0;
}
