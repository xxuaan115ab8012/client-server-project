/*
 * logger.c - two-stage debug logger (libcommon.so)
 *
 * Every line is formatted into a local buffer and emitted with one
 * write(2) so that lines from parent and child processes never
 * interleave in the middle of a line.
 */
#include "logger.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int g_runtime_debug = 0;

void log_init(int runtime_debug_enabled)
{
    g_runtime_debug = runtime_debug_enabled ? 1 : 0;
}

int log_debug_enabled(void)
{
#if ENABLE_DEBUG_LOG
    return g_runtime_debug;
#else
    return 0;
#endif
}

static const char *level_name(int level)
{
    switch (level) {
    case LOG_LEVEL_ERROR: return "ERROR";
    case LOG_LEVEL_INFO:  return "INFO";
    case LOG_LEVEL_DEBUG: return "DEBUG";
    default:              return "LOG";
    }
}

static void write_all_stderr(const char *s, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(STDERR_FILENO, s + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        off += (size_t)n;
    }
}

void log_message(int level, const char *fmt, ...)
{
    char line[1024];
    char stamp[32];
    time_t now;
    struct tm tmv;
    int n, saved_errno;
    va_list ap;

    if (level == LOG_LEVEL_DEBUG && !log_debug_enabled())
        return;

    saved_errno = errno;

    now = time(NULL);
    if (localtime_r(&now, &tmv) == NULL ||
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tmv) == 0)
        snprintf(stamp, sizeof stamp, "0000-00-00 00:00:00");

    /* "[INFO]" is one character shorter than "[DEBUG]"/"[ERROR]"; pad so
     * the pid column lines up (matches the SPEC 6.2 example). */
    n = snprintf(line, sizeof line, "[%s] [%s]%s [pid=%ld] ",
                 stamp, level_name(level),
                 level == LOG_LEVEL_INFO ? " " : "",
                 (long)getpid());
    if (n < 0 || (size_t)n >= sizeof line) {
        errno = saved_errno;
        return;
    }

    va_start(ap, fmt);
    {
        int m = vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
        va_end(ap);
        if (m < 0) {
            errno = saved_errno;
            return;
        }
        n += m;
        if ((size_t)n >= sizeof line - 1)
            n = (int)sizeof line - 2;     /* truncated: leave room for LF */
    }
    line[n++] = '\n';
    write_all_stderr(line, (size_t)n);

    errno = saved_errno;
}

char *log_sanitize(const char *src, char *dst, size_t cap)
{
    size_t i = 0, o = 0;
    size_t max_visible = LOG_SANITIZE_MAX;

    if (dst == NULL || cap == 0)
        return dst;
    if (src == NULL) {
        dst[0] = '\0';
        return dst;
    }
    if (max_visible > cap - 1)
        max_visible = cap - 1;

    while (src[i] != '\0' && o < max_visible) {
        unsigned char c = (unsigned char)src[i++];
        dst[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    dst[o] = '\0';

    if (src[i] != '\0' && cap >= 4) {
        /* truncated: mark it */
        size_t pos = (o + 3 < cap) ? o : cap - 4;
        memcpy(dst + pos, "...", 4);
    }
    return dst;
}
