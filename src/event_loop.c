#define _GNU_SOURCE

#include "event_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_EVENTS 64
#define READ_CHUNK_SIZE 4096

static const char BODY[] = "Hello from my C server!";

/* Built once at startup instead of per connection: the response is
 * identical for every request at this layer, so there's nothing to
 * recompute on the hot path. Safe as plain global state only because the
 * loop below is single threaded. */
static char g_response[256];
static int g_response_len;

static void init_response(void) {
    g_response_len = snprintf(
        g_response, sizeof(g_response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        sizeof(BODY) - 1, BODY);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void send_response_and_close(int client_fd) {
    ssize_t written;
    do {
        written = write(client_fd, g_response, (size_t)g_response_len);
    } while (written < 0 && errno == EINTR);

    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("write");
    }
    /* We promised "Connection: close" in the header, and the response is
     * small enough that a short write is pathological rather than
     * expected and not worth a retry/backpressure path before layer 3
     * brings real request parsing and buffered writes. */
    close(client_fd);
}

/* Edge-triggered epoll only wakes us once per transition to readable, so
 * a single read() is not enough and anything left unread stays invisible
 * until more bytes arrive (or forever, if the client is done sending).
 * We keep reading until the kernel tells us the socket is dry. */
static void handle_client_readable(int client_fd) {
    char buf[READ_CHUNK_SIZE];

    for (;;) {
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n > 0) {
            /* Payload is discarded and this layer doesn't parse requests
             * yet, it just needs to fully drain the socket per the ET
             * contract above. Note this does mean a client trickling an
             * enormous body could hog this single threaded loop, which is
             * acceptable for a hello-world responder but not for a real
             * server. */
            continue;
        }
        if (n == 0) {
            close(client_fd); /* peer closed before we got to respond */
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break; /* drained. this is the only correct exit from an ET read loop */
        }
        perror("read");
        close(client_fd);
        return;
    }

    send_response_and_close(client_fd);
}

/* listen_fd is itself edge triggered, so a single accept() per wakeup
 * would leave connections stranded in the backlog whenever two or more
 * complete their handshake between two epoll_wait() calls and we'd never
 * get a second edge to remind us they're there. Draining to EAGAIN is
 * mandatory, not an optimization. */
static void accept_new_connections(int epoll_fd, int listen_fd) {
    for (;;) {
        int conn_fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK);
        if (conn_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return; /* backlog fully drained for this edge */
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                /* ECONNABORTED: peer reset the connection before we
                 * accepted it (see accept(2), Linux notes). Not our
                 * problem. move on to the next one in the backlog. */
                continue;
            }
            /* Anything else (EMFILE/ENFILE, etc.) is a real trap in ET
             * mode: bailing out here with connections still queued means
             * we will NOT be notified again, because the edge already
             * fired and nothing new is arriving. A production server
             * needs an fd exhaustion backoff. this layer just logs it. */
            perror("accept4");
            return;
        }

        struct epoll_event ev = {
            .events = EPOLLIN | EPOLLET,
            .data.fd = conn_fd,
        };
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) < 0) {
            perror("epoll_ctl(ADD conn_fd)");
            close(conn_fd);
        }
    }
}

int event_loop_run(int listen_fd) {
    if (set_nonblocking(listen_fd) < 0) {
        perror("fcntl(listen_fd, O_NONBLOCK)");
        return -1;
    }
    init_response();

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    struct epoll_event listen_ev = {
        .events = EPOLLIN | EPOLLET,
        .data.fd = listen_fd,
    };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_ev) < 0) {
        perror("epoll_ctl(ADD listen_fd)");
        close(epoll_fd);
        return -1;
    }

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int ready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue; /* epoll_wait(2): a caught signal restarts the wait */
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < ready; i++) {
            int fd = events[i].data.fd;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                /* This layer keeps no per connection state to react
                 * differently to a dead listen socket vs a reset client,
                 * and closing either is the right call here. close()
                 * alone is sufficient to drop fd from the epoll set
                 * see epoll(7): a descriptor is removed automatically
                 * once its last copy is closed. Only matters if you've
                 * dup()'d the fd elsewhere, which we never do. */
                close(fd);
                continue;
            }

            if (fd == listen_fd) {
                accept_new_connections(epoll_fd, listen_fd);
            } else {
                handle_client_readable(fd);
            }
        }
    }

    close(epoll_fd);
    return 0;
}
