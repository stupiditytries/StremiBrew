// Two things about the console that the rest of the app (the C++, the Rust and FFmpeg
// alike) should not have to know. Both work by standing in front of a C library function
// (the linker's --wrap: see build.sh).
//
// Threads: one made without saying how much stack it wants gets very little from the
// console's C library; here it gets a megabyte.
//
// The clock: a console that is kept offline can have its clock months out, and then every
// secure connection fails (a certificate looks not yet valid, or expired) and what the
// account records is dated wrongly. When the app starts, the time is asked of the
// network, and if the console's clock disagrees the difference is added to what the
// clock says from then on, for this app only.

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

void stremio_host_log(const char *text);

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*entry)(void *),
                          void *argument);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*entry)(void *),
                          void *argument)
{
    if (attributes != NULL)
        return __real_pthread_create(thread, attributes, entry, argument);
    pthread_attr_t roomy;
    pthread_attr_init(&roomy);
    pthread_attr_setstacksize(&roomy, 1u << 20);
    const int result = __real_pthread_create(thread, &roomy, entry, argument);
    pthread_attr_destroy(&roomy);
    return result;
}

// Seconds added to the console's clock.
static volatile long long clock_correction;

int __real_clock_gettime(clockid_t clock, struct timespec *out);

int __wrap_clock_gettime(clockid_t clock, struct timespec *out)
{
    const int result = __real_clock_gettime(clock, out);
    if (result == 0 && clock == CLOCK_REALTIME)
        out->tv_sec += clock_correction;
    return result;
}

// Days from 1 January 1970 to a date.
static long long days_to(int year, int month, int day)
{
    year -= month <= 2;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const int of_era = (int)(year - era * 400);
    const int of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    return era * 146097 + (of_era * 365 + of_era / 4 - of_era / 100 + of_year) - 719468;
}

// Waits for a socket to be ready to read or write. Returns 0 when it is.
static int ready(int socket, short what, int milliseconds)
{
    struct pollfd watched = {socket, what, 0};
    return poll(&watched, 1, milliseconds) == 1 && (watched.revents & what) ? 0 : -1;
}

// The time by a web server's reckoning: the Date line of its answer to a request made
// in the clear (a secure one could not be trusted before the time is known). Returns 0
// and the time in seconds since 1970, or -1.
static int web_time(const char *host, long long *seconds)
{
    struct addrinfo hints, *found = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, "80", &hints, &found) != 0 || found == NULL)
        return -1;
    int outcome = -1;
    const int link = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    if (link >= 0)
    {
        // Nothing here waits more than a few seconds.
        fcntl(link, F_SETFL, fcntl(link, F_GETFL, 0) | O_NONBLOCK);
        int connected = connect(link, found->ai_addr, found->ai_addrlen);
        if (connected != 0 && errno == EINPROGRESS && ready(link, POLLOUT, 4000) == 0)
        {
            int failure = 0;
            socklen_t size = sizeof failure;
            if (getsockopt(link, SOL_SOCKET, SO_ERROR, &failure, &size) == 0 && failure == 0)
                connected = 0;
        }
        char request[200];
        const int length = snprintf(request, sizeof request,
                                    "HEAD / HTTP/1.1\r\nHost: %s\r\nUser-Agent: StremiBrew\r\nConnection: close\r\n\r\n",
                                    host);
        if (connected == 0 && ready(link, POLLOUT, 4000) == 0 && send(link, request, (size_t)length, 0) == length)
        {
            char reply[2048];
            size_t got = 0;
            while (got + 1 < sizeof reply && ready(link, POLLIN, 4000) == 0)
            {
                const ssize_t count = recv(link, reply + got, sizeof reply - 1 - got, 0);
                if (count <= 0)
                    break;
                got += (size_t)count;
                reply[got] = 0;
                if (strstr(reply, "\r\n\r\n") != NULL)
                    break;
            }
            reply[got] = 0;
            for (char *letter = reply; *letter != 0; ++letter)
                *letter = (char)tolower((unsigned char)*letter);
            // "date: wed, 08 oct 2026 01:23:45 gmt"
            const char *line = strstr(reply, "\ndate:");
            char month[4] = {0};
            int day = 0, year = 0, hour = 0, minute = 0, second = 0;
            if (line != NULL &&
                sscanf(line + 6, " %*[a-z], %d %3s %d %d:%d:%d", &day, month, &year, &hour, &minute, &second) == 6)
            {
                static const char names[] = "janfebmaraprmayjunjulaugsepoctnovdec";
                const char *at = strstr(names, month);
                if (at != NULL && year > 2020)
                {
                    *seconds = days_to(year, (int)(at - names) / 3 + 1, day) * 86400 + hour * 3600 + minute * 60 + second;
                    outcome = 0;
                }
            }
        }
        close(link);
    }
    freeaddrinfo(found);
    return outcome;
}

// Asks the network the time and, if the console's clock is out, corrects what the app
// takes the time to be. Needs the network up; takes a few seconds at most.
void clock_check_against_network(void)
{
    static const char *const hosts[] = {"connectivitycheck.gstatic.com", "www.msftconnecttest.com", "example.com"};
    char text[200];
    for (size_t index = 0; index < sizeof hosts / sizeof hosts[0]; ++index)
    {
        long long network = 0;
        if (web_time(hosts[index], &network) != 0)
            continue;
        struct timespec console;
        __real_clock_gettime(CLOCK_REALTIME, &console);
        const long long apart = network - (long long)console.tv_sec;
        if (llabs(apart) >= 120)
        {
            clock_correction = apart;
            snprintf(text, sizeof text,
                     "clock: the console's is %lld days and %lld seconds from the network's (%s); corrected for this app",
                     apart / 86400, apart % 86400, hosts[index]);
        }
        else
            snprintf(text, sizeof text, "clock: agrees with the network's (%s) to %lld seconds", hosts[index], apart);
        stremio_host_log(text);
        return;
    }
    stremio_host_log("clock: the network did not say the time; the console's clock is used as it is");
}
