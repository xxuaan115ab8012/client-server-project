/*
 * logger.h - two-stage debug logger (part of libcommon.so).
 *
 *   Compile-time : -DENABLE_DEBUG_LOG=1 compiles LOG_DEBUG() in.
 *   Run-time     : log_init(1) (the --debug flag) actually shows them.
 *
 * Output line format (stderr):
 *   [YYYY-MM-DD HH:MM:SS] [LEVEL] [pid=N] message
 */
#ifndef LSM_LOGGER_H
#define LSM_LOGGER_H

#include <stddef.h>
#include "common.h"

enum log_level {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_DEBUG = 2
};

/* Initialise the logger. runtime_debug_enabled != 0 enables DEBUG lines
 * (only effective when ENABLE_DEBUG_LOG was 1 at compile time). */
void log_init(int runtime_debug_enabled);

/* Returns non-zero when DEBUG lines are both compiled in and enabled. */
int log_debug_enabled(void);

/* Format and emit one log line. DEBUG lines are dropped unless enabled.
 * The whole line is emitted with a single write(2) to avoid interleaving
 * between parent and child processes. */
void log_message(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * log_sanitize - copy src into dst (cap bytes incl. NUL) replacing every
 * control / non-printable byte with '.', truncating to at most
 * LOG_SANITIZE_MAX visible characters (appends "..." when truncated).
 * Use this before logging any client-supplied data (log-injection guard).
 * Returns dst.
 */
#define LOG_SANITIZE_MAX 96
char *log_sanitize(const char *src, char *dst, size_t cap);

#define LOG_ERROR(...) log_message(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_INFO(...)  log_message(LOG_LEVEL_INFO,  __VA_ARGS__)

#if ENABLE_DEBUG_LOG
#  define LOG_DEBUG(...) log_message(LOG_LEVEL_DEBUG, __VA_ARGS__)
#else
#  define LOG_DEBUG(...) ((void)0)   /* not compiled in */
#endif

#endif /* LSM_LOGGER_H */
