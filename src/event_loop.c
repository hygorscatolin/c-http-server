/* accept4() with SOCK_NONBLOCK is a GNU/Linux extension, -std=c11 alone
 * won't expose it from <sys/socket.h>. */
#define _GNU_SOURCE

#include "event_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http_parser.h"

#define MAX_EVENTS 64
#define READ_CHUNK_SIZE 4096
#define RESPONSE_BUF_SIZE 512

/* One of these per accepted connection, heap allocated so its address
 * can be stashed straight in epoll_event.data.ptr. That sidesteps
 * needing an fd-to-state lookup table: epoll hands the pointer back to
 * us on every event. The parser lives inline here (not behind its own
 * pointer) because both share the exact same lifetime as the socket. */
typedef struct connection {
    int fd;
    http_request_parser_t parser;
} connection_t;

typedef struct {
    int status;
    const char *reason;
    const char *body;
    const char *extra_header; /* raw header line incl. trailing CRLF, or NULL */
} routed_response_t;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void close_connection(connection_t *conn) {
    /* close() alone drops fd from the epoll set automatically, see
     * epoll(7). That only holds because we never dup() this fd
     * elsewhere, if we did, the epoll registration would outlive this
     * close and epoll_wait would hand back a dangling conn pointer. */
    close(conn->fd);
    free(conn);
}

/* True origin-form paths never contain '?', but a request-target can:
 * "/hello?x=1". Routing only cares about the path portion. */
static bool path_is(const http_request_parser_t *p, const char *target) {
    size_t target_len = strlen(target);
    size_t path_len = p->path_len;
    const char *q = memchr(p->path, '?', p->path_len);
    if (q != NULL) {
        path_len = (size_t)(q - p->path);
    }
    return path_len == target_len && memcmp(p->path, target, target_len) == 0;
}

static routed_response_t route_request(const http_request_parser_t *parser) {
    if (path_is(parser, "/") || path_is(parser, "/hello")) {
        if (parser->method_id != HTTP_METHOD_GET) {
            /* RFC 7231 section 6.5.5 requires a 405 response to list the
             * methods the target does support in an Allow header. */
            return (routed_response_t){
                .status = 405, .reason = "Method Not Allowed", .body = "Method Not Allowed", .extra_header = "Allow: GET\r\n"};
        }
        if (path_is(parser, "/")) {
            return (routed_response_t){.status = 200, .reason = "OK", .body = "Hello from my C server!", .extra_header = NULL};
        }
        return (routed_response_t){.status = 200, .reason = "OK", .body = "Hello, hello!", .extra_header = NULL};
    }
    return (routed_response_t){.status = 404, .reason = "Not Found", .body = "Not Found", .extra_header = NULL};
}

static int build_response(char *out, size_t out_size, int status, const char *reason, const char *body,
                           const char *extra_header) {
    return snprintf(out, out_size,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %zu\r\n"
                     "%s"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s",
                     status, reason, strlen(body), extra_header != NULL ? extra_header : "", body);
}

static void write_response_and_close(connection_t *conn, const char *response, size_t response_len) {
    ssize_t written;
    do {
        written = write(conn->fd, response, response_len);
    } while (written < 0 && errno == EINTR);

    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("write");
    }
    /* Every response here promises Connection: close, and the bodies are
     * small enough that a short write is pathological rather than
     * expected. Not worth a retry/backpressure path before a later layer
     * brings buffered, resumable writes. */
    close_connection(conn);
}

static void respond_and_close(connection_t *conn, routed_response_t r) {
    char out[RESPONSE_BUF_SIZE];
    int len = build_response(out, sizeof(out), r.status, r.reason, r.body, r.extra_header);
    write_response_and_close(conn, out, (size_t)len);
}

static void respond_bad_request_and_close(connection_t *conn) {
    char out[RESPONSE_BUF_SIZE];
    int len = build_response(out, sizeof(out), 400, "Bad Request", "Bad Request", NULL);
    write_response_and_close(conn, out, (size_t)len);
}

/* Edge-triggered epoll only wakes us once per transition to readable, so
 * a single read() is not enough, anything left unread stays invisible
 * until more bytes arrive (or forever, if the client is done sending).
 * We keep reading until the kernel tells us the socket is dry, feeding
 * every chunk to the parser as it arrives.
 *
 * Once the parser reaches DONE or ERROR we keep draining rather than
 * stopping immediately. Closing a socket while data is still sitting
 * unread in its receive buffer makes Linux send RST instead of a normal
 * FIN (see tcp(7) and Stevens, UNP section 18.6), which can drop the
 * response we just wrote before the client ever reads it. A pipelined
 * next request or a body we don't support both land here and get
 * silently discarded. */
static void handle_client_readable(connection_t *conn) {
    char buf[READ_CHUNK_SIZE];
    bool peer_closed = false;

    for (;;) {
        ssize_t n = read(conn->fd, buf, sizeof(buf));
        if (n > 0) {
            if (conn->parser.state != HTTP_STATE_DONE && conn->parser.state != HTTP_STATE_ERROR) {
                http_parser_execute(&conn->parser, buf, (size_t)n);
            }
            /* else: draining post-completion input, see comment above */
            continue;
        }
        if (n == 0) {
            peer_closed = true;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break; /* drained, this is the only correct exit from an ET read loop */
        }
        perror("read");
        close_connection(conn);
        return;
    }

    switch (conn->parser.state) {
    case HTTP_STATE_DONE:
        respond_and_close(conn, route_request(&conn->parser));
        return;
    case HTTP_STATE_ERROR:
        respond_bad_request_and_close(conn);
        return;
    default:
        if (peer_closed) {
            close_connection(conn); /* client vanished mid-request, nothing to answer */
        }
        /* else genuinely incomplete: unlike layer 2's "read once, respond,
         * close", state lives on the connection and we simply return here
         * to wait for the next EPOLLIN edge with more bytes. A client
         * trickling headers in one byte at a time can still starve this
         * single-threaded loop, same tradeoff as layer 2's oversized body
         * case, still out of scope for a hello server. */
        return;
    }
}

/* listen_fd is itself edge-triggered, so a single accept() per wakeup
 * would leave connections stranded in the backlog whenever two or more
 * complete their handshake between two epoll_wait() calls, and we'd
 * never get a second edge to remind us they're there. Draining to EAGAIN
 * is mandatory, not an optimization. */
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
                 * problem, move on to the next one in the backlog. */
                continue;
            }
            /* Anything else (EMFILE/ENFILE, etc.) is a real trap in ET
             * mode: bailing out here with connections still queued means
             * we will NOT be notified again, because the edge already
             * fired and nothing new is arriving. A production server
             * needs an fd-exhaustion backoff, this layer just logs it. */
            perror("accept4");
            return;
        }

        connection_t *conn = malloc(sizeof(*conn));
        if (conn == NULL) {
            /* Out of memory under connection load: drop this one client
             * instead of taking the whole server down. */
            perror("malloc(connection_t)");
            close(conn_fd);
            continue;
        }
        conn->fd = conn_fd;
        http_parser_init(&conn->parser);

        struct epoll_event ev = {
            .events = EPOLLIN | EPOLLET,
            .data.ptr = conn,
        };
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) < 0) {
            perror("epoll_ctl(ADD conn_fd)");
            close_connection(conn);
        }
    }
}

int event_loop_run(int listen_fd) {
    if (set_nonblocking(listen_fd) < 0) {
        perror("fcntl(listen_fd, O_NONBLOCK)");
        return -1;
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    /* data.ptr == NULL marks the listen socket. Every accepted
     * connection is registered with a non-NULL connection_t pointer
     * instead, so events[] tells us which case we're in without a
     * separate fd lookup. */
    struct epoll_event listen_ev = {
        .events = EPOLLIN | EPOLLET,
        .data.ptr = NULL,
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
            void *ud = events[i].data.ptr;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (ud == NULL) {
                    /* The listen socket itself is broken. Nothing left to
                     * accept connections with, so there is no point
                     * continuing the loop. */
                    fprintf(stderr, "listen socket reported EPOLLERR/EPOLLHUP\n");
                    close(epoll_fd);
                    return -1;
                }
                close_connection((connection_t *)ud);
                continue;
            }

            if (ud == NULL) {
                accept_new_connections(epoll_fd, listen_fd);
            } else {
                handle_client_readable((connection_t *)ud);
            }
        }
    }

    close(epoll_fd);
    return 0;
}
