#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

/*
 * Drives the accept/read/respond cycle over listen_fd using a single
 * epoll instance in edge-triggered mode. Blocks the calling thread
 * forever under normal operation. Connections are kept alive across
 * multiple requests when the client and protocol version agree to it
 * (see http_parser_should_keep_alive), and any connection that goes
 * idle for idle_timeout_seconds is closed, see event_loop.c for why
 * that matters against Slowloris-style attacks.
 *
 * listen_fd must already be bound and listening. This function switches
 * it to O_NONBLOCK itself (ET epoll requires every registered fd to be
 * non-blocking, so that's an invariant of this module, not something
 * callers should have to remember). Ownership of the fd's lifetime stays
 * with the caller, this function never closes it.
 *
 * Returns -1 if the loop could not even be set up (epoll_create1/epoll_ctl
 * failure), 0 if it ran and later exited after an unrecoverable
 * epoll_wait error.
 */
int event_loop_run(int listen_fd, int idle_timeout_seconds);

#endif
