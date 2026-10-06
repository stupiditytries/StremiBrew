#include "trailer_control.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

#include <GL/gl.h>

#include "nanovg.h"
#define NANOVG_GL3 1
#include "nanovg_gl.h"

extern "C"
{
std::size_t stremio_trailer(const char *id, char *out, std::size_t capacity);
}

void log_line(const char *format, ...);

namespace ps5
{
namespace
{
// The texture trailers are drawn into. Trailers are 720p at most; this leaves the
// featured area (about half the 4K screen's width) sharp.
constexpr int kWidth = 1920, kHeight = 1080;
// A trailer plays under the UI's own sounds, well below a video's full level.
constexpr float kVolume = 0.3f;
} // namespace

struct TrailerControl::Shared
{
    std::mutex mutex;
    int answered = 0; // the ticket of the lookup that `url` answers
    std::string url;  // empty when that title has no trailer
};

TrailerControl::TrailerControl(ui::App &app, NVGcontext *vg)
    : app_{app}, vg_{vg}, shared_{std::make_shared<Shared>()}
{
}

ui::TrailerHandler TrailerControl::handler()
{
    ui::TrailerHandler handler;
    handler.prepare = [this](const std::string &id) {
        player_.close();
        opened_ = false;
        started_ = false;
        playing_ = false;
        wanted_ = true;
        const int ticket = ++ticket_;
        std::thread{[shared = shared_, id, ticket] {
            char url[2048] = {};
            const std::size_t length = stremio_trailer(id.c_str(), url, sizeof url);
            std::lock_guard lock{shared->mutex};
            // A newer lookup's answer is not replaced by an older one arriving late.
            if (ticket > shared->answered)
            {
                shared->answered = ticket;
                shared->url.assign(url, length);
            }
        }}.detach();
    };
    handler.start = [this] { started_ = true; };
    handler.stop = [this] {
        player_.close();
        wanted_ = started_ = opened_ = playing_ = false;
    };
    return handler;
}

bool TrailerControl::create_target()
{
    glGenFramebuffers(1, &framebuffer_);
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!complete)
    {
        log_line("trailers: no texture to draw into");
        return false;
    }
    // A texture drawn into has its first row at the bottom; the UI's images have theirs
    // at the top.
    image_ = nvglCreateImageFromHandleGL3(vg_, texture_, kWidth, kHeight, NVG_IMAGE_FLIPY | NVG_IMAGE_NODELETE);
    return image_ != 0;
}

void TrailerControl::frame()
{
    // The answer to the lookup: the trailer is opened at once but held at its start
    // until the UI says to play, so the wait for it to open is spent while the focus
    // is still settling.
    if (wanted_ && !opened_)
    {
        std::string url;
        bool answered = false;
        {
            std::lock_guard lock{shared_->mutex};
            answered = shared_->answered == ticket_;
            if (answered)
                url = shared_->url;
        }
        // A trailer left behind takes a moment to close and holds its threads until it
        // has; the next one waits for that, or moving quickly along a row would pile
        // them up until the console has no threads left to give.
        if (answered && !url.empty() && Player::sessions() > 1)
            answered = false;
        if (answered)
        {
            if (url.empty())
            {
                wanted_ = false;
            }
            else
            {
                Player::Options options;
                options.preview = true;
                options.volume = kVolume;
                options.paused = true;
                player_.open(url, 0.0, {}, options);
                opened_ = true;
                opened_at_ = std::chrono::steady_clock::now();
            }
        }
    }
    bool live = false;
    float bar_top = 0, bar_bottom = 0;
    if (opened_ && player_.active())
    {
        if (framebuffer_ == 0 && !create_target())
        {
            player_.close();
            opened_ = wanted_ = false;
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glViewport(0, 0, kWidth, kHeight);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        player_.draw(kWidth, kHeight);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        const ui::Playback status = player_.status();
        // Whether the video carries black bars is settled before it plays, so that it is
        // shown at one size from its first moment. (Should that take unusually long, it
        // plays without waiting further, as it is.)
        float own_top = 0, own_bottom = 0;
        const bool decided = player_.bars_decided(own_top, own_bottom) ||
                             std::chrono::steady_clock::now() - opened_at_ > std::chrono::seconds(4);
        if (started_ && decided && !playing_)
        {
            playing_ = true;
            player_.set_paused(false);
        }
        live = playing_ && status.state == ui::Playback::State::Playing;
        // The black bars in the texture: the ones a picture wider than the texture
        // leaves above and below it, and any the picture itself carries.
        const float shape = player_.picture_aspect();
        const float filled = std::min(1.0f, (static_cast<float>(kWidth) / kHeight) / std::max(shape, 0.1f));
        bar_top = (1.0f - filled) / 2 + own_top * filled;
        bar_bottom = (1.0f - filled) / 2 + own_bottom * filled;
        if (status.state == ui::Playback::State::Ended || status.state == ui::Playback::State::Failed)
        {
            // Over (or it would not play): back to the artwork.
            player_.close();
            opened_ = wanted_ = playing_ = false;
        }
    }
    app_.set_trailer(image_, live, bar_top, bar_bottom);
}
} // namespace ps5
