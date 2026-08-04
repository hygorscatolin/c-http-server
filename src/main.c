#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "static_files.h"
#include "worker_pool.h"

#define PORT 8080
#define BACKLOG 128 /* completed connections queued for accept(); does not bound in-flight SYNs */

/* Relative to the working directory, so the server is expected to run
 * from the repository root. Overridable mainly so the integration tests
 * can serve a scratch directory they're free to fill with oversized and
 * deliberately hostile files, see tests/test_static_files.py. */
#define DEFAULT_PUBLIC_ROOT "public"

/* Slowloris opens many connections and either sends nothing at all or
 * trickles a request a few bytes at a time, never completing it, tying
 * up a connection slot indefinitely without ever giving the server a
 * request it can answer and close normally. A one-fd-per-connection
 * server like this one has a hard ceiling on concurrent connections (see
 * ulimit -n), so a handful of such clients can starve every legitimate
 * one. Layer 6 raises the ceiling by spreading connections over several
 * threads, it does not remove it: the fd limit is per process, not per
 * thread, so the timeout is what actually reclaims those slots.
 *
 * 30s is a compromise: generous enough that a real client on a slow
 * mobile link, or one just pausing briefly between keep-alive requests,
 * is never punished, short enough that the classic "open and never
 * send" or "send half the headers and stop" attack gets reclaimed
 * quickly instead of accumulating without bound. It does NOT defend
 * against a trickle deliberately timed to land one byte just under this
 * window forever, a fully hardened server would also cap total
 * connection lifetime regardless of activity. That's out of scope here.
 *
 * Overridable via HTTP_SERVER_IDLE_TIMEOUT_SECONDS mainly so it can be
 * driven down for fast integration tests, see tests/test_keepalive.py. */
#define DEFAULT_IDLE_TIMEOUT_SECONDS 30

/* Written to by the SIGINT/SIGTERM handler, and read by every worker's
 * epoll instance. It is the only piece of state the signal handler can
 * touch, and eventfd is what makes that safe: write(2) is
 * async-signal-safe, so the handler does nothing but post 1 to a counter
 * and the actual shutdown work happens back in the event loops, on their
 * own threads, with the full C library available.
 *
 * The alternative, a `volatile sig_atomic_t stop` flag polled by the
 * loops, would need every worker to be woken up to notice it: they sit
 * in epoll_wait() with an infinite timeout, and a signal only interrupts
 * whichever single thread happens to run the handler. Making the flag an
 * fd puts it in the same epoll set as everything else, so one write wakes
 * all N workers at once. */
static int g_shutdown_fd = -1;

static void handle_shutdown_signal(int signum) {
    (void)signum;

    /* errno is per thread but the handler runs on some thread's stack
     * mid-syscall, so clobbering it here would corrupt whatever that
     * thread was about to inspect. */
    int saved_errno = errno;
    uint64_t one = 1;
    ssize_t written = write(g_shutdown_fd, &one, sizeof(one));
    /* Nothing to do if it fails, and nothing safe to say either
     * (fprintf is not async-signal-safe). The result is only consumed to
     * keep -Wunused-result quiet: the one plausible failure is EAGAIN
     * from the counter saturating, which would take 2^64-1 signals. */
    (void)written;
    errno = saved_errno;
}

static int install_shutdown_handler(int signum) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: the loops already treat EINTR from epoll_wait as
     * "go round again", and an interrupted syscall here is harmless
     * because the eventfd write has already happened by then. */
    sa.sa_flags = 0;
    if (sigaction(signum, &sa, NULL) < 0) {
        perror("sigaction");
        return -1;
    }
    return 0;
}

static int idle_timeout_from_env(void) {
    const char *raw = getenv("HTTP_SERVER_IDLE_TIMEOUT_SECONDS");
    if (raw == NULL || raw[0] == '\0') {
        return DEFAULT_IDLE_TIMEOUT_SECONDS;
    }
    char *end = NULL;
    long value = strtol(raw, &end, 10);
    if (end == raw || *end != '\0' || value <= 0 || value > 3600) {
        fprintf(stderr, "ignoring invalid HTTP_SERVER_IDLE_TIMEOUT_SECONDS=%s, using default %d\n", raw,
                DEFAULT_IDLE_TIMEOUT_SECONDS);
        return DEFAULT_IDLE_TIMEOUT_SECONDS;
    }
    return (int)value;
}

int main(void) {
    /* Writing to a socket whose peer has gone away raises SIGPIPE, whose
     * default action is to kill the process: one client closing its
     * browser tab mid-download would take the whole server down. send()
     * could opt out per call with MSG_NOSIGNAL, but sendfile(2) has no
     * equivalent flag, so ignoring the signal process-wide is the only
     * option. The write then fails with EPIPE, which the event loop
     * already handles like any other fatal write error. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal(SIGPIPE)");
        return EXIT_FAILURE;
    }

    /* Resolved once, up front: a document root that doesn't exist is a
     * misconfiguration worth failing on immediately, not something to
     * discover as a puzzling 404 on every request later. Doing it before
     * any thread exists also means the resolved root is only ever
     * written once, and read-only for every worker afterwards. */
    const char *public_root = getenv("HTTP_SERVER_PUBLIC_ROOT");
    if (public_root == NULL || public_root[0] == '\0') {
        public_root = DEFAULT_PUBLIC_ROOT;
    }
    if (static_files_init(public_root) < 0) {
        return EXIT_FAILURE;
    }

    /* EFD_NONBLOCK because a signal handler must never block, EFD_CLOEXEC
     * out of habit: nothing here execs, but an fd that would silently
     * leak into a child if that ever changed is not worth leaving open. */
    g_shutdown_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_shutdown_fd < 0) {
        perror("eventfd");
        return EXIT_FAILURE;
    }

    /* Installed before any thread exists, so every worker is created
     * with the handler already in place. Signals are delivered to an
     * arbitrary thread that hasn't blocked them, which doesn't matter
     * here: whichever one runs the handler, the eventfd it writes is
     * seen by all of them. */
    if (install_shutdown_handler(SIGINT) < 0 || install_shutdown_handler(SIGTERM) < 0) {
        close(g_shutdown_fd);
        return EXIT_FAILURE;
    }

    int idle_timeout_seconds = idle_timeout_from_env();
    worker_pool_config_t pool = {
        .port = PORT,
        .backlog = BACKLOG,
        .worker_count = worker_pool_worker_count(),
        .idle_timeout_seconds = idle_timeout_seconds,
        .shutdown_fd = g_shutdown_fd,
    };

    printf("Servidor escutando na porta %d (%d thread(s) via SO_REUSEPORT, epoll edge-triggered, keep-alive, "
           "idle timeout %ds, estáticos de '%s')...\n",
           PORT, pool.worker_count, idle_timeout_seconds, public_root);
    /* stdout is a pipe under the integration tests, so it is fully
     * buffered rather than line buffered: without this the banner would
     * only appear when the buffer fills or the process exits. */
    fflush(stdout);

    int status = worker_pool_run(&pool);

    close(g_shutdown_fd);
    return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
