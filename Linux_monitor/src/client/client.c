/*
 * client.c - Linux System Monitor TCP client
 *
 *   client [--host H] [--port N] [--command "CMD"] [--hold N] [--idle N]
 *          [--debug] [--help]
 *
 *   --command : send one command, print the response, then QUIT.
 *   --hold N  : after the response, keep the connection open N seconds
 *               before sending QUIT (concurrency demonstrations).
 *   --idle N  : connect and send nothing for N seconds (timeout tests).
 *               Mutually exclusive with --command / --hold.
 *   (none)    : interactive mode - read commands from stdin.
 *
 * Exit status: 0 success, 1 server replied ERR, 2 usage error,
 *              3 connection / transport error.
 */
#include "common.h"
#include "logger.h"
#include "net.h"
#include "protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EXIT_OK        0
#define EXIT_SERVER_ERR 1
#define EXIT_USAGE     2
#define EXIT_NET       3

struct client_opts {
    const char *host;
    unsigned short port;
    const char *command;
    long hold_sec;
    long idle_sec;
    int debug;
};

static void usage(const char *prog, FILE *out)
{
    fprintf(out,
        "Usage: %s [--host ADDR] [--port N] [--command \"CMD\"] [--hold N]\n"
        "       [--idle N] [--debug] [--help]\n"
        "  --host ADDR     server address (default %s)\n"
        "  --port N        server port (default %d)\n"
        "  --command CMD   send one command (e.g. \"GET ALL\") and exit\n"
        "  --hold N        keep connection open N sec after the response\n"
        "  --idle N        connect, send nothing for N sec (timeout test)\n"
        "  --debug         show DEBUG log lines (only if built with DEBUG=1)\n"
        "  --help          show this help\n"
        "Without --command the client reads commands from stdin.\n",
        prog, DEFAULT_HOST, DEFAULT_PORT);
}

static int parse_long(const char *s, long lo, long hi, long *out)
{
    char *end = NULL;
    long v;
    if (s == NULL || *s == '\0')
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

static int parse_args(int argc, char **argv, struct client_opts *o)
{
    int have_hold = 0, have_idle = 0;
    long v;

    o->host = DEFAULT_HOST;
    o->port = DEFAULT_PORT;
    o->command = NULL;
    o->hold_sec = 0;
    o->idle_sec = 0;
    o->debug = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0], stdout);
            exit(EXIT_OK);
        } else if (strcmp(a, "--debug") == 0) {
            o->debug = 1;
        } else if (strcmp(a, "--host") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "error: --host requires a value\n");
                return -1;
            }
            o->host = argv[i];
        } else if (strcmp(a, "--port") == 0) {
            if (++i >= argc || parse_long(argv[i], 1, 65535, &v) != 0) {
                fprintf(stderr, "error: --port requires a number 1-65535\n");
                return -1;
            }
            o->port = (unsigned short)v;
        } else if (strcmp(a, "--command") == 0) {
            if (++i >= argc || argv[i][0] == '\0') {
                fprintf(stderr, "error: --command requires a value\n");
                return -1;
            }
            o->command = argv[i];
        } else if (strcmp(a, "--hold") == 0) {
            if (++i >= argc || parse_long(argv[i], 0, 3600, &v) != 0) {
                fprintf(stderr, "error: --hold requires seconds 0-3600\n");
                return -1;
            }
            o->hold_sec = v;
            have_hold = 1;
        } else if (strcmp(a, "--idle") == 0) {
            if (++i >= argc || parse_long(argv[i], 1, 3600, &v) != 0) {
                fprintf(stderr, "error: --idle requires seconds 1-3600\n");
                return -1;
            }
            o->idle_sec = v;
            have_idle = 1;
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            return -1;
        }
    }

    /* Mutually exclusive combinations (SPEC 10.2). */
    if (have_idle && (o->command != NULL || have_hold)) {
        fprintf(stderr, "error: --idle cannot be combined with --command or --hold\n");
        return -1;
    }
    if (o->command != NULL && strlen(o->command) + 1 > MAX_REQUEST_LEN) {
        /* Still allowed for testing REQUEST_TOO_LONG, but warn. */
        LOG_INFO("warning: command exceeds %d bytes; server will reject it",
                 MAX_REQUEST_LEN);
    }
    return 0;
}

static void sleep_sec(long sec)
{
    struct timespec ts;
    ts.tv_sec = sec;
    ts.tv_nsec = 0;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

/* Send "cmd\n". Returns NET_OK or a NET_* error. */
static int send_command(int fd, const char *cmd)
{
    char buf[4096];
    int n = snprintf(buf, sizeof buf, "%s\n", cmd);
    if (n < 0 || (size_t)n >= sizeof buf) {
        errno = EMSGSIZE;
        return NET_ERROR;
    }
    LOG_DEBUG("send: %s", cmd);
    return net_write_all(fd, buf, (size_t)n, WRITE_DEADLINE_MS);
}

/*
 * Receive one response line into resp. Returns line length (>= 0) or a
 * negative NET_* code; logs the reason on failure.
 */
static int recv_response(int fd, char *resp, size_t cap, int deadline_ms)
{
    int n = net_readline(fd, resp, cap, deadline_ms);
    if (n >= 0) {
        LOG_DEBUG("recv: %s", resp);
        return n;
    }
    switch (n) {
    case NET_EOF:
        LOG_INFO("server closed the connection");
        break;
    case NET_TIMEOUT:
        LOG_ERROR("no response within %d ms", deadline_ms);
        break;
    case NET_TOO_LONG:
        LOG_ERROR("response exceeds %zu bytes", cap - 1);
        break;
    default:
        LOG_ERROR("recv failed errno=%d (%s)", errno, strerror(errno));
        break;
    }
    return n;
}

/* Send QUIT and consume the "OK bye=1" reply (best effort). */
static void send_quit(int fd)
{
    char resp[MAX_RESPONSE_LEN + 1];
    if (send_command(fd, PROTO_CMD_QUIT) != NET_OK)
        return;
    (void)recv_response(fd, resp, sizeof resp, CLIENT_REPLY_DEADLINE_MS);
}

/* ---- modes ------------------------------------------------------------- */

static int run_idle(int fd, long idle_sec)
{
    char resp[MAX_RESPONSE_LEN + 1];
    int n;

    LOG_INFO("idle mode: holding connection %ld s without sending", idle_sec);
    /* Wait for the whole idle period, but notice if the server speaks. */
    n = recv_response(fd, resp, sizeof resp, (int)(idle_sec * 1000));
    if (n >= 0) {
        printf("%s\n", resp);
        fflush(stdout);
        if (proto_is_err(resp))
            LOG_INFO("server replied with an error during idle period");
    } else if (n == NET_TIMEOUT) {
        LOG_INFO("idle period elapsed, connection still open; closing");
    }
    return EXIT_OK;
}

static int run_single(int fd, const struct client_opts *o)
{
    char resp[MAX_RESPONSE_LEN + 1];
    int n;

    if (send_command(fd, o->command) != NET_OK) {
        LOG_ERROR("send failed errno=%d (%s)", errno, strerror(errno));
        return EXIT_NET;
    }
    n = recv_response(fd, resp, sizeof resp, CLIENT_REPLY_DEADLINE_MS);
    if (n < 0)
        return EXIT_NET;

    printf("%s\n", resp);
    fflush(stdout);

    if (o->hold_sec > 0) {
        LOG_INFO("holding connection open for %ld s", o->hold_sec);
        sleep_sec(o->hold_sec);
    }

    if (strcmp(o->command, PROTO_CMD_QUIT) != 0 && proto_is_ok(resp))
        send_quit(fd);
    else if (strcmp(o->command, PROTO_CMD_QUIT) != 0 && proto_is_err(resp)) {
        char code[64];
        /* REQUEST_TOO_LONG / TIMEOUT close the connection server side. */
        if (proto_get_field(resp, "code", code, sizeof code) &&
            strcmp(code, "REQUEST_TOO_LONG") != 0 &&
            strcmp(code, "TIMEOUT") != 0 &&
            strcmp(code, "SERVER_BUSY") != 0)
            send_quit(fd);
    }

    return proto_is_ok(resp) ? EXIT_OK : EXIT_SERVER_ERR;
}

static int run_interactive(int fd)
{
    char line[4096];
    char resp[MAX_RESPONSE_LEN + 1];
    int saw_err = 0;
    int conn_open = 1;

    if (isatty(STDIN_FILENO))
        fprintf(stderr, "Interactive mode. Type commands (PING, GET ALL, QUIT) ...\n");

    while (conn_open && fgets(line, sizeof line, stdin) != NULL) {
        size_t len = strlen(line);
        int n;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;

        if (send_command(fd, line) != NET_OK) {
            LOG_ERROR("send failed errno=%d (%s)", errno, strerror(errno));
            return EXIT_NET;
        }
        n = recv_response(fd, resp, sizeof resp, CLIENT_REPLY_DEADLINE_MS);
        if (n < 0)
            return saw_err ? EXIT_SERVER_ERR : EXIT_NET;
        printf("%s\n", resp);
        fflush(stdout);

        if (proto_is_err(resp))
            saw_err = 1;
        if (strcmp(line, PROTO_CMD_QUIT) == 0)
            conn_open = 0;
    }

    if (conn_open)
        send_quit(fd);                       /* stdin EOF: be polite */
    return saw_err ? EXIT_SERVER_ERR : EXIT_OK;
}

int main(int argc, char **argv)
{
    struct client_opts o;
    int fd, rc;

    if (parse_args(argc, argv, &o) != 0) {
        usage(argv[0], stderr);
        return EXIT_USAGE;
    }
    log_init(o.debug);

    fd = net_connect(o.host, o.port);
    if (fd < 0) {
        LOG_ERROR("connect to %s:%u failed errno=%d (%s)", o.host,
                  (unsigned)o.port, errno, strerror(errno));
        return EXIT_NET;
    }
    LOG_DEBUG("connected to %s:%u fd=%d", o.host, (unsigned)o.port, fd);

    if (o.idle_sec > 0)
        rc = run_idle(fd, o.idle_sec);
    else if (o.command != NULL)
        rc = run_single(fd, &o);
    else
        rc = run_interactive(fd);

    close(fd);
    LOG_DEBUG("connection closed exit=%d", rc);
    return rc;
}
