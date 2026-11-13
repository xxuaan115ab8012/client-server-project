/*
 * protocol.c - response formatting helpers (libcommon.so)
 */
#include "protocol.h"
#include "net.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const k_error_names[PROTO_ERR_COUNT] = {
    "BAD_COMMAND",
    "INVALID_ARGUMENT",
    "REQUEST_TOO_LONG",
    "TIMEOUT",
    "INTERNAL_ERROR",
    "SERVER_BUSY",
    "DEMO_DISABLED"
};

const char *proto_error_name(int code)
{
    if (code < 0 || code >= PROTO_ERR_COUNT)
        return k_error_names[PROTO_ERR_INTERNAL_ERROR];
    return k_error_names[code];
}

/* Effective capacity: never produce more than MAX_RESPONSE_LEN bytes. */
static size_t effective_cap(size_t cap)
{
    return cap > MAX_RESPONSE_LEN + 1 ? MAX_RESPONSE_LEN + 1 : cap;
}

static int vformat_ok(char *buf, size_t cap, const char *fmt, va_list ap)
{
    size_t ecap;
    int n, m;

    if (buf == NULL || cap < 4)
        return -1;
    ecap = effective_cap(cap);

    n = snprintf(buf, ecap, "%s ", PROTO_OK_PREFIX);
    if (n < 0 || (size_t)n >= ecap)
        return -1;
    m = vsnprintf(buf + n, ecap - (size_t)n, fmt, ap);
    if (m < 0)
        return -1;
    n += m;
    /* need room for LF + NUL; total line incl. LF must be <= ecap - 1 */
    if ((size_t)n + 1 >= ecap)
        return -1;
    buf[n++] = '\n';
    buf[n] = '\0';
    return n;
}

int proto_format_ok(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vformat_ok(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int proto_format_err(char *buf, size_t cap, int code, const char *detail)
{
    char tok[64];
    size_t ecap;
    int n;

    if (buf == NULL || cap < 4)
        return -1;
    ecap = effective_cap(cap);

    snprintf(tok, sizeof tok, "%s", (detail && *detail) ? detail : "none");
    proto_sanitize_value(tok);

    n = snprintf(buf, ecap, "%s code=%s detail=%s\n",
                 PROTO_ERR_PREFIX, proto_error_name(code), tok);
    if (n < 0 || (size_t)n >= ecap)
        return -1;
    return n;
}

int proto_send_ok(int fd, int deadline_ms, const char *fmt, ...)
{
    char buf[MAX_RESPONSE_LEN + 1];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vformat_ok(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    return net_write_all(fd, buf, (size_t)n, deadline_ms);
}

int proto_send_err(int fd, int deadline_ms, int code, const char *detail)
{
    char buf[256];
    int n = proto_format_err(buf, sizeof buf, code, detail);
    if (n < 0)
        return -1;
    return net_write_all(fd, buf, (size_t)n, deadline_ms);
}

static int has_prefix_word(const char *line, const char *word)
{
    size_t len = strlen(word);
    if (line == NULL)
        return 0;
    if (strncmp(line, word, len) != 0)
        return 0;
    return line[len] == '\0' || line[len] == ' ' ||
           line[len] == '\r' || line[len] == '\n';
}

int proto_is_ok(const char *line)
{
    return has_prefix_word(line, PROTO_OK_PREFIX);
}

int proto_is_err(const char *line)
{
    return has_prefix_word(line, PROTO_ERR_PREFIX);
}

void proto_sanitize_value(char *s)
{
    if (s == NULL)
        return;
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c <= 0x20 || c == 0x7f)
            *s = '_';
    }
}

int proto_get_field(const char *line, const char *key, char *out, size_t cap)
{
    size_t klen;
    const char *p;

    if (line == NULL || key == NULL || out == NULL || cap == 0)
        return 0;
    out[0] = '\0';
    klen = strlen(key);
    p = line;
    while ((p = strstr(p, key)) != NULL) {
        int at_start = (p == line) || (p[-1] == ' ');
        if (at_start && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t i = 0;
            while (v[i] != '\0' && v[i] != ' ' && v[i] != '\n' &&
                   v[i] != '\r' && i + 1 < cap) {
                out[i] = v[i];
                i++;
            }
            out[i] = '\0';
            return 1;
        }
        p += klen;
    }
    return 0;
}
