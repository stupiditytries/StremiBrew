/*
 * Step-by-step trace of a non-blocking TCP connect, to find which call the console
 * rejects. Each step reports its return value and errno through `log`.
 */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static void report(void (*log)(const char *), const char *step, long result)
{
    char line[96];
    snprintf(line, sizeof line, "NET %s = %ld errno %d", step, result, result < 0 ? errno : 0);
    log(line);
}

static void attempt(void (*log)(const char *), const struct addrinfo *target, int method)
{
    int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    report(log, "socket", descriptor);
    if (descriptor < 0)
        return;
    if (method == 0)
    {
        int enabled = 1;
        report(log, "ioctl FIONBIO", ioctl(descriptor, FIONBIO, &enabled));
    }
    else
    {
        int flags = fcntl(descriptor, F_GETFL, 0);
        report(log, "fcntl F_GETFL", flags);
        report(log, "fcntl F_SETFL O_NONBLOCK", fcntl(descriptor, F_SETFL, flags | O_NONBLOCK));
    }
    int connected = connect(descriptor, target->ai_addr, target->ai_addrlen);
    report(log, "connect", connected);
    if (connected < 0 && errno == EINPROGRESS)
    {
        struct pollfd wait = {.fd = descriptor, .events = POLLOUT};
        int ready = poll(&wait, 1, 10000);
        report(log, "poll", ready);
        report(log, "poll revents", wait.revents);
        int pending = 0;
        socklen_t length = sizeof pending;
        report(log, "getsockopt SO_ERROR",
               getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &pending, &length));
        report(log, "pending socket error", pending);
    }
    close(descriptor);
}

void net_diag(void (*log)(const char *))
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *target = NULL;
    int status = getaddrinfo("example.com", "80", &hints, &target);
    report(log, "getaddrinfo", -status);
    if (status != 0)
        return;
    log("NET method ioctl");
    attempt(log, target, 0);
    log("NET method fcntl");
    attempt(log, target, 1);
    freeaddrinfo(target);
}
