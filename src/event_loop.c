/* accept4() with SOCK_NONBLOCK is a GNU/Linux extension, -std=c11 alone
 * won't expose it from <sys/socket.h>. */
#define _GNU_SOURCE

#include "event_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "http_parser.h"

#define MAX_EVENTS 64
#define READ_CHUNK_SIZE 4096
#define RESPONSE_BUF_SIZE 512

/* How often the idle-timeout sweep runs, independent of how often the
 * loop happens to wake up for I/O. See main.c for the timeout value
 * itself and the Slowloris rationale behind it. 5s means a connection
 * can linger up to that long past its actual deadline before we notice,
 * a reasonable slack for a check this cheap (see sweep_idle_connections). */
#define IDLE_SWEEP_INTERVAL_SECONDS 5

/* One of these per accepted connection, heap allocated so its address
 * can be stashed straight in epoll_event.data.ptr. That sidesteps
 * needing an fd-to-state lookup table: epoll hands the pointer back to
 * us on every event. The parser lives inline here (not behind its own
 * pointer) because both share the exact same lifetime as the socket. */
typedef struct connection {
    int fd;
    http_request_parser_t parser;

    struct connection *prev;
    struct connection *next;

    /* Holds the response currently being sent. Always built fresh by
     * process_input() before send_response() is called, then possibly
     * re-sent across several EPOLLOUT wakeups if the client is a slow
     * reader. Fixed size instead of malloc'd: every response this
     * server produces is a short, known-shape status line, so there's
     * nothing to gain from a heap buffer sized to fit exactly. */
    char write_buf[RESPONSE_BUF_SIZE];
    size_t write_len;
    size_t write_sent;
    bool keep_alive_after_write;
    bool awaiting_writable; /* true while registered for EPOLLOUT instead of EPOLLIN, see send_response() */

    /* Bytes already read() off the wire but not yet parsed, stashed here
     * only when process_input() has to pause mid-batch because a
     * response write blocked. Always holds at most one read()'s worth,
     * see process_input() and stash_pending_input(). */
    char pending_input[READ_CHUNK_SIZE];
    size_t pending_input_len;

    time_t last_activity; /* wall-clock time of the last successful read(), see sweep_idle_connections() */
} connection_t;

typedef struct {
    int status;
    const char *reason;
    const char *body;
    const char *extra_header; /* raw header line incl. trailing CRLF, or NULL */
} routed_response_t;

/* Head of the live-connection list, used only by the idle-timeout sweep
 * to enumerate connections epoll itself won't list for us (epoll_wait
 * only reports ready fds, not the full registered set). A plain
 * file-scope static is fine here: this module only ever runs one event
 * loop per process, there is no concurrent instance to conflict with. */
static connection_t *g_connections = NULL;

/* Distinguishes the idle-timeout timer from a connection in
 * epoll_event.data.ptr. The listen socket uses NULL for the same
 * purpose; any other pointer value is a connection_t. The tag's address
 * is what matters, its (unused) value is irrelevant. */
static const int g_timer_tag;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void list_insert(connection_t *conn) {
    conn->prev = NULL;
    conn->next = g_connections;
    if (g_connections != NULL) {
        g_connections->prev = conn;
    }
    g_connections = conn;
}

static void list_remove(connection_t *conn) {
    if (conn->prev != NULL) {
        conn->prev->next = conn->next;
    } else {
        g_connections = conn->next;
    }
    if (conn->next != NULL) {
        conn->next->prev = conn->prev;
    }
}

static void close_connection(connection_t *conn) {
    /* close() alone drops fd from the epoll set automatically, see
     * epoll(7). That only holds because we never dup() this fd
     * elsewhere, if we did, the epoll registration would outlive this
     * close and epoll_wait would hand back a dangling conn pointer. */
    list_remove(conn);
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
                           const char *extra_header, const char *connection_value) {
    return snprintf(out, out_size,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %zu\r\n"
                     "%s"
                     "Connection: %s\r\n"
                     "\r\n"
                     "%s",
                     status, reason, strlen(body), extra_header != NULL ? extra_header : "", connection_value, body);
}

/* Drains conn->write_buf[write_sent..write_len) as far as the socket
 * currently allows. *closed reports a fatal write error (caller must
 * close_connection and stop touching conn). The return value only
 * distinguishes "fully sent" from "blocked, try again later", check
 * *closed first. */
static bool try_flush_write_buffer(connection_t *conn, bool *closed) {
    *closed = false;
    while (conn->write_sent < conn->write_len) {
        ssize_t n = write(conn->fd, conn->write_buf + conn->write_sent, conn->write_len - conn->write_sent);
        if (n > 0) {
            conn->write_sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;
        }
        perror("write");
        *closed = true;
        return false;
    }
    return true;
}

static bool finish_after_response_sent(int epoll_fd, connection_t *conn) {
    if (!conn->keep_alive_after_write) {
        close_connection(conn);
        return false;
    }

    http_parser_init(&conn->parser);
    conn->write_len = 0;
    conn->write_sent = 0;
    conn->last_activity = time(NULL); /* idle clock restarts: waiting for the next request isn't idle time yet */

    if (conn->awaiting_writable) {
        struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.ptr = conn};
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev) < 0) {
            perror("epoll_ctl(MOD EPOLLIN)");
            close_connection(conn);
            return false;
        }
        conn->awaiting_writable = false;
    }
    /* else: still EPOLLIN-registered from before, the common case of a
     * response that flushed synchronously never touched epoll_ctl. */

    return true;
}

/* conn->write_buf/write_len/write_sent=0/keep_alive_after_write must
 * already be set by the caller. Tries to flush synchronously, if the
 * socket can't take any more right now, the unsent tail is already
 * sitting in conn->write_buf (nothing to copy) and we switch this
 * connection's epoll interest to EPOLLOUT so we get woken up again once
 * there's room, instead of spinning or blocking the whole event loop on
 * one slow reader.
 *
 * While a write is pending we deliberately stop asking for EPOLLIN too:
 * queuing more than one response per connection is scope a hello server
 * doesn't need, so a client that pipelines requests faster than it
 * drains its socket just gets ordinary TCP receive-buffer backpressure
 * instead of an ever-growing response queue on our side.
 *
 * Returns false if the connection was closed (fatal write error, or a
 * fully flushed response whose Connection: close means we're done with
 * it), true if it's still alive (either fully flushed and reset for
 * another request, or now waiting on EPOLLOUT). */
static bool send_response(int epoll_fd, connection_t *conn) {
    bool closed = false;
    bool done = try_flush_write_buffer(conn, &closed);
    if (closed) {
        close_connection(conn);
        return false;
    }
    if (!done) {
        struct epoll_event ev = {.events = EPOLLOUT | EPOLLET, .data.ptr = conn};
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev) < 0) {
            perror("epoll_ctl(MOD EPOLLOUT)");
            close_connection(conn);
            return false;
        }
        conn->awaiting_writable = true;
        return true;
    }
    return finish_after_response_sent(epoll_fd, conn);
}

/* data may itself be conn->pending_input (see flush_pending_write's
 * resume path), so the source and destination regions can overlap.
 * memmove, not memcpy. */
static void stash_pending_input(connection_t *conn, const char *data, size_t len) {
    memmove(conn->pending_input, data, len);
    conn->pending_input_len = len;
}

/* Parses and responds to as many complete requests as are present in
 * [data, data+len), stopping early the moment a response can't be
 * flushed synchronously (send_response already arranged EPOLLOUT and
 * stashed whatever of this range is still unparsed). Returns false if
 * the connection was closed along the way: a parse error, a fatal write
 * error, or a fully flushed response whose Connection header was close. */
static bool process_input(int epoll_fd, connection_t *conn, const char *data, size_t len) {
    size_t offset = 0;

    while (offset < len) {
        size_t consumed = 0;
        http_parse_result_t result = http_parser_execute(&conn->parser, data + offset, len - offset, &consumed);
        offset += consumed;

        if (result == HTTP_PARSE_INCOMPLETE) {
            break; /* consumed == len - offset by construction, nothing left to look at */
        }

        bool malformed = (result == HTTP_PARSE_ERROR);
        routed_response_t r = malformed
            ? (routed_response_t){.status = 400, .reason = "Bad Request", .body = "Bad Request", .extra_header = NULL}
            : route_request(&conn->parser);
        /* A parse error means the request framing itself is broken, we
         * genuinely don't know where this "request" ends in the byte
         * stream, so there is no safe way to keep treating this
         * connection as HTTP. Close regardless of any Connection header
         * that made it through before things went wrong. */
        bool keep_alive = malformed ? false : http_parser_should_keep_alive(&conn->parser);

        conn->keep_alive_after_write = keep_alive;
        conn->write_len = (size_t)build_response(conn->write_buf, sizeof(conn->write_buf), r.status, r.reason, r.body,
                                                  r.extra_header, keep_alive ? "keep-alive" : "close");
        conn->write_sent = 0;

        if (!send_response(epoll_fd, conn)) {
            return false;
        }
        if (conn->awaiting_writable) {
            stash_pending_input(conn, data + offset, len - offset);
            return true;
        }
        /* Fully flushed and kept alive: the parser was just reset inside
         * send_response -> finish_after_response_sent. Loop again in
         * case another complete request is already sitting in the rest
         * of this same chunk (back-to-back pipelined requests). */
    }

    return true;
}

/* Edge-triggered epoll only wakes us once per transition to readable, so
 * a single read() is not enough, anything left unread stays invisible
 * until more bytes arrive (or forever, if the client is done sending).
 * We keep reading until the kernel tells us the socket is dry, handing
 * every chunk to process_input() as it arrives. */
static void handle_client_readable(int epoll_fd, connection_t *conn) {
    char buf[READ_CHUNK_SIZE];

    for (;;) {
        ssize_t n = read(conn->fd, buf, sizeof(buf));
        if (n > 0) {
            conn->last_activity = time(NULL);
            if (!process_input(epoll_fd, conn, buf, (size_t)n)) {
                return; /* closed */
            }
            if (conn->awaiting_writable) {
                /* process_input already stashed whatever was left unread
                 * in this chunk and switched us to EPOLLOUT. Nothing more
                 * to do on the read side until the write drains. */
                return;
            }
            continue;
        }
        if (n == 0) {
            /* Peer closed. If a request was mid-flight there's nothing
             * to answer; if we were just idling between keep-alive
             * requests this is simply the connection ending normally.
             * Either way there's no response owed. */
            close_connection(conn);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; /* drained, this is the only correct exit from an ET read loop */
        }
        perror("read");
        close_connection(conn);
        return;
    }
}

/* EPOLLOUT handler: resumes a response write that previously blocked. */
static void flush_pending_write(int epoll_fd, connection_t *conn) {
    bool closed = false;
    bool done = try_flush_write_buffer(conn, &closed);
    if (closed) {
        close_connection(conn);
        return;
    }
    if (!done) {
        return; /* still can't write everything, stay armed for EPOLLOUT */
    }
    if (!finish_after_response_sent(epoll_fd, conn)) {
        return; /* closed: the response we just finished had Connection: close */
    }

    if (conn->pending_input_len > 0) {
        size_t len = conn->pending_input_len;
        conn->pending_input_len = 0; /* clear first, process_input may need to stash into it again */
        process_input(epoll_fd, conn, conn->pending_input, len);
        /* Don't touch conn again here regardless of the outcome: by now
         * it may be closed (freed), fully caught up and back to plain
         * EPOLLIN waiting, or paused again on a second blocked write
         * with a new remainder already stashed. process_input and its
         * callees left it in a consistent state either way. */
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
        memset(conn, 0, sizeof(*conn));
        conn->fd = conn_fd;
        http_parser_init(&conn->parser);
        conn->last_activity = time(NULL);

        /* Inserted before the epoll_ctl attempt below so that
         * close_connection() works uniformly whether registration
         * succeeds or fails. */
        list_insert(conn);

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

/* Mechanism for the Slowloris mitigation documented in main.c: any
 * connection that hasn't had a successful read() in idle_timeout_seconds
 * gets closed here, whether it's stuck mid-request or just waiting idle
 * between keep-alive requests. epoll has no built-in notion of a
 * per-fd idle timeout, so this periodic linear sweep of every open
 * connection is what actually enforces one. */
static void sweep_idle_connections(int idle_timeout_seconds) {
    time_t now = time(NULL);
    connection_t *conn = g_connections;
    while (conn != NULL) {
        connection_t *next = conn->next; /* close_connection frees conn, grab next first */
        if (now - conn->last_activity >= idle_timeout_seconds) {
            close_connection(conn);
        }
        conn = next;
    }
}

int event_loop_run(int listen_fd, int idle_timeout_seconds) {
    if (set_nonblocking(listen_fd) < 0) {
        perror("fcntl(listen_fd, O_NONBLOCK)");
        return -1;
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    /* data.ptr == NULL marks the listen socket, &g_timer_tag marks the
     * idle-timeout timer, anything else is a connection_t. events[]
     * tells us which case we're in without a separate fd lookup. */
    struct epoll_event listen_ev = {.events = EPOLLIN | EPOLLET, .data.ptr = NULL};
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_ev) < 0) {
        perror("epoll_ctl(ADD listen_fd)");
        close(epoll_fd);
        return -1;
    }

    /* A periodic timerfd instead of relying on epoll_wait's own timeout
     * argument: it decouples the sweep interval from however often the
     * loop happens to wake up for I/O, and it's just another fd in the
     * same epoll set rather than a special case in the wait call, see
     * timerfd_create(2). */
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (timer_fd < 0) {
        perror("timerfd_create");
        close(epoll_fd);
        return -1;
    }
    struct itimerspec sweep_interval = {
        .it_interval = {.tv_sec = IDLE_SWEEP_INTERVAL_SECONDS, .tv_nsec = 0},
        .it_value = {.tv_sec = IDLE_SWEEP_INTERVAL_SECONDS, .tv_nsec = 0},
    };
    if (timerfd_settime(timer_fd, 0, &sweep_interval, NULL) < 0) {
        perror("timerfd_settime");
        close(timer_fd);
        close(epoll_fd);
        return -1;
    }
    struct epoll_event timer_ev = {.events = EPOLLIN | EPOLLET, .data.ptr = (void *)&g_timer_tag};
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &timer_ev) < 0) {
        perror("epoll_ctl(ADD timer_fd)");
        close(timer_fd);
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
                if (ud == NULL || ud == (void *)&g_timer_tag) {
                    /* The listen socket or the idle-timeout timer itself
                     * is broken, neither has a sane degraded mode (no
                     * more accepts, or silently losing Slowloris
                     * protection), so there is no point continuing. */
                    fprintf(stderr, "%s reported EPOLLERR/EPOLLHUP, giving up\n",
                            ud == NULL ? "listen socket" : "idle-timeout timer");
                    close(timer_fd);
                    close(epoll_fd);
                    return -1;
                }
                close_connection((connection_t *)ud);
                continue;
            }

            if (ud == NULL) {
                accept_new_connections(epoll_fd, listen_fd);
            } else if (ud == (void *)&g_timer_tag) {
                uint64_t expirations;
                ssize_t n = read(timer_fd, &expirations, sizeof(expirations));
                /* timerfd_read(2, "Reading from a timerfd"): a read
                 * always returns exactly sizeof(uint64_t) or fails with
                 * EAGAIN, never a short read. A different failure here
                 * would mean something is seriously wrong with the timer
                 * fd, but wall-clock time in the sweep below doesn't
                 * depend on this value, so it's safe to just log and
                 * sweep anyway rather than treat it as fatal. */
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read(timer_fd)");
                }
                sweep_idle_connections(idle_timeout_seconds);
            } else {
                connection_t *conn = (connection_t *)ud;
                if (conn->awaiting_writable) {
                    flush_pending_write(epoll_fd, conn);
                } else {
                    handle_client_readable(epoll_fd, conn);
                }
            }
        }
    }

    close(timer_fd);
    close(epoll_fd);
    return 0;
}
