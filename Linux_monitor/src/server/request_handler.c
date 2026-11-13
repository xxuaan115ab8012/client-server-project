/*
 * request_handler.c - parse client commands, dispatch, build responses
 *
 * Runs inside the forked child.  Uses only the libcommon API for I/O so
 * that the .so is genuinely exercised (SPEC 7.1).
 */
#include "request_handler.h"

#include "common.h"
#include "logger.h"
#include "net.h"
#include "protocol.h"
#include "system_info.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Result of handling one request line. */
enum action {
    ACT_KEEP,          /* response sent, keep connection            */
    ACT_CLOSE,         /* response sent (or not), close connection  */
};

struct request {
    char *argv[8];
    int argc;
};

/* Split a line on single spaces/tabs into at most 8 tokens (in place). */
static void tokenize(char *line, struct request *req)
{
    char *save = NULL;
    char *tok;

    req->argc = 0;
    for (tok = strtok_r(line, " \t", &save);
         tok != NULL && req->argc < 8;
         tok = strtok_r(NULL, " \t", &save)) {
        req->argv[req->argc++] = tok;
    }
}

/* Parse a strictly decimal integer in [lo, hi]. Returns 1 ok / 0 bad. */
static int parse_int_range(const char *s, long lo, long hi, long *out)
{
    char *end = NULL;
    long v;

    if (s == NULL || *s == '\0')
        return 0;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return 0;
    if (v < lo || v > hi)
        return 0;
    *out = v;
    return 1;
}

static void sleep_whole_seconds(long sec)
{
    struct timespec ts;
    ts.tv_sec = sec;
    ts.tv_nsec = 0;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

/* ---- individual handlers: fill resp, return 0 ok / -1 internal error --- */

static int build_cpu(char *resp, size_t cap)
{
    struct sysmon_cpu c;
    if (sysmon_get_cpu(&c) != 0) {
        LOG_ERROR("system_info: cpu read failed errno=%d", errno);
        return proto_format_err(resp, cap, PROTO_ERR_INTERNAL_ERROR,
                                "cpu_unavailable");
    }
    return proto_format_ok(resp, cap, "cpu_cores=%ld cpu_usage_pct=%.1f",
                           c.cores, c.usage_pct);
}

static int build_memory(char *resp, size_t cap)
{
    struct sysmon_memory m;
    if (sysmon_get_memory(&m) != 0) {
        LOG_ERROR("system_info: memory read failed errno=%d", errno);
        return proto_format_err(resp, cap, PROTO_ERR_INTERNAL_ERROR,
                                "memory_unavailable");
    }
    return proto_format_ok(resp, cap, "mem_total_kb=%llu mem_available_kb=%llu",
                           m.total_kb, m.available_kb);
}

static int build_uptime(char *resp, size_t cap)
{
    long long up;
    if (sysmon_get_uptime(&up) != 0) {
        LOG_ERROR("system_info: uptime read failed errno=%d", errno);
        return proto_format_err(resp, cap, PROTO_ERR_INTERNAL_ERROR,
                                "uptime_unavailable");
    }
    return proto_format_ok(resp, cap, "uptime_sec=%lld", up);
}

static int build_hostname(char *resp, size_t cap)
{
    struct sysmon_host h;
    if (sysmon_get_host(&h) != 0) {
        LOG_ERROR("system_info: host read failed errno=%d", errno);
        return proto_format_err(resp, cap, PROTO_ERR_INTERNAL_ERROR,
                                "hostname_unavailable");
    }
    return proto_format_ok(resp, cap, "hostname=%s", h.hostname);
}

static int build_all(char *resp, size_t cap)
{
    struct sysmon_host h;
    struct sysmon_cpu c;
    struct sysmon_memory m;
    long long up;

    if (sysmon_get_host(&h) != 0 || sysmon_get_cpu(&c) != 0 ||
        sysmon_get_memory(&m) != 0 || sysmon_get_uptime(&up) != 0) {
        LOG_ERROR("system_info: GET ALL failed errno=%d", errno);
        return proto_format_err(resp, cap, PROTO_ERR_INTERNAL_ERROR,
                                "system_info_unavailable");
    }
    return proto_format_ok(resp, cap,
                           "hostname=%s os=%s kernel=%s cpu_cores=%ld "
                           "cpu_usage_pct=%.1f mem_total_kb=%llu "
                           "mem_available_kb=%llu uptime_sec=%lld",
                           h.hostname, h.os, h.kernel, c.cores, c.usage_pct,
                           m.total_kb, m.available_kb, up);
}

/*
 * dispatch - handle one request line.
 * Writes the response into resp (len returned via *resp_len, 0 = send
 * nothing) and returns ACT_KEEP / ACT_CLOSE.
 */
static enum action dispatch(char *line, const struct handler_opts *opts,
                            char *resp, size_t cap, int *resp_len)
{
    struct request req;
    const char *cmd;

    *resp_len = 0;
    tokenize(line, &req);

    if (req.argc == 0) {
#if BASELINE_NO_VALIDATE
        LOG_INFO("baseline: empty request, closing connection");
        return ACT_CLOSE;
#else
        *resp_len = proto_format_err(resp, cap, PROTO_ERR_BAD_COMMAND,
                                     "empty_request");
        return ACT_KEEP;
#endif
    }
    cmd = req.argv[0];

    if (strcmp(cmd, PROTO_CMD_PING) == 0) {
        if (req.argc != 1) {
            *resp_len = proto_format_err(resp, cap, PROTO_ERR_INVALID_ARGUMENT,
                                         "unexpected_argument");
            return ACT_KEEP;
        }
        *resp_len = proto_format_ok(resp, cap, "pong=1");
        return ACT_KEEP;
    }

    if (strcmp(cmd, PROTO_CMD_QUIT) == 0) {
        *resp_len = proto_format_ok(resp, cap, "bye=1");
        return ACT_CLOSE;
    }

    if (strcmp(cmd, PROTO_CMD_GET) == 0) {
        const char *what;
        if (req.argc < 2) {
            *resp_len = proto_format_err(resp, cap, PROTO_ERR_INVALID_ARGUMENT,
                                         "missing_target");
            return ACT_KEEP;
        }
        if (req.argc > 2) {
            *resp_len = proto_format_err(resp, cap, PROTO_ERR_INVALID_ARGUMENT,
                                         "unexpected_argument");
            return ACT_KEEP;
        }
        what = req.argv[1];
        if (strcmp(what, PROTO_ARG_ALL) == 0)
            *resp_len = build_all(resp, cap);
        else if (strcmp(what, PROTO_ARG_CPU) == 0)
            *resp_len = build_cpu(resp, cap);
        else if (strcmp(what, PROTO_ARG_MEMORY) == 0)
            *resp_len = build_memory(resp, cap);
        else if (strcmp(what, PROTO_ARG_UPTIME) == 0)
            *resp_len = build_uptime(resp, cap);
        else if (strcmp(what, PROTO_ARG_HOSTNAME) == 0)
            *resp_len = build_hostname(resp, cap);
        else {
#if BASELINE_NO_VALIDATE
            LOG_INFO("baseline: unknown GET target, closing connection");
            return ACT_CLOSE;
#else
            *resp_len = proto_format_err(resp, cap, PROTO_ERR_BAD_COMMAND,
                                         "unknown_target");
            return ACT_KEEP;
#endif
        }
        if (*resp_len < 0) {
            /* response could not be formatted within MAX_RESPONSE_LEN */
            LOG_ERROR("response formatting failed (too long)");
            *resp_len = proto_format_err(resp, cap, PROTO_ERR_INTERNAL_ERROR,
                                         "response_too_long");
        }
        return ACT_KEEP;
    }

    if (strcmp(cmd, PROTO_CMD_DEMO) == 0) {
        long n = 0;
        if (!opts->demo_enabled) {
            *resp_len = proto_format_err(resp, cap, PROTO_ERR_DEMO_DISABLED,
                                         "demo_mode_off");
            return ACT_KEEP;
        }
        if (req.argc < 2 || strcmp(req.argv[1], PROTO_ARG_WAIT) != 0) {
#if BASELINE_NO_VALIDATE
            LOG_INFO("baseline: unknown DEMO subcommand, closing connection");
            return ACT_CLOSE;
#else
            *resp_len = proto_format_err(resp, cap, PROTO_ERR_BAD_COMMAND,
                                         "unknown_subcommand");
            return ACT_KEEP;
#endif
        }
        if (req.argc != 3 ||
            !parse_int_range(req.argv[2], DEMO_WAIT_MIN_SEC,
                             DEMO_WAIT_MAX_SEC, &n)) {
            *resp_len = proto_format_err(resp, cap, PROTO_ERR_INVALID_ARGUMENT,
                                         "wait_seconds_1_to_5");
            return ACT_KEEP;
        }
        LOG_DEBUG("demo wait start sec=%ld", n);
        sleep_whole_seconds(n);
        LOG_DEBUG("demo wait done sec=%ld", n);
        *resp_len = proto_format_ok(resp, cap, "waited_sec=%ld", n);
        return ACT_KEEP;
    }

    /* Unknown command */
#if BASELINE_NO_VALIDATE
    LOG_INFO("baseline: unknown command, closing connection without reply");
    return ACT_CLOSE;
#else
    *resp_len = proto_format_err(resp, cap, PROTO_ERR_BAD_COMMAND,
                                 "unknown_command");
    return ACT_KEEP;
#endif
}

void serve_client(int fd, const struct handler_opts *opts)
{
    char req[MAX_REQUEST_LEN + 1];       /* line incl. LF <= 256 bytes */
    char resp[MAX_RESPONSE_LEN + 1];
    char safe[LOG_SANITIZE_MAX + 8];
    int first = 1;
    int requests = 0;

    (void)safe;   /* only referenced by LOG_DEBUG (may be compiled out) */

    for (;;) {
        int deadline_ms;
        int n;
        int resp_len = 0;
        enum action act;

#if BASELINE_NO_TIMEOUT
        deadline_ms = -1;                /* baseline: wait forever */
#else
        deadline_ms = first ? REQUEST_DEADLINE_MS : IDLE_TIMEOUT_MS;
#endif
        LOG_DEBUG("waiting for request deadline_ms=%d", deadline_ms);

        n = net_readline(fd, req, sizeof req, deadline_ms);
        if (n == NET_EOF) {
            LOG_INFO("client closed connection requests=%d", requests);
            break;
        }
        if (n == NET_TIMEOUT) {
            LOG_INFO("%s timeout after %d ms, closing",
                     first ? "request" : "idle", deadline_ms);
            (void)proto_send_err(fd, BUSY_REPLY_DEADLINE_MS, PROTO_ERR_TIMEOUT,
                                 first ? "request_deadline" : "idle_timeout");
            break;
        }
        if (n == NET_TOO_LONG) {
            LOG_INFO("request exceeds %d bytes, closing", MAX_REQUEST_LEN);
            (void)proto_send_err(fd, BUSY_REPLY_DEADLINE_MS,
                                 PROTO_ERR_REQUEST_TOO_LONG, "max_256_bytes");
            break;
        }
        if (n < 0) {
            LOG_ERROR("read failed rc=%s errno=%d", net_result_name(n), errno);
            break;
        }

        first = 0;
        requests++;
        LOG_DEBUG("request=%s", log_sanitize(req, safe, sizeof safe));

        act = dispatch(req, opts, resp, sizeof resp, &resp_len);

        if (resp_len > 0) {
            int rc = net_write_all(fd, resp, (size_t)resp_len,
                                   WRITE_DEADLINE_MS);
            if (rc != NET_OK) {
                LOG_ERROR("write failed rc=%s errno=%d",
                          net_result_name(rc), errno);
                break;
            }
            LOG_DEBUG("response=%s", log_sanitize(resp, safe, sizeof safe));
        } else if (resp_len < 0) {
            LOG_ERROR("could not format response; closing");
            break;
        }

        if (act == ACT_CLOSE)
            break;
    }
}
