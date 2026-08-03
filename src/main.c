#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "event_loop.h"

#define PORT 8080
#define BACKLOG 128 /* completed connections queued for accept(); does not bound in-flight SYNs */

/* Slowloris opens many connections and either sends nothing at all or
 * trickles a request a few bytes at a time, never completing it, tying
 * up a connection slot indefinitely without ever giving the server a
 * request it can answer and close normally. A single-threaded,
 * one-fd-per-connection server like this one has a hard ceiling on
 * concurrent connections (see ulimit -n), so a handful of such clients
 * can starve every legitimate one.
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

static int create_listen_socket(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* Without this, restarting the server right after it exits fails with
     * EADDRINUSE while the previous socket sits in TIME_WAIT. */
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(PORT),
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        exit(EXIT_FAILURE);
    }

    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        exit(EXIT_FAILURE);
    }

    return fd;
}

int main(void) {
    int listen_fd = create_listen_socket();
    int idle_timeout_seconds = idle_timeout_from_env();
    printf("Servidor escutando na porta %d (epoll, edge-triggered, keep-alive, idle timeout %ds)...\n", PORT,
           idle_timeout_seconds);

    int status = event_loop_run(listen_fd, idle_timeout_seconds);

    close(listen_fd);
    return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
