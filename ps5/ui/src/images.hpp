// Images from the web: posters, backgrounds and title logos. Downloaded images live in a
// folder, one file per image, named by a hash of the image's address; this class asks the
// host to download the ones that are missing, turns the files into textures as they are
// needed, and lets go of textures that have not been drawn for a while.

#pragma once

#include <cstddef>
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

    enum class State
    {
        Ready,       // the texture can be drawn
        Pending,     // being downloaded, or waiting its turn to be decoded
        Unavailable, // no address, the download failed, or the file is not an image
    };

    struct Texture
    {
        State state = State::Unavailable;
        int handle = 0; // valid when `state` is Ready
        int width = 0;
        int height = 0;
    };

    // The texture for `address`, decoding it on first use. At most `kDecodesPerFrame`
    // images are decoded between calls to `begin_frame`, so a screen full of new images
    // fills in over a few frames instead of stalling one.
    Texture get(const std::string &address);
    // Starts downloading `address` if it is not in the folder, without making a texture:
    // for images that are likely to be wanted soon.
    void prefetch(const std::string &address);
    void begin_frame();

    // How the host downloads images. `fetch` is called once for each image that is wanted
    // but not in the folder, with its address and the file to save it as; `failed` says
    // whether a download that was asked for has failed.
    using Fetch = std::function<void(const std::string &address, const std::string &file)>;
    using Failed = std::function<bool(const std::string &address)>;
    void set_fetcher(Fetch fetch, Failed failed);

  private:
    static constexpr int kDecodesPerFrame = 3;
    // Textures are kept up to this many bytes; beyond it, those not drawn recently go.
    static constexpr std::size_t kTextureBudget = std::size_t{320} << 20;
    static constexpr long kKeepFrames = 120;   // never drop a texture drawn this recently
    static constexpr long kLookEveryFrames = 10; // how often to look for a pending file

    struct Entry
    {
        Texture texture;
        std::size_t bytes = 0;
        long last_used = 0;
    };
    struct Download
    {
        long look_again = 0; // the frame on which to look for the file again
    };

    bool request(const std::string &address, const std::string &file);
    void trim();

    NVGcontext *context_;
    std::string folder_;
    int decodes_left_ = kDecodesPerFrame;
    long frame_ = 0;
    std::size_t texture_bytes_ = 0;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, Download> downloads_;
    Fetch fetch_;
    Failed failed_;
};
} // namespace ui
