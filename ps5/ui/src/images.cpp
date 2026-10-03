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
    for (const auto &[address, texture] : textures_)
        if (texture.handle != 0)
            nvgDeleteImage(context_, texture.handle);
}

void Images::begin_frame()
{
    decodes_left_ = kDecodesPerFrame;
}

Images::Texture Images::get(const std::string &address)
{
    if (address.empty())
        return {};
    if (const auto found = textures_.find(address); found != textures_.end())
        return found->second;
    if (decodes_left_ == 0)
        return {};

    std::vector<unsigned char> bytes = read_file(folder_ + "/" + image_cache_name(address));
    if (bytes.empty())
        return {}; // not downloaded yet: asked again on a later frame
    --decodes_left_;

    Texture texture;
    const int flags = NVG_IMAGE_GENERATE_MIPMAPS;
    if (is_webp(bytes))
    {
        // Stremio's poster service sends WebP, which the drawing library cannot read.
        int width = 0, height = 0;
        if (std::uint8_t *pixels = WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height))
        {
            texture.handle = nvgCreateImageRGBA(context_, width, height, flags, pixels);
            WebPFree(pixels);
        }
    }
    else
    {
        texture.handle =
            nvgCreateImageMem(context_, flags, bytes.data(), static_cast<int>(bytes.size()));
    }
    if (texture.handle != 0)
        nvgImageSize(context_, texture.handle, &texture.width, &texture.height);
    // A file that cannot be decoded is remembered too, so it is not retried every frame.
    textures_.emplace(address, texture);
    return texture;
}
} // namespace ui
