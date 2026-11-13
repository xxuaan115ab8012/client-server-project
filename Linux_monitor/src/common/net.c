/*
 * net.c - reliable socket helpers (libcommon.so)
 *
 * Design notes
 *  - Every blocking wait goes through poll() with the remaining time of a
 *    CLOCK_MONOTONIC deadline computed once per call (SPEC NFR-08).
 *  - net_readline() uses MSG_PEEK so that it never consumes bytes that
 *    belong to the next line; it therefore needs no per-fd buffer and is
 *    safe to call repeatedly on a persistent connection.
 *  - All send()/recv() calls are retried on EINTR and short transfers
 *    (SPEC RB-06); send() uses MSG_NOSIGNAL (SPEC RB-05).
 */
#include "net.h"
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

long long net_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* Remaining ms until deadline; -1 for "infinite"; 0 when already expired. */
static int remaining_ms(long long deadline_abs_ms)
{
    long long now, rem;
    if (deadline_abs_ms < 0)
        return -1;
    now = net_monotonic_ms();
    rem = deadline_abs_ms - now;
    if (rem <= 0)
        return 0;
    if (rem > 0x7fffffffLL)
        rem = 0x7fffffffLL;
    return (int)rem;
}

static long long make_deadline(int deadline_ms)
{
    if (deadline_ms < 0)
        return -1;
    return net_monotonic_ms() + deadline_ms;
}

/*
 * Wait until fd is ready for `events` or the deadline expires.
 * Returns 1 ready, NET_TIMEOUT, or NET_ERROR (errno set).
 */
static int wait_fd(int fd, short events, long long deadline_abs_ms)
{
    for (;;) {
        struct pollfd pfd;
        int timeout = remaining_ms(deadline_abs_ms);
        int rc;

        if (timeout == 0)
            return NET_TIMEOUT;

        pfd.fd = fd;
        pfd.events = events;
        pfd.revents = 0;

        rc = poll(&pfd, 1, timeout);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return NET_ERROR;
        }
        if (rc == 0)
            return NET_TIMEOUT;
        /* POLLHUP / POLLERR are reported to the caller via recv/send. */
        return 1;
    }
}

int net_readline(int fd, char *buf, size_t cap, int deadline_ms)
{
    long long deadline;
    size_t limit;      /* max bytes of the line including LF */
    size_t have = 0;   /* bytes already consumed into buf    */

    if (buf == NULL || cap < 2) {
        errno = EINVAL;
        return NET_ERROR;
    }
    limit = cap - 1;
    deadline = make_deadline(deadline_ms);

    for (;;) {
        ssize_t n;
        size_t want = limit - have;
        char *nl;
        int rc;

        if (want == 0)
            return NET_TOO_LONG;

        rc = wait_fd(fd, POLLIN, deadline);
        if (rc != 1)
            return rc;

        /* Peek at what is available without consuming it. */
        n = recv(fd, buf + have, want, MSG_PEEK | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return NET_ERROR;
        }
        if (n == 0)
            return NET_EOF;   /* peer closed (partial line discarded) */

        nl = memchr(buf + have, '\n', (size_t)n);
        if (nl != NULL) {
            size_t take = (size_t)(nl - (buf + have)) + 1;   /* incl. LF */
            size_t got = 0;
            /* Consume exactly `take` bytes (loop for short reads). */
            while (got < take) {
                ssize_t m = recv(fd, buf + have + got, take - got, 0);
                if (m < 0) {
                    if (errno == EINTR)
                        continue;
                    return NET_ERROR;
                }
                if (m == 0)
                    return NET_EOF;
                got += (size_t)m;
            }
            have += take;
            /* Strip LF and optional CR, NUL terminate. */
            have--;                       /* drop LF */
            if (have > 0 && buf[have - 1] == '\r')
                have--;
            buf[have] = '\0';
            return (int)have;
        }

        /* No LF in the peeked window: consume it all and keep reading. */
        {
            size_t got = 0;
            size_t take = (size_t)n;
            while (got < take) {
                ssize_t m = recv(fd, buf + have + got, take - got, 0);
                if (m < 0) {
                    if (errno == EINTR)
                        continue;
                    return NET_ERROR;
                }
                if (m == 0)
                    return NET_EOF;
                got += (size_t)m;
            }
            have += take;
        }
        if (have >= limit)
            return NET_TOO_LONG;   /* line (incl. LF) would exceed limit */
    }
}

int net_write_all(int fd, const void *buf, size_t len, int deadline_ms)
{
    const char *p = (const char *)buf;
    size_t sent = 0;
    long long deadline = make_deadline(deadline_ms);

    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent,
                         MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            int rc;
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                return NET_ERROR;          /* EPIPE, ECONNRESET, ...   */
            rc = wait_fd(fd, POLLOUT, deadline);
            if (rc != 1)
                return rc;                 /* NET_TIMEOUT / NET_ERROR  */
            continue;
        }
        sent += (size_t)n;                 /* short write: loop again  */
    }
    return NET_OK;
}

static int resolve(const char *host, unsigned short port, int passive,
                   struct addrinfo **res)
{
    struct addrinfo hints;
    char portstr[16];

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);

    return getaddrinfo(host, portstr, &hints, res);
}

int net_connect(const char *host, unsigned short port)
{
    struct addrinfo *res = NULL, *ai;
    int fd = -1;
    int saved_errno = ECONNREFUSED;

    if (host == NULL)
        host = DEFAULT_HOST;

    if (resolve(host, port, 0, &res) != 0) {
        errno = EINVAL;
        return -1;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }
        for (;;) {
            if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
                break;
            if (errno == EINTR)
                continue;
            saved_errno = errno;
            close(fd);
            fd = -1;
            break;
        }
        if (fd >= 0)
            break;
    }
    freeaddrinfo(res);
    if (fd < 0)
        errno = saved_errno;
    return fd;
}

int net_listen(const char *host, unsigned short port, int backlog)
{
    struct addrinfo *res = NULL;
    int fd;
    int one = 1;
    int saved_errno;

    if (host == NULL)
        host = DEFAULT_HOST;

    if (resolve(host, port, 1, &res) != 0 || res == NULL) {
        errno = EINVAL;
        return -1;
    }

    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        saved_errno = errno;
        freeaddrinfo(res);
        errno = saved_errno;
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0 ||
        bind(fd, res->ai_addr, res->ai_addrlen) < 0 ||
        listen(fd, backlog) < 0) {
        saved_errno = errno;
        close(fd);
        freeaddrinfo(res);
        errno = saved_errno;
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}

int net_peer_string(int fd, char *out, size_t cap)
{
    struct sockaddr_in sin;
    socklen_t slen = sizeof sin;
    char ip[INET_ADDRSTRLEN];

    if (out == NULL || cap == 0)
        return -1;
    out[0] = '\0';
    if (getpeername(fd, (struct sockaddr *)&sin, &slen) != 0)
        return -1;
    if (sin.sin_family != AF_INET ||
        inet_ntop(AF_INET, &sin.sin_addr, ip, sizeof ip) == NULL)
        return -1;
    if (snprintf(out, cap, "%s:%u", ip, (unsigned)ntohs(sin.sin_port)) < 0)
        return -1;
    return 0;
}

const char *net_result_name(int rc)
{
    switch (rc) {
    case NET_OK:       return "OK";
    case NET_EOF:      return "EOF";
    case NET_TIMEOUT:  return "TIMEOUT";
    case NET_TOO_LONG: return "TOO_LONG";
    case NET_ERROR:    return "ERROR";
    default:           return rc >= 0 ? "OK" : "UNKNOWN";
    }
}
