// Images from the web: posters, backgrounds and title logos. Downloaded images live in a
// folder, one file per image, named by a hash of the image's address; this class asks the
// host to download the ones that are missing, decodes the files on worker threads, turns
// the results into textures a few at a time, and lets go of textures that have not been
// drawn for a while.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
        Pending,     // being downloaded or decoded
        Unavailable, // no address, the download failed, the file is not an image, or the
                     // image has nothing visible in it
    };

    struct Texture
    {
        State state = State::Unavailable;
        int handle = 0; // valid when `state` is Ready
        int width = 0;
        int height = 0;
        // The part of the image that is not fully transparent (the whole image for one
        // without transparency). A title logo is placed by this, so the empty margin
        // many of them carry does not push the artwork out of line.
        int content_x = 0, content_y = 0, content_width = 0, content_height = 0;
    };

    // The texture for `address`. The first request starts the work; the texture is ready
    // some frames later. `max_width` is the widest the image is drawn, in screen pixels:
    // a larger image is shrunk to about that before it becomes a texture, which keeps
    // big source images (some add-ons send 800 px squares for a 200 px tile) from
    // costing upload time and memory. The first request's value is the one used.
    Texture get(const std::string &address, int max_width);
    // A softened copy of the image: shrunk small and blurred, to be drawn stretched as a
    // backdrop that text stays readable on. Kept separately from the sharp one.
    // `light` asks for a gentler blur that keeps more of the picture.
    Texture get_soft(const std::string &address, bool light = false);
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
    static constexpr int kUploadsPerFrame = 1; // decoded images turned into textures a frame
    static constexpr int kWorkers = 2;
    // Textures are kept up to this many bytes; beyond it, those not drawn recently go.
    static constexpr std::size_t kTextureBudget = std::size_t{320} << 20;
    static constexpr long kKeepFrames = 120;     // never drop a texture drawn this recently
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
    struct Job
    {
        std::string address;
        std::string file;
        int max_width = 0;
        std::string key; // what the result is filed under (differs for a softened copy)
        bool soft = false;
        bool light = false;
    };
    struct Decoded
    {
        std::string address;
        std::vector<unsigned char> pixels; // RGBA; empty when the file could not be used
        int width = 0, height = 0;
        int content_x = 0, content_y = 0, content_width = 0, content_height = 0;
    };

    bool request(const std::string &address, const std::string &file);
    Texture find(const std::string &address, const std::string &key, int max_width, bool soft,
                 bool light);
    void trim();
    void work();
    static Decoded decode(const Job &job);

    NVGcontext *context_;
    std::string folder_;
    long frame_ = 0;
    std::size_t texture_bytes_ = 0;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, Download> downloads_;
    std::unordered_set<std::string> decoding_; // handed to the workers, not back yet
    Fetch fetch_;
    Failed failed_;

    // Shared with the worker threads.
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::deque<Decoded> decoded_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};
} // namespace ui
