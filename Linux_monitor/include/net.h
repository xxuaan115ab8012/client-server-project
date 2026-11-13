/*
 * net.h - reliable socket helpers shared by client and server
 * (part of libcommon.so).
 *
 * All functions are blocking from the caller's point of view but use
 * poll() together with a CLOCK_MONOTONIC total deadline, so a peer that
 * trickles one byte at a time cannot hold a worker forever.
 */
#ifndef LSM_NET_H
#define LSM_NET_H

#include <stddef.h>

/* Return codes shared by net_readline() and net_write_all(). */
enum net_result {
    NET_OK       =  0,  /* success                                        */
    NET_EOF      = -1,  /* peer closed the connection (recv returned 0)   */
    NET_TIMEOUT  = -2,  /* total deadline expired                          */
    NET_TOO_LONG = -3,  /* line exceeds cap - 1 bytes (including LF)       */
    NET_ERROR    = -4   /* system error; errno is set                      */
};

/*
 * net_readline - read one LF-terminated line from fd.
 *
 *  buf/cap     : destination buffer. The longest accepted line, including
 *                its LF, is cap - 1 bytes so that a NUL always fits.
 *  deadline_ms : total time budget in ms; < 0 means wait forever.
 *
 * Returns the line length (>= 0) WITHOUT the LF and WITHOUT a trailing CR
 * (buf is NUL terminated) on success, otherwise one of the negative
 * NET_* codes: NET_EOF (peer closed before a full line arrived),
 * NET_TIMEOUT, NET_TOO_LONG or NET_ERROR (errno set).
 * EINTR is retried internally. No bytes past the LF are consumed, so the
 * function can be called repeatedly on a persistent connection.
 */
int net_readline(int fd, char *buf, size_t cap, int deadline_ms);

/*
 * net_write_all - send exactly len bytes, handling short writes, EINTR
 * and EAGAIN.  SIGPIPE is suppressed via MSG_NOSIGNAL.
 *
 * Returns NET_OK, NET_TIMEOUT (deadline expired before all bytes were
 * sent) or NET_ERROR (errno set, e.g. EPIPE / ECONNRESET).
 * deadline_ms < 0 means wait forever.
 */
int net_write_all(int fd, const void *buf, size_t len, int deadline_ms);

/*
 * net_connect - create a TCP socket and connect to host:port (IPv4).
 * Returns the connected fd, or -1 with errno set.
 */
int net_connect(const char *host, unsigned short port);

/*
 * net_listen - create a TCP listening socket bound to host:port with
 * SO_REUSEADDR enabled.  Returns the listening fd, or -1 with errno set.
 */
int net_listen(const char *host, unsigned short port, int backlog);

/*
 * net_peer_string - format the peer address of fd as "ip:port" into
 * out (at least 48 bytes recommended).  Returns 0 or -1.
 */
int net_peer_string(int fd, char *out, size_t cap);

/*
 * net_result_name - human readable name of a NET_* code (for logs).
 */
const char *net_result_name(int rc);

/*
 * net_monotonic_ms - milliseconds from CLOCK_MONOTONIC, exported so that
 * callers can measure elapsed time consistently.
 */
long long net_monotonic_ms(void);

#endif /* LSM_NET_H */
