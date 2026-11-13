/*
 * common.h - shared constants and build-mode switches for the
 * Linux System Monitor project (client, server and libcommon.so).
 */
#ifndef LSM_COMMON_H
#define LSM_COMMON_H

/* ---- Protocol limits (SPEC NFR-06 / NFR-07) ------------------------- */
#define MAX_REQUEST_LEN      256   /* bytes, including the trailing LF   */
#define MAX_RESPONSE_LEN     2048  /* bytes, including the trailing LF   */

/* ---- Concurrency (SPEC NFR-03) -------------------------------------- */
#define MAX_CLIENTS          64    /* maximum simultaneous child workers */
#define LISTEN_BACKLOG       128

/* ---- Deadlines in milliseconds (SPEC NFR-08) ------------------------ */
#define REQUEST_DEADLINE_MS  5000  /* first complete line after accept   */
#define IDLE_TIMEOUT_MS      10000 /* waiting for the next command        */
#define WRITE_DEADLINE_MS    5000  /* total deadline for one response     */
#define BUSY_REPLY_DEADLINE_MS 1000/* best-effort SERVER_BUSY reply       */
#define CLIENT_REPLY_DEADLINE_MS 15000 /* client waits for a response    */

/* ---- Network defaults (SPEC NFR-04) --------------------------------- */
#define DEFAULT_HOST         "127.0.0.1"
#define DEFAULT_PORT         9090

/* ---- Demo mode (SPEC FR-16) ----------------------------------------- */
#define DEMO_WAIT_MIN_SEC    1
#define DEMO_WAIT_MAX_SEC    5

/* ---- Compile-time debug switch (SPEC 6.1) --------------------------- */
#ifndef ENABLE_DEBUG_LOG
#define ENABLE_DEBUG_LOG     0
#endif

/*
 * ---- Baseline experiment switches (SPEC 12) -------------------------
 * BASELINE_MODE=1 turns on all three "unprotected" behaviours.  Each one
 * can also be enabled individually.  None of them affects memory safety.
 */
#ifndef BASELINE_MODE
#define BASELINE_MODE        0
#endif

#if BASELINE_MODE
#  ifndef BASELINE_NO_VALIDATE
#    define BASELINE_NO_VALIDATE 1   /* unknown command -> close socket   */
#  endif
#  ifndef BASELINE_NO_TIMEOUT
#    define BASELINE_NO_TIMEOUT  1   /* no request / idle deadlines       */
#  endif
#  ifndef BASELINE_NO_REAP
#    define BASELINE_NO_REAP     1   /* no SIGCHLD handler, no waitpid()  */
#  endif
#endif

#ifndef BASELINE_NO_VALIDATE
#define BASELINE_NO_VALIDATE 0
#endif
#ifndef BASELINE_NO_TIMEOUT
#define BASELINE_NO_TIMEOUT  0
#endif
#ifndef BASELINE_NO_REAP
#define BASELINE_NO_REAP     0
#endif

#endif /* LSM_COMMON_H */
