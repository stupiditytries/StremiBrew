// Functions Rust's standard library expects of the C library that the Switch's does not
// have.

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

#include <switch.h>

// Random bytes, from the console's generator.
ssize_t getrandom(void *buffer, size_t length, unsigned int flags)
{
    (void)flags;
    randomGet(buffer, length);
    return (ssize_t)length;
}

// The few facts about the system that are asked for.
long sysconf(int name)
{
    switch (name)
    {
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF:
        return 3; // of the console's four cores, the ones an app's threads run on
    case _SC_PAGESIZE:
        return 0x1000;
    default:
        errno = EINVAL;
        return -1;
    }
}
