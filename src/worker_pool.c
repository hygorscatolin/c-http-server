/* SO_REUSEPORT lives behind __USE_MISC in <sys/socket.h>, and -std=c11
 * (which defines __STRICT_ANSI__) hides it along with the rest of the
 * non-standard socket options. Same reason event_loop.c needs it. */
#define _GNU_SOURCE

#include "worker_pool.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "event_loop.h"

/* Not a scaling limit, a sanity limit: it only bounds what a typo in
 * HTTP_SERVER_WORKERS (or a machine reporting something absurd) can turn
 * into thread stacks and listen sockets. Past the CPU count extra workers
 * stop buying throughput anyway, they just add scheduler contention. */
#define MAX_WORKERS 64

/* One per thread. Each worker only ever writes to its own slot, so the
 * array is shared but no two threads touch the same object, which is
 * what the C11 memory model requires (distinct array elements are
 * distinct memory locations, see 3.14 and 5.1.2.4). The config it points
 * at is read-only for the entire life of the pool. */
typedef struct {
    pthread_t thread;
    int id;
    const worker_pool_config_t *config;
    bool started; /* pthread_create succeeded, so this one must be joined */
    bool served;  /* got as far as a listening socket and a running event loop */
} worker_t;

/*
 * One listening socket per thread, all bound to the same port, instead of
 * one shared socket that every thread accept()s from.
 *
 * With a shared listen socket, the accept queue is a single object that
 * N threads contend on. The classic shapes that takes are all bad: a
 * thundering herd, where every thread wakes on a connection and all but
 * one go back to sleep having done nothing; or one designated acceptor
 * thread handing fds to workers through a queue, which needs a mutex and
 * a condition variable on the hottest path in the server and makes that
 * one thread the ceiling on connection rate.
 *
 * SO_REUSEPORT (Linux 3.9+) removes the contention instead of managing
 * it. Several sockets may bind the same address as long as every one of
 * them sets the option, and the kernel then hashes each incoming
 * connection's 4-tuple to pick exactly one of them to queue it on. So:
 *
 *   - each worker has a private accept queue, and wakes only for
 *     connections that are already its own, so no herd and no lock;
 *   - the balancing happens in the kernel, at the point where it already
 *     had to make a decision anyway, so it costs nothing extra;
 *   - a connection is stable: every packet of that 4-tuple hashes the
 *     same way, so a keep-alive connection stays with the worker that
 *     accepted it, which is exactly the invariant event_loop.c relies on
 *     when it keeps per-connection state with no locking at all.
 *
 * The price is that balancing is per connection and hash-based, not per
 * load: a worker that draws a long-lived, heavy connection keeps it, and
 * the kernel will not rebalance. For short HTTP requests that averages
 * out; for a server with a few very expensive connections it would not,
 * and work stealing between loops would be the answer.
 *
 * A note on correctness rather than performance: every socket must set
 * SO_REUSEPORT *before* bind(), and all of them must belong to the same
 * effective UID, which is the kernel's defense against another user
 * hijacking a port by joining an existing group.
 */
static int create_listen_socket(int port, int backlog, int worker_id) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* Without this, restarting the server right after it exits fails with
     * EADDRINUSE while the previous socket sits in TIME_WAIT. */
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        /* Worth its own message: this is the one failure that means the
         * whole layer-6 design is unavailable (a kernel older than 3.9,
         * or a sandbox that filters the option) rather than a routine
         * startup error. */
        fprintf(stderr, "worker %d: setsockopt(SO_REUSEPORT): %s\n", worker_id, strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)port),
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    /* The backlog is per socket, so the process as a whole can queue
     * worker_count * backlog pending connections. That is a feature of
     * the design, not double counting: a burst is spread over N queues
     * by the same hash that spreads the connections themselves. */
    if (listen(fd, backlog) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

/* The whole body of a worker thread: build a socket, run the loop on it,
 * clean up. Nothing here is shared with the other workers except the
 * read-only config and the shutdown fd, so there is no synchronization
 * in this function and none is missing from it. */
static void *worker_main(void *arg) {
    worker_t *worker = arg;

    int listen_fd = create_listen_socket(worker->config->port, worker->config->backlog, worker->id);
    if (listen_fd < 0) {
        /* This thread is out, the others are unaffected: they have their
         * own sockets, already bound. worker_pool_run reports the pool
         * as degraded when it joins. */
        fprintf(stderr, "worker %d: could not listen, this thread is giving up\n", worker->id);
        return NULL;
    }

    event_loop_config_t loop_config = {
        .listen_fd = listen_fd,
        .idle_timeout_seconds = worker->config->idle_timeout_seconds,
        .shutdown_fd = worker->config->shutdown_fd,
        .worker_id = worker->id,
    };

    if (event_loop_run(&loop_config) < 0) {
        fprintf(stderr, "worker %d: event loop failed to start\n", worker->id);
    } else {
        worker->served = true;
    }

    close(listen_fd);
    return NULL;
}

/* _SC_NPROCESSORS_ONLN counts the CPUs the *system* has online, which is
 * not always the number this process may actually use: under a cpuset,
 * a CPU affinity mask or a container CPU quota it can be a large
 * overcount, and sched_getaffinity() (or the cgroup quota) is what a
 * container-aware server would consult instead. Kept as specified here,
 * with HTTP_SERVER_WORKERS as the escape hatch when the answer is wrong. */
static int online_cpu_count(void) {
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online < 1) {
        /* sysconf returning -1 without setting errno means "no limit
         * defined", which for a CPU count means "the system won't say".
         * One worker is the honest fallback. */
        fprintf(stderr, "could not determine the number of online CPUs, running a single worker\n");
        return 1;
    }
    if (online > MAX_WORKERS) {
        return MAX_WORKERS;
    }
    return (int)online;
}

int worker_pool_worker_count(void) {
    const char *raw = getenv("HTTP_SERVER_WORKERS");
    if (raw == NULL || raw[0] == '\0') {
        return online_cpu_count();
    }

    char *end = NULL;
    long value = strtol(raw, &end, 10);
    if (end == raw || *end != '\0' || value < 1 || value > MAX_WORKERS) {
        fprintf(stderr, "ignoring invalid HTTP_SERVER_WORKERS=%s (expected 1..%d), detecting instead\n", raw,
                MAX_WORKERS);
        return online_cpu_count();
    }
    return (int)value;
}

int worker_pool_run(const worker_pool_config_t *config) {
    if (config->worker_count < 1) {
        fprintf(stderr, "worker pool asked for %d workers, refusing to start\n", config->worker_count);
        return -1;
    }

    worker_t *workers = calloc((size_t)config->worker_count, sizeof(*workers));
    if (workers == NULL) {
        perror("calloc(workers)");
        return -1;
    }

    int started = 0;
    for (int i = 0; i < config->worker_count; i++) {
        workers[i].id = i;
        workers[i].config = config;

        /* pthread_create reports through its return value, not errno:
         * EAGAIN (out of thread slots or memory for a stack) is the one
         * that actually shows up in practice. Losing a worker is not
         * losing the server, so the pool keeps going with fewer threads
         * rather than refusing to serve anything, and says so loudly.
         * Only the case where nothing at all started is fatal. */
        int err = pthread_create(&workers[i].thread, NULL, worker_main, &workers[i]);
        if (err != 0) {
            fprintf(stderr, "pthread_create(worker %d): %s\n", i, strerror(err));
            continue;
        }
        workers[i].started = true;
        started++;
    }

    if (started == 0) {
        fprintf(stderr, "no worker thread could be started\n");
        free(workers);
        return -1;
    }
    if (started < config->worker_count) {
        fprintf(stderr, "running degraded: %d of %d workers started\n", started, config->worker_count);
    }

    /* Blocks here for the life of the server. Joining every started
     * thread (rather than detaching them and exiting) is what makes
     * shutdown orderly: main() only returns once every loop has left
     * its epoll_wait and released its connections, so a leak report at
     * exit means a real leak. */
    int serving = 0;
    for (int i = 0; i < config->worker_count; i++) {
        if (!workers[i].started) {
            continue;
        }
        int err = pthread_join(workers[i].thread, NULL);
        if (err != 0) {
            /* Nothing sensible to do about it, but silently skipping a
             * thread we are about to stop waiting for is worse. */
            fprintf(stderr, "pthread_join(worker %d): %s\n", i, strerror(err));
            continue;
        }
        if (workers[i].served) {
            serving++;
        }
    }

    free(workers);

    if (serving == 0) {
        fprintf(stderr, "no worker ever reached a listening state\n");
        return -1;
    }
    return 0;
}
