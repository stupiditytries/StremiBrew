/*
 * Step-by-step trace of the socket features asynchronous network code needs: switching a
 * socket to non-blocking mode, a non-blocking connect awaited with poll(), creating a
 * socket that is non-blocking from the start, and kernel event queues. Each step reports
 * its return value and errno through `log`.
 */

#include <stdint.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static void report(void (*log)(const char *), const char *step, long result)
{
    char line[96];
    snprintf(line, sizeof line, "NET %s = %ld errno %d", step, result, result < 0 ? errno : 0);
    log(line);
}

static void await_connect(void (*log)(const char *), int descriptor,
                          const struct addrinfo *target)
{
    int connected = connect(descriptor, target->ai_addr, target->ai_addrlen);
    report(log, "connect", connected);
    if (connected < 0 && errno == EINPROGRESS)
    {
        struct pollfd wait = {.fd = descriptor, .events = POLLOUT};
        report(log, "poll", poll(&wait, 1, 10000));
        report(log, "poll revents", wait.revents);
        int pending = 0;
        socklen_t length = sizeof pending;
        report(log, "getsockopt SO_ERROR",
               getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &pending, &length));
        report(log, "pending socket error", pending);
    }
}

static void attempt(void (*log)(const char *), const struct addrinfo *target, int method)
{
    int type = SOCK_STREAM;
    if (method == 2)
        type |= SOCK_NONBLOCK | SOCK_CLOEXEC;
    int descriptor = socket(AF_INET, type, 0);
    report(log, "socket", descriptor);
    if (descriptor < 0)
        return;
    if (method == 0)
    {
        int enabled = 1;
        report(log, "ioctl FIONBIO", ioctl(descriptor, FIONBIO, &enabled));
    }
    else if (method == 1)
    {
        int flags = fcntl(descriptor, F_GETFL, 0);
        report(log, "fcntl F_GETFL", flags);
        report(log, "fcntl F_SETFL O_NONBLOCK", fcntl(descriptor, F_SETFL, flags | O_NONBLOCK));
    }
    report(log, "fcntl F_GETFL after", fcntl(descriptor, F_GETFL, 0));
    await_connect(log, descriptor, target);
    close(descriptor);
}

static void event_queue(void (*log)(const char *), const struct addrinfo *target)
{
    int queue = kqueue();
    report(log, "kqueue", queue);
    if (queue < 0)
        return;
    int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    int enabled = 1;
    report(log, "ioctl FIONBIO", ioctl(descriptor, FIONBIO, &enabled));
    report(log, "connect", connect(descriptor, target->ai_addr, target->ai_addrlen));
    struct kevent change;
    EV_SET(&change, descriptor, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);
    report(log, "kevent add", kevent(queue, &change, 1, NULL, 0, NULL));
    struct kevent event;
    struct timespec timeout = {.tv_sec = 10};
    int count = kevent(queue, NULL, 0, &event, 1, &timeout);
    report(log, "kevent wait", count);
    if (count == 1)
    {
        report(log, "kevent filter", event.filter);
        report(log, "kevent flags", event.flags);
    }
    close(descriptor);
    close(queue);
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
    log("NET method socket flag");
    attempt(log, target, 2);
    log("NET event queue");
    event_queue(log, target);
    freeaddrinfo(target);
}
