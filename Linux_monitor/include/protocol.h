/*
 * protocol.h - text protocol constants and formatting helpers shared by
 * client and server (part of libcommon.so).
 *
 *   Request  : one ASCII line, upper-case keyword(s), terminated by LF.
 *   Success  : "OK key=value key=value ...\n"
 *   Failure  : "ERR code=ERROR_CODE detail=SHORT_TOKEN\n"
 */
#ifndef LSM_PROTOCOL_H
#define LSM_PROTOCOL_H

#include <stddef.h>
#include "common.h"

#define PROTO_OK_PREFIX   "OK"
#define PROTO_ERR_PREFIX  "ERR"

/* Command keywords */
#define PROTO_CMD_PING     "PING"
#define PROTO_CMD_GET      "GET"
#define PROTO_CMD_QUIT     "QUIT"
#define PROTO_CMD_DEMO     "DEMO"
#define PROTO_ARG_ALL      "ALL"
#define PROTO_ARG_CPU      "CPU"
#define PROTO_ARG_MEMORY   "MEMORY"
#define PROTO_ARG_UPTIME   "UPTIME"
#define PROTO_ARG_HOSTNAME "HOSTNAME"
#define PROTO_ARG_WAIT     "WAIT"

/* Error codes (SPEC 5.3) */
enum proto_error {
    PROTO_ERR_BAD_COMMAND = 0,
    PROTO_ERR_INVALID_ARGUMENT,
    PROTO_ERR_REQUEST_TOO_LONG,
    PROTO_ERR_TIMEOUT,
    PROTO_ERR_INTERNAL_ERROR,
    PROTO_ERR_SERVER_BUSY,
    PROTO_ERR_DEMO_DISABLED,
    PROTO_ERR_COUNT
};

/* Stable string for an error code, e.g. "BAD_COMMAND". Unknown -> "INTERNAL_ERROR". */
const char *proto_error_name(int code);

/*
 * proto_format_ok - build "OK <fmt...>\n" into buf.
 * Returns the total length (including LF) or -1 when the result would not
 * fit into min(cap, MAX_RESPONSE_LEN) bytes (plus NUL).  Nothing is
 * truncated silently (SPEC NFR-07).
 */
int proto_format_ok(char *buf, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/*
 * proto_format_err - build "ERR code=<code> detail=<detail>\n" into buf.
 * detail is sanitised (no spaces / control chars). Returns length or -1.
 */
int proto_format_err(char *buf, size_t cap, int code, const char *detail);

/* Convenience wrappers that format and send through net_write_all().
 * Return NET_OK / NET_* (see net.h); -1 if formatting failed. */
int proto_send_ok(int fd, int deadline_ms, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int proto_send_err(int fd, int deadline_ms, int code, const char *detail);

/* Classification of a received response line (without LF). */
int proto_is_ok(const char *line);   /* "OK" or "OK ..."  */
int proto_is_err(const char *line);  /* "ERR" or "ERR ..." */

/*
 * proto_sanitize_value - in place: replace whitespace and control
 * characters with '_' so the value is a single token (SPEC 5.3).
 */
void proto_sanitize_value(char *s);

/*
 * proto_get_field - find "key=" in line and copy its value into out.
 * Returns 1 if found, 0 otherwise.
 */
int proto_get_field(const char *line, const char *key, char *out, size_t cap);

#endif /* LSM_PROTOCOL_H */
