#include "images.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "nanovg.h"
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
} // namespace

Images::Images(NVGcontext *context, std::string cache_folder)
    : context_{context}, folder_{std::move(cache_folder)}
{
}

Images::~Images()
{
    for (const auto &[address, entry] : entries_)
        if (entry.texture.handle != 0)
            nvgDeleteImage(context_, entry.texture.handle);
}

void Images::begin_frame()
{
    decodes_left_ = kDecodesPerFrame;
    ++frame_;
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
    if (address.empty() || entries_.count(address) != 0 || downloads_.count(address) != 0)
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

Images::Texture Images::get(const std::string &address)
{
    if (address.empty())
        return {};
    if (const auto found = entries_.find(address); found != entries_.end())
    {
        found->second.last_used = frame_;
        return found->second.texture;
    }
    const Texture pending{State::Pending, 0, 0, 0};
    if (decodes_left_ == 0)
        return pending;

    // An image that is still downloading is looked for every so often, not every frame.
    const auto download = downloads_.find(address);
    if (download != downloads_.end() && frame_ < download->second.look_again)
        return pending;
    const std::string file = folder_ + "/" + image_cache_name(address);
    std::vector<unsigned char> bytes = read_file(file);
    if (bytes.empty())
    {
        if (!request(address, file))
        {
            entries_[address] = Entry{}; // remembered as unavailable
            return {};
        }
        downloads_[address].look_again = frame_ + kLookEveryFrames;
        return pending;
    }
    downloads_.erase(address);
    --decodes_left_;

    Entry entry;
    entry.last_used = frame_;
    const int flags = NVG_IMAGE_GENERATE_MIPMAPS;
    if (is_webp(bytes))
    {
        // Stremio's image service sends WebP, which the drawing library cannot read.
        int width = 0, height = 0;
        if (std::uint8_t *pixels = WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height))
        {
            entry.texture.handle = nvgCreateImageRGBA(context_, width, height, flags, pixels);
            WebPFree(pixels);
        }
    }
    else
    {
        entry.texture.handle =
            nvgCreateImageMem(context_, flags, bytes.data(), static_cast<int>(bytes.size()));
    }
    if (entry.texture.handle != 0)
    {
        nvgImageSize(context_, entry.texture.handle, &entry.texture.width, &entry.texture.height);
        entry.texture.state = State::Ready;
        // Four bytes a pixel, plus a third for the smaller copies used when drawn small.
        entry.bytes = static_cast<std::size_t>(entry.texture.width) * entry.texture.height * 16 / 3;
        texture_bytes_ += entry.bytes;
    }
    // A file that is not an image is remembered too, so it is not decoded again every frame.
    entries_.emplace(address, entry);
    return entry.texture;
}
} // namespace ui
