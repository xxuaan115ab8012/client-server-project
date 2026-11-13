/*
 * server.c - Linux System Monitor TCP server (parent process)
 *
 *   listen_fd = socket/bind/listen
 *   loop: client_fd = accept()
 *         block SIGCHLD -> fork()
 *           child : restore signals, close(listen_fd), serve, _exit()
 *           parent: record pid, close(client_fd), unblock SIGCHLD
 *
 * No threads are used (SPEC FR-03).  Robustness features (SPEC 8):
 *   - SIGCHLD handler is async-signal-safe: waitpid(WNOHANG) loop only.
 *   - SIGPIPE ignored; libcommon also uses MSG_NOSIGNAL.
 *   - accept() EINTR retry, fork() failure handling, MAX_CLIENTS limit.
 *   - SIGINT/SIGTERM -> graceful shutdown with child reaping.
 */
#include "common.h"
#include "logger.h"
#include "net.h"
#include "protocol.h"
#include "request_handler.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct server_opts {
    const char *host;
    unsigned short port;
    int debug;
    int demo;
};

/* ---- state shared with signal handlers (async-signal-safe types) ---- */
static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_active_children = 0;
static volatile pid_t g_child_pids[MAX_CLIENTS];

static void child_table_remove(pid_t pid)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_child_pids[i] == pid) {
            g_child_pids[i] = 0;
            return;
        }
    }
}

static int child_table_add(pid_t pid)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_child_pids[i] == 0) {
            g_child_pids[i] = pid;
            return 0;
        }
    }
    return -1;
}

#if !BASELINE_NO_REAP
/* Only async-signal-safe calls here (SPEC 8): waitpid + memory updates. */
static void on_sigchld(int signo)
{
    int saved_errno = errno;
    pid_t pid;
    (void)signo;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        child_table_remove(pid);
        if (g_active_children > 0)
            g_active_children--;
    }
    errno = saved_errno;
}
#endif

static void on_terminate(int signo)
{
    (void)signo;
    g_shutdown = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    /* SIGPIPE: ignore so a dead peer cannot kill the process (RB-05). */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) != 0)
        return -1;

#if !BASELINE_NO_REAP
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;        /* no SA_RESTART: accept() sees EINTR and retries */
    if (sigaction(SIGCHLD, &sa, NULL) != 0)
        return -1;
#endif

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_terminate;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                   /* interrupt accept() */
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;
    return 0;
}

static void usage(const char *prog, FILE *out)
{
    fprintf(out,
        "Usage: %s [--host ADDR] [--port N] [--debug] [--demo] [--help]\n"
        "  --host ADDR   bind address (default %s)\n"
        "  --port N      TCP port 1-65535 (default %d)\n"
        "  --debug       show DEBUG log lines (only if built with DEBUG=1)\n"
        "  --demo        enable the 'DEMO WAIT N' test command (N=1..5)\n"
        "  --help        show this help\n",
        prog, DEFAULT_HOST, DEFAULT_PORT);
}

static int parse_port(const char *s, unsigned short *out)
{
    char *end = NULL;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535)
        return -1;
    *out = (unsigned short)v;
    return 0;
}

static int parse_args(int argc, char **argv, struct server_opts *o)
{
    o->host = DEFAULT_HOST;
    o->port = DEFAULT_PORT;
    o->debug = 0;
    o->demo = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0], stdout);
            exit(0);
        } else if (strcmp(a, "--debug") == 0) {
            o->debug = 1;
        } else if (strcmp(a, "--demo") == 0) {
            o->demo = 1;
        } else if (strcmp(a, "--host") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "error: --host requires a value\n");
                return -1;
            }
            o->host = argv[i];
        } else if (strcmp(a, "--port") == 0) {
            if (++i >= argc || parse_port(argv[i], &o->port) != 0) {
                fprintf(stderr, "error: --port requires a number 1-65535\n");
                return -1;
            }
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            usage(argv[0], stderr);
            return -1;
        }
    }
    return 0;
}

/* Best-effort SERVER_BUSY reply (SPEC RB-09); never blocks for long. */
static void reject_busy(int fd)
{
    (void)proto_send_err(fd, BUSY_REPLY_DEADLINE_MS, PROTO_ERR_SERVER_BUSY,
                         "max_clients");
}

/* ---- child side -------------------------------------------------------- */
static void run_child(int listen_fd, int client_fd,
                      const struct server_opts *o, const sigset_t *orig_mask)
{
    struct sigaction sa;
    struct handler_opts ho;
    char peer[64];

    /* Restore default dispositions (SIGPIPE stays ignored) and unblock. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigprocmask(SIG_SETMASK, orig_mask, NULL);

    close(listen_fd);                       /* child does not accept */

    if (net_peer_string(client_fd, peer, sizeof peer) != 0)
        snprintf(peer, sizeof peer, "unknown");
    LOG_INFO("child started peer=%s fd=%d", peer, client_fd);

    ho.demo_enabled = o->demo;
    serve_client(client_fd, &ho);

    close(client_fd);
    LOG_INFO("child exited peer=%s", peer);
    _exit(0);
}

/* ---- shutdown ---------------------------------------------------------- */
static void shutdown_children(void)
{
    int remaining = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        pid_t pid = g_child_pids[i];
        if (pid > 0) {
            kill(pid, SIGTERM);
            remaining++;
        }
    }
    LOG_INFO("shutdown: signalled %d child process(es)", remaining);

    /* Reap everything we still own (blocking waitpid is fine here). */
    for (;;) {
        pid_t pid = waitpid(-1, NULL, 0);
        if (pid > 0) {
            child_table_remove(pid);
            continue;
        }
        if (pid < 0 && errno == EINTR)
            continue;
        break;                               /* ECHILD: nothing left */
    }
}

int main(int argc, char **argv)
{
    struct server_opts o;
    sigset_t block_chld, orig_mask;
    int listen_fd;

    if (parse_args(argc, argv, &o) != 0)
        return 2;

    log_init(o.debug);

    if (install_signal_handlers() != 0) {
        LOG_ERROR("sigaction failed errno=%d", errno);
        return 1;
    }

    listen_fd = net_listen(o.host, o.port, LISTEN_BACKLOG);
    if (listen_fd < 0) {
        LOG_ERROR("listen failed addr=%s:%u errno=%d (%s)", o.host,
                  (unsigned)o.port, errno, strerror(errno));
        return 1;
    }

    LOG_INFO("server listening addr=%s:%u demo=%d debug_compiled=%d "
             "debug_runtime=%d baseline=%d max_clients=%d",
             o.host, (unsigned)o.port, o.demo, ENABLE_DEBUG_LOG, o.debug,
             BASELINE_MODE, MAX_CLIENTS);

    sigemptyset(&block_chld);
    sigaddset(&block_chld, SIGCHLD);

    while (!g_shutdown) {
        struct sockaddr_storage ss;
        socklen_t slen = sizeof ss;
        int client_fd;
        pid_t pid;

        client_fd = accept(listen_fd, (struct sockaddr *)&ss, &slen);
        if (client_fd < 0) {
            if (errno == EINTR) {
                LOG_DEBUG("accept interrupted by signal, retrying");
                continue;                   /* SPEC 8: retry on EINTR  */
            }
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
                errno == ENOMEM) {
                LOG_ERROR("accept failed errno=%d (%s), backing off",
                          errno, strerror(errno));
                sleep(1);
                continue;
            }
            LOG_ERROR("accept failed errno=%d (%s)", errno, strerror(errno));
            continue;
        }
        LOG_DEBUG("accepted client fd=%d active_children=%d", client_fd,
                  (int)g_active_children);

        if (g_active_children >= MAX_CLIENTS) {
            LOG_ERROR("server busy: active_children=%d >= %d, rejecting fd=%d",
                      (int)g_active_children, MAX_CLIENTS, client_fd);
            reject_busy(client_fd);
            close(client_fd);
            continue;
        }

        /* Critical section: SIGCHLD must not race with the counter. */
        sigprocmask(SIG_BLOCK, &block_chld, &orig_mask);
        pid = fork();
        if (pid < 0) {
            int e = errno;
            sigprocmask(SIG_SETMASK, &orig_mask, NULL);
            LOG_ERROR("fork failed errno=%d (%s); dropping fd=%d", e,
                      strerror(e), client_fd);
            (void)proto_send_err(client_fd, BUSY_REPLY_DEADLINE_MS,
                                 PROTO_ERR_SERVER_BUSY, "fork_failed");
            close(client_fd);
            continue;
        }
        if (pid == 0) {
            run_child(listen_fd, client_fd, &o, &orig_mask); /* never returns */
        }

        /* parent */
        g_active_children++;
        if (child_table_add(pid) != 0)
            LOG_ERROR("child table full; pid=%ld untracked", (long)pid);
        close(client_fd);                   /* parent must not keep it */
        sigprocmask(SIG_SETMASK, &orig_mask, NULL);
        LOG_INFO("forked child pid=%ld fd=%d active_children=%d",
                 (long)pid, client_fd, (int)g_active_children);
    }

    LOG_INFO("shutdown requested, closing listen socket");
    close(listen_fd);
    shutdown_children();
    LOG_INFO("server stopped");
    return 0;
}
