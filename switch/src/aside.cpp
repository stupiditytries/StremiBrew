#include "aside.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace nx
{
namespace
{
struct Aside
{
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};

std::mutex mutex;
// Never destroyed: threads still at work when the app ends are left to end with it.
std::vector<Aside> &running = *new std::vector<Aside>;
} // namespace

void run_aside(std::function<void()> work)
{
    const std::lock_guard lock{mutex};
    for (auto aside = running.begin(); aside != running.end();)
    {
        if (*aside->finished)
        {
            aside->thread.join();
            aside = running.erase(aside);
        }
        else
            ++aside;
    }
    auto finished = std::make_shared<std::atomic<bool>>(false);
    running.push_back({std::thread{[work = std::move(work), finished] {
                           work();
                           *finished = true;
                       }},
                       finished});
}
} // namespace nx
