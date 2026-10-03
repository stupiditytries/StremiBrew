// Poster images. Downloaded images live in a folder, one file per image, named by a hash
// of the image's address; this class turns those files into textures as they are needed.

#pragma once

#include <functional>
#include <string>
#include <unordered_map>

struct NVGcontext;

namespace ui
{
// The file name an image address is cached under: 64-bit FNV-1a as 16 hex digits + ".img".
std::string image_cache_name(const std::string &address);

class Images
{
  public:
    Images(NVGcontext *context, std::string cache_folder);
    ~Images();
    Images(const Images &) = delete;
    Images &operator=(const Images &) = delete;

    struct Texture
    {
        int handle = 0; // 0: not available (not downloaded yet, or not decodable)
        int width = 0;
        int height = 0;
    };

    // The texture for `address`, decoding it on first use. At most `kDecodesPerFrame`
    // images are decoded between calls to `begin_frame`, so a screen full of new posters
    // fills in over a few frames instead of stalling one.
    Texture get(const std::string &address);
    void begin_frame();

    // Called once for each image that is wanted but not in the cache folder, with the
    // image's address and the file it should be saved as. The host starts the download;
    // the image appears on screen once the file exists.
    using Fetcher = std::function<void(const std::string &address, const std::string &file)>;
    void set_fetcher(Fetcher fetcher);

  private:
    static constexpr int kDecodesPerFrame = 3;

    NVGcontext *context_;
    std::string folder_;
    int decodes_left_ = kDecodesPerFrame;
    std::unordered_map<std::string, Texture> textures_;
    Fetcher fetcher_;
    // For images still being downloaded: the frame on which to look for the file again.
    std::unordered_map<std::string, long> look_again_;
    long frame_ = 0;
    static constexpr long kLookEveryFrames = 20;
};
} // namespace ui
