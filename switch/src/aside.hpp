// Work done on a thread of its own that nobody waits for: fetching a subtitle, winding
// down a video that has been closed.
//
// Elsewhere such a thread is simply let go of. This console's threads cannot be (asking
// to detach one is an error), so they are kept, and each is joined, once it has finished,
// the next time anything is run aside.

#pragma once

#include <functional>

namespace nx
{
void run_aside(std::function<void()> work);
} // namespace nx
