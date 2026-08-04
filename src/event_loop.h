#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

/*
 * Everything one event loop needs to know about the outside world. Passed
 * by the caller and never modified by the loop: since layer 6 runs one of
 * these per worker thread, anything the loop would want to keep across
 * iterations lives in its own stack-local state instead of in a global,
 * so two loops running side by side can't see each other at all.
 */
typedef struct {
    /* Already bound and listening. Each worker thread owns its own,
     * bound to the same port with SO_REUSEPORT, see worker_pool.c. */
    int listen_fd;

    int idle_timeout_seconds;

    /* An eventfd that becomes readable exactly once, when the process is
     * asked to stop (see main.c). Every worker registers the same fd
     * level-triggered, so a single write from the signal handler wakes
     * all of them; the loop returns 0 after cleaning up. May be -1, in
     * which case the loop simply has no shutdown path and runs forever.
     * The loop never reads or closes it. */
    int shutdown_fd;

    /* Reported back to clients in the X-Worker response header, which is
     * how tests/test_thread_pool.py can tell which thread answered. */
    int worker_id;
} event_loop_config_t;

/*
 * Drives the accept/read/respond cycle over config->listen_fd using a
 * single epoll instance in edge-triggered mode. Blocks the calling thread
 * until shutdown_fd fires (or forever, if there is none). Connections are
 * kept alive across multiple requests when the client and protocol
 * version agree to it (see http_parser_should_keep_alive), and any
 * connection that goes idle for idle_timeout_seconds is closed, see
 * event_loop.c for why that matters against Slowloris-style attacks.
 *
 * listen_fd must already be bound and listening. This function switches
 * it to O_NONBLOCK itself (ET epoll requires every registered fd to be
 * non-blocking, so that's an invariant of this module, not something
 * callers should have to remember). Ownership of the fd's lifetime stays
 * with the caller, this function never closes it.
 *
 * Reentrant across threads: no static mutable state is involved, so any
 * number of threads may run this concurrently as long as each brings its
 * own listen_fd. Connections accepted by one loop are only ever touched
 * by that loop.
 *
 * Returns -1 if the loop could not even be set up (epoll_create1/epoll_ctl
 * failure), 0 if it ran and later exited, whether because it was asked to
 * shut down or after an unrecoverable epoll_wait error.
 */
int event_loop_run(const event_loop_config_t *config);

#endif
