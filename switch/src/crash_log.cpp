// What the app writes to its log when it dies, so that a crash on the console can be
// read afterwards: the kind of fault and where it was, as places in the app's program
// (see tools/where.sh, which turns them into function names and lines).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <typeinfo>

#include <switch.h>

void log_line(const char *format, ...);

extern "C"
{
// Where the program was loaded (the linker's own symbol for its first byte).
extern char __start__[] __attribute__((visibility("hidden")));

// libnx hands a crashed thread to this function, on a stack of its own.
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof __nx_exception_stack;

void __libnx_exception_handler(ThreadExceptionDump *dump)
{
    const auto base = reinterpret_cast<std::uint64_t>(__start__);
    const auto place = [base](std::uint64_t address) { return address - base; };
    log_line("CRASH: fault 0x%x, at program+0x%llx, called from program+0x%llx, touching address 0x%llx",
             dump->error_desc, static_cast<unsigned long long>(place(dump->pc.x)),
             static_cast<unsigned long long>(place(dump->lr.x)), static_cast<unsigned long long>(dump->far.x));
    // The callers, as far as the frame records go.
    std::uint64_t frame = dump->fp.x;
    for (int depth = 0; depth < 12 && frame != 0 && (frame & 7) == 0; ++depth)
    {
        const auto *record = reinterpret_cast<const std::uint64_t *>(frame);
        log_line("CRASH:   caller %d at program+0x%llx", depth, static_cast<unsigned long long>(place(record[1])));
        if (record[0] <= frame)
            break;
        frame = record[0];
    }
}
}

// The other ways out: a C++ failure that nothing handles, and the app ending by itself
// (which is also where a Rust panic and abort() end up).
void watch_for_endings()
{
    std::set_terminate([] {
        // What was thrown, and from where (this file alone is built with exceptions on,
        // to be able to ask).
        const char *what = "nothing thrown";
        char text[256] = {};
        if (std::exception_ptr thrown = std::current_exception())
        {
            try
            {
                std::rethrow_exception(thrown);
            }
            catch (const std::exception &error)
            {
                std::snprintf(text, sizeof text, "%s: %s", typeid(error).name(), error.what());
                what = text;
            }
            catch (...)
            {
                what = "something that is not a std::exception";
            }
        }
        log_line("ENDING: std::terminate was called (%s)", what);
        const auto base = reinterpret_cast<std::uint64_t>(__start__);
        auto frame = reinterpret_cast<std::uint64_t>(__builtin_frame_address(0));
        for (int depth = 0; depth < 14 && frame != 0 && (frame & 7) == 0; ++depth)
        {
            const auto *record = reinterpret_cast<const std::uint64_t *>(frame);
            log_line("ENDING:   caller %d at program+0x%llx", depth,
                     static_cast<unsigned long long>(record[1] - base));
            if (record[0] <= frame)
                break;
            frame = record[0];
        }
        std::abort();
    });
    std::atexit([] { log_line("ENDING: the app is exiting"); });
}
