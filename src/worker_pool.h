#ifndef WORKER_POOL_H
#define WORKER_POOL_H

typedef struct {
    int port;
    int backlog; /* per worker: each one gets its own listen socket, and its own queue */
    int worker_count;
    int idle_timeout_seconds;
    int shutdown_fd; /* see event_loop_config_t; -1 for a pool that only stops when killed */
} worker_pool_config_t;

/*
 * How many workers to run when the operator didn't say. Reads
 * HTTP_SERVER_WORKERS if it is set (that's what the tests use to pin a
 * known count), otherwise asks the system how many CPUs are online.
 * Always returns at least 1, and never more than the internal cap.
 */
int worker_pool_worker_count(void);

/*
 * Starts config->worker_count threads, each running its own independent
 * event loop over its own SO_REUSEPORT listen socket on config->port,
 * and blocks until every one of them has exited (which, in practice,
 * means until config->shutdown_fd fires). See worker_pool.c for why the
 * listen socket is per thread rather than shared.
 *
 * Returns 0 if at least one worker ran, -1 if none did: a pool that
 * never managed to listen on anything is a failed startup, while losing
 * some of the workers is degraded but still a serving process.
 */
int worker_pool_run(const worker_pool_config_t *config);

#endif
