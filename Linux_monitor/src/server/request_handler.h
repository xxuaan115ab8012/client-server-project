/*
 * request_handler.h - per-connection command loop (server child side)
 */
#ifndef LSM_REQUEST_HANDLER_H
#define LSM_REQUEST_HANDLER_H

struct handler_opts {
    int demo_enabled;     /* --demo: allow "DEMO WAIT N" */
};

/*
 * serve_client - run the request/response loop for one accepted
 * connection until QUIT, EOF, timeout or a fatal transport error.
 * The caller still owns (and must close) fd afterwards.
 */
void serve_client(int fd, const struct handler_opts *opts);

#endif /* LSM_REQUEST_HANDLER_H */
