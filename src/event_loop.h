#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

/*
 * Drives the accept/read/respond cycle over `listen_fd` using a single
 * epoll instance in edge-triggered mode. Blocks the calling thread
 * forever under normal operation.
 *
 * `listen_fd` must already be bound and listening; this function switches
 * it to O_NONBLOCK itself (ET epoll requires every registered fd to be
 * non-blocking, so that's an invariant of this module, not something
 * callers should have to remember). Ownership of the fd's lifetime stays
 * with the caller -- this function never closes it.
 *
 * Returns -1 if the loop could not even be set up (epoll_create1/epoll_ctl
 * failure), 0 if it ran and later exited after an unrecoverable
 * epoll_wait error.
 */
int event_loop_run(int listen_fd);

#endif
