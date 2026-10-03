#include "images.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "nanovg.h"
#include "stb_image.h"
#include "webp/decode.h"

namespace ui
{
std::string image_cache_name(const std::string &address)
{
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (const unsigned char byte : address)
    {
        hash ^= byte;
        hash *= 0x100000001B3ull;
    }
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.img", static_cast<unsigned long long>(hash));
    return name;
}

namespace
{
bool file_exists(const std::string &path)
{
    if (std::FILE *file = std::fopen(path.c_str(), "rb"))
    {
        std::fclose(file);
        return true;
    }
    return false;
}

std::vector<unsigned char> read_file(const std::string &path)
{
    std::vector<unsigned char> bytes;
    if (std::FILE *file = std::fopen(path.c_str(), "rb"))
    {
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        if (size > 0)
        {
            bytes.resize(static_cast<std::size_t>(size));
            if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
                bytes.clear();
        }
        std::fclose(file);
    }
    return bytes;
}

bool is_webp(const std::vector<unsigned char> &bytes)
{
    return bytes.size() > 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
           std::memcmp(bytes.data() + 8, "WEBP", 4) == 0;
}

// A softened copy is this wide at most; stretched over the screen it is already smooth,
// and the blur below removes what detail is left.
constexpr int kSoftWidth = 480;
constexpr int kLightSoftWidth = 960;

// Blurs an RGBA image in place: a box blur run three times each way, which is close to a
// Gaussian.
void soften(std::vector<unsigned char> &pixels, int width, int height, int kRadius)
{
    std::vector<unsigned char> other(pixels.size());
    const auto pass = [&](const unsigned char *from, unsigned char *to, bool horizontal) {
        const int outer = horizontal ? height : width;
        const int inner = horizontal ? width : height;
        const int step = horizontal ? 4 : width * 4;
        for (int line = 0; line < outer; ++line)
        {
            const std::size_t start = horizontal ? static_cast<std::size_t>(line) * width * 4
                                                  : static_cast<std::size_t>(line) * 4;
            for (int channel = 0; channel < 4; ++channel)
            {
                int sum = 0;
                for (int offset = -kRadius; offset <= kRadius; ++offset)
                    sum += from[start + static_cast<std::size_t>(std::clamp(offset, 0, inner - 1)) * step + channel];
                for (int index = 0; index < inner; ++index)
                {
                    to[start + static_cast<std::size_t>(index) * step + channel] =
                        static_cast<unsigned char>(sum / (2 * kRadius + 1));
                    const int leaving = std::clamp(index - kRadius, 0, inner - 1);
                    const int entering = std::clamp(index + kRadius + 1, 0, inner - 1);
                    sum += from[start + static_cast<std::size_t>(entering) * step + channel] -
                           from[start + static_cast<std::size_t>(leaving) * step + channel];
                }
            }
        }
    };
    for (int round = 0; round < 3; ++round)
    {
        pass(pixels.data(), other.data(), true);
        pass(other.data(), pixels.data(), false);
    }
}

// An image's alpha below this counts as nothing being there.
constexpr unsigned char kVisibleAlpha = 24;
} // namespace

Images::Images(NVGcontext *context, std::string cache_folder)
    : context_{context}, folder_{std::move(cache_folder)}
{
    for (int index = 0; index < kWorkers; ++index)
        workers_.emplace_back([this] { work(); });
}

Images::~Images()
{
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_all();
    for (std::thread &worker : workers_)
        worker.join();
    for (const auto &[address, entry] : entries_)
        if (entry.texture.handle != 0)
            nvgDeleteImage(context_, entry.texture.handle);
}

// Reads and decodes one file. Runs on a worker thread, so it touches nothing of the
// class's and nothing of the drawing library's.
Images::Decoded Images::decode(const Job &job)
{
    Decoded result;
    result.address = job.key;
    const std::vector<unsigned char> bytes = read_file(job.file);
    if (bytes.empty())
        return result;

    int width = 0, height = 0;
    unsigned char *pixels = nullptr;
    const bool webp = is_webp(bytes);
    if (webp)
    {
        // Stremio's image service sends WebP, which the general decoder cannot read.
        pixels = WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height);
    }
    else
    {
        int components = 0;
        pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width,
                                       &height, &components, 4);
    }
    if (pixels == nullptr || width <= 0 || height <= 0)
        return result;

    // The bounds of what is visible. A file that decodes to nothing but transparency (a
    // placeholder some services send for a logo they do not have) is not used at all.
    int left = width, top = height, right = -1, bottom = -1;
    for (int y = 0; y < height; ++y)
    {
        const unsigned char *row = pixels + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x)
            if (row[x * 4 + 3] > kVisibleAlpha)
            {
                if (x < left)
                    left = x;
                if (x > right)
                    right = x;
                if (y < top)
                    top = y;
                bottom = y;
            }
    }
    if (right >= left && bottom >= top)
    {
        // Shrink by a whole factor (averaging each block of pixels) when the image is at
        // least twice as wide as it is ever drawn. Textures have no smaller copies of
        // themselves to fall back on, so this is also what keeps a big image from
        // shimmering when drawn small.
        const int limit = job.soft ? (job.light ? kLightSoftWidth : kSoftWidth) : job.max_width;
        const int factor = limit > 0 ? std::max(1, width / limit) : 1;
        const int out_width = width / factor, out_height = height / factor;
        result.width = out_width;
        result.height = out_height;
        result.content_x = left / factor;
        result.content_y = top / factor;
        result.content_width = std::max(1, std::min(out_width - result.content_x, (right - left + factor) / factor));
        result.content_height = std::max(1, std::min(out_height - result.content_y, (bottom - top + factor) / factor));
        if (factor == 1)
        {
            result.pixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
        }
        else
        {
            result.pixels.resize(static_cast<std::size_t>(out_width) * out_height * 4);
            const unsigned area = static_cast<unsigned>(factor * factor);
            for (int y = 0; y < out_height; ++y)
                for (int x = 0; x < out_width; ++x)
                {
                    unsigned sum[4] = {0, 0, 0, 0};
                    for (int dy = 0; dy < factor; ++dy)
                    {
                        const unsigned char *source =
                            pixels + (static_cast<std::size_t>(y * factor + dy) * width + x * factor) * 4;
                        for (int dx = 0; dx < factor; ++dx, source += 4)
                        {
                            sum[0] += source[0];
                            sum[1] += source[1];
                            sum[2] += source[2];
                            sum[3] += source[3];
                        }
                    }
                    unsigned char *target =
                        result.pixels.data() + (static_cast<std::size_t>(y) * out_width + x) * 4;
                    for (int channel = 0; channel < 4; ++channel)
                        target[channel] = static_cast<unsigned char>(sum[channel] / area);
                }
        }
    }
    if (webp)
        WebPFree(pixels);
    else
        stbi_image_free(pixels);
    if (job.soft && !result.pixels.empty())
        soften(result.pixels, result.width, result.height, job.light ? 2 : 3);
    return result;
}

void Images::work()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock{mutex_};
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_)
                return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Decoded result = decode(job);
        const std::lock_guard<std::mutex> lock{mutex_};
        decoded_.push_back(std::move(result));
    }
}

void Images::begin_frame()
{
    ++frame_;

    // Decoded images become textures here, on the drawing thread, a few each frame.
    for (int count = 0; count < kUploadsPerFrame; ++count)
    {
        Decoded image;
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            if (decoded_.empty())
                break;
            image = std::move(decoded_.front());
            decoded_.pop_front();
        }
        decoding_.erase(image.address);
        Entry entry;
        entry.last_used = frame_;
        if (!image.pixels.empty())
            entry.texture.handle =
                nvgCreateImageRGBA(context_, image.width, image.height, 0, image.pixels.data());
        if (entry.texture.handle != 0)
        {
            entry.texture.state = State::Ready;
            entry.texture.width = image.width;
            entry.texture.height = image.height;
            entry.texture.content_x = image.content_x;
            entry.texture.content_y = image.content_y;
            entry.texture.content_width = image.content_width;
            entry.texture.content_height = image.content_height;
            entry.bytes = static_cast<std::size_t>(image.width) * image.height * 4;
            texture_bytes_ += entry.bytes;
        }
        // An image that could not be used is remembered too, as unavailable.
        entries_[image.address] = entry;
    }

    if (texture_bytes_ > kTextureBudget)
        trim();
}

void Images::set_fetcher(Fetch fetch, Failed failed)
{
    fetch_ = std::move(fetch);
    failed_ = std::move(failed);
}

// Frees the textures that were drawn longest ago until the total is back under budget.
// Their files stay in the folder, so they come back quickly if they are wanted again.
void Images::trim()
{
    while (texture_bytes_ > kTextureBudget)
    {
        auto oldest = entries_.end();
        for (auto entry = entries_.begin(); entry != entries_.end(); ++entry)
            if (entry->second.texture.handle != 0 &&
                frame_ - entry->second.last_used > kKeepFrames &&
                (oldest == entries_.end() || entry->second.last_used < oldest->second.last_used))
                oldest = entry;
        if (oldest == entries_.end())
            return; // everything left is on screen
        nvgDeleteImage(context_, oldest->second.texture.handle);
        texture_bytes_ -= oldest->second.bytes;
        entries_.erase(oldest);
    }
}

// Makes sure `address` is being downloaded. Returns false when the download has failed.
bool Images::request(const std::string &address, const std::string &file)
{
    const auto download = downloads_.find(address);
    if (download == downloads_.end())
    {
        if (!fetch_)
            return false; // nothing can download it (the PC preview)
        fetch_(address, file);
        downloads_[address].look_again = frame_ + kLookEveryFrames;
        return true;
    }
    return !(failed_ && failed_(address));
}

void Images::prefetch(const std::string &address)
{
    if (address.empty() || entries_.count(address) != 0 || downloads_.count(address) != 0 ||
        decoding_.count(address) != 0)
        return;
    const std::string file = folder_ + "/" + image_cache_name(address);
    if (file_exists(file))
    {
        // Remembered as "no download needed", so the folder is not checked again.
        downloads_[address].look_again = 0;
        return;
    }
    request(address, file);
}

Images::Texture Images::get(const std::string &address, int max_width)
{
    return find(address, address, max_width, false, false);
}

Images::Texture Images::get_soft(const std::string &address, bool light)
{
    if (address.empty())
        return {};
    // The control character keeps the key from ever matching a real address.
    return find(address, address + (light ? "\x01light" : "\x01soft"), 0, true, light);
}

// `address` names the file; `key` is what the texture is filed under.
Images::Texture Images::find(const std::string &address, const std::string &key, int max_width,
                             bool soft, bool light)
{
    if (address.empty())
        return {};
    if (const auto found = entries_.find(key); found != entries_.end())
    {
        found->second.last_used = frame_;
        return found->second.texture;
    }
    Texture pending;
    pending.state = State::Pending;
    if (decoding_.count(key) != 0)
        return pending;

    // An image that is still downloading is looked for every so often, not every frame.
    const auto download = downloads_.find(address);
    if (download != downloads_.end() && frame_ < download->second.look_again)
        return pending;
    const std::string file = folder_ + "/" + image_cache_name(address);
    if (!file_exists(file))
    {
        if (!request(address, file))
        {
            entries_[key] = Entry{}; // remembered as unavailable
            return {};
        }
        downloads_[address].look_again = frame_ + kLookEveryFrames;
        return pending;
    }
    downloads_.erase(address);

    // The file is there: a worker reads and decodes it.
    decoding_.insert(key);
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        jobs_.push_back(Job{address, file, max_width, key, soft, light});
    }
    wake_.notify_one();
    return pending;
}
} // namespace ui
