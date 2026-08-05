/* accept4() with SOCK_NONBLOCK is a GNU/Linux extension, -std=c11 alone
 * won't expose it from <sys/socket.h>. */
#define _GNU_SOURCE

#include "event_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>  /* IPPROTO_TCP */
#include <netinet/tcp.h> /* TCP_NODELAY, see set_tcp_nodelay */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "http_parser.h"
#include "static_files.h"

#define MAX_EVENTS 64
#define READ_CHUNK_SIZE 4096
#define RESPONSE_BUF_SIZE 512

/* sendfile(2) refuses counts above 0x7ffff000 on Linux ("if count is
 * larger, it is capped" in some versions, EINVAL in others), so a file
 * bigger than 2GB has to be handed over in several calls regardless of
 * how much the socket would take. */
#define SENDFILE_MAX_CHUNK 0x7ffff000

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

    /* Holds the response head currently being sent (status line plus
     * headers, and for the built-in routes the body too). Always built
     * fresh by process_input() before send_response() is called, then
     * possibly re-sent across several EPOLLOUT wakeups if the client is
     * a slow reader. Fixed size instead of malloc'd: every response this
     * server produces is a short, known-shape status line, so there's
     * nothing to gain from a heap buffer sized to fit exactly. */
    char write_buf[RESPONSE_BUF_SIZE];
    size_t write_len;
    size_t write_sent;
    bool keep_alive_after_write;
    bool awaiting_writable; /* true while registered for EPOLLOUT instead of EPOLLIN, see send_response() */

    /* Static file body still to be streamed with sendfile(), or fd < 0
     * when the response has no file body. -1 rather than 0 is load
     * bearing: connections are memset to zero on accept, and fd 0 is a
     * perfectly valid descriptor, so a zeroed field would read as "a
     * file is open" and get stdin closed on the first response. */
    int file_fd;
    off_t file_offset; /* bytes of the file already handed to the kernel */
    off_t file_size;   /* from fstat() at open time, and what Content-Length promised */

    /* Bytes already read() off the wire but not yet parsed, stashed here
     * only when process_input() has to pause mid-batch because a
     * response write blocked. Always holds at most one read()'s worth,
     * see process_input() and stash_pending_input(). */
    char pending_input[READ_CHUNK_SIZE];
    size_t pending_input_len;

    time_t last_activity; /* wall-clock time of the last successful read(), see sweep_idle_connections() */
} connection_t;

/* All the state one running loop owns. It lives on the stack of whoever
 * called event_loop_run() and is threaded through every helper below by
 * pointer, which is what makes layer 6 possible: with N worker threads
 * each running their own loop, a file-scope static for any of this
 * (notably the connection list) would be a data race, since two threads
 * would be splicing the same list on every accept and close.
 *
 * Nothing in here is ever shared between loops. Each loop has its own
 * listen socket (SO_REUSEPORT, see worker_pool.c), its own epoll
 * instance, its own timer, and its own connections, so no lock is needed
 * anywhere in this file. */
typedef struct {
    int epoll_fd;
    int listen_fd;
    int timer_fd;
    int idle_timeout_seconds;
    int worker_id;

    /* Head of the live-connection list, used only by the idle-timeout
     * sweep to enumerate connections epoll itself won't list for us
     * (epoll_wait only reports ready fds, not the full registered set). */
    connection_t *connections;
} event_loop_t;

typedef struct {
    int status;
    const char *reason;
    const char *body;         /* in-memory body, NULL when the body is file.fd */
    const char *content_type;
    const char *extra_header; /* raw header line incl. trailing CRLF, or NULL */

    /* file.fd >= 0 means the body comes from the filesystem and body is
     * NULL. Ownership of that fd transfers to whoever handles this
     * response, see process_input(). */
    static_file_t file;
} routed_response_t;

/* Distinguish the idle-timeout timer and the shutdown eventfd from a
 * connection in epoll_event.data.ptr. The listen socket uses NULL for the
 * same purpose; any other pointer value is a connection_t. Each tag's
 * address is what matters, its (unused) value is irrelevant. Being
 * read-only, they stay file-scope statics even with several loops
 * running: sharing an immutable object across threads is not shared
 * mutable state. */
static const int g_timer_tag;
static const int g_shutdown_tag;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Disables Nagle's algorithm (RFC 896) on an accepted connection.
 *
 * Found by measurement, not by reading: scripts/benchmark.sh reported
 * every static file response taking a flat ~44ms regardless of load (22
 * req/s at one connection, 4457 req/s at 200, the same 44ms latency in
 * both), while the in-memory route on the same server answered in about
 * 1ms. A constant that neither grows with concurrency nor shrinks with
 * idleness is a timer, not contention, and ~40ms is the Linux delayed
 * ACK timer.
 *
 * The two algorithms meet like this. A file response leaves in two
 * pieces: write() puts the header out, then sendfile() sends the body
 * (see try_flush_response). The header is a small segment, so once it is
 * in flight Nagle refuses to send the next small segment until that one
 * is acknowledged, which is exactly what it is for: it stops a stream of
 * tiny writes from becoming a stream of tiny packets. Meanwhile the
 * client has just sent its request and has nothing to say, so delayed
 * ACK (RFC 1122 section 4.2.3.2) holds the acknowledgement back for up
 * to 40ms hoping to piggyback it on data of its own. Neither side is
 * wrong and neither is going to move first, so every response waits out
 * the timer. The in-memory routes never showed it because their whole
 * response, headers and body, is a single write().
 *
 * TCP_NODELAY is the standard answer for an HTTP server, and it is worth
 * being precise about why rather than treating it as a magic incantation.
 * Nagle's heuristic exists because the application has not told the
 * kernel where its messages end, so the kernel guesses from write sizes
 * and timing. In a request/response protocol that guess is simply wrong:
 * the server writes one complete, self-delimiting message and then stops
 * to wait for the peer, so there is never a following small write to
 * coalesce with, and holding the last segment can only ever add latency.
 * Every general purpose server sets it for that reason (nginx, Apache,
 * Go's net/http, and most others do it unconditionally on accept).
 *
 * The alternative would be MSG_MORE on the header write, which tells the
 * kernel "more data follows, hold this segment", the honest way to say
 * what Nagle was guessing. It coalesces header and body into one segment
 * rather than merely refusing to delay, so it is strictly better on this
 * one path, but it has to be threaded through every place a response is
 * written and gets silently wrong the moment a path forgets it. Turning
 * the heuristic off once, per socket, at the only place sockets are
 * created, cannot rot in the same way. The cost of TCP_NODELAY is that a
 * response written in several small pieces goes out as several small
 * packets, which is bounded here: one write() plus one sendfile(),
 * never a per-chunk trickle.
 *
 * Set on the accepted socket rather than on the listener because whether
 * an accepted socket inherits it is platform-specific, and doing it here
 * keeps it next to the write path it exists to serve. Failure is logged
 * and ignored: the connection is still perfectly correct without it,
 * only slower, and refusing to serve a client over a latency
 * optimization would be the worse trade. */
static void set_tcp_nodelay(int fd) {
    int nodelay = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
        perror("setsockopt(TCP_NODELAY)");
    }
}

static void list_insert(event_loop_t *loop, connection_t *conn) {
    conn->prev = NULL;
    conn->next = loop->connections;
    if (loop->connections != NULL) {
        loop->connections->prev = conn;
    }
    loop->connections = conn;
}

static void list_remove(event_loop_t *loop, connection_t *conn) {
    if (conn->prev != NULL) {
        conn->prev->next = conn->next;
    } else {
        loop->connections = conn->next;
    }
    if (conn->next != NULL) {
        conn->next->prev = conn->prev;
    }
}

/* Releases the file backing the current response, if any. Every exit
 * path out of a file response goes through here or close_connection(),
 * otherwise a client hanging up mid-download would leak an fd per
 * request until accept() starts failing with EMFILE. */
static void release_file_body(connection_t *conn) {
    if (conn->file_fd >= 0) {
        close(conn->file_fd);
        conn->file_fd = -1;
    }
    conn->file_offset = 0;
    conn->file_size = 0;
}

static void close_connection(event_loop_t *loop, connection_t *conn) {
    /* close() alone drops fd from the epoll set automatically, see
     * epoll(7). That only holds because we never dup() this fd
     * elsewhere, if we did, the epoll registration would outlive this
     * close and epoll_wait would hand back a dangling conn pointer. */
    list_remove(loop, conn);
    release_file_body(conn);
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

/* Every response whose body is a short constant. Centralized mostly so
 * the file field can't be left uninitialized (a zeroed one would mean
 * "fd 0 holds the body"). */
static routed_response_t simple_response(int status, const char *reason, const char *body, const char *extra_header) {
    return (routed_response_t){
        .status = status,
        .reason = reason,
        .body = body,
        .content_type = "text/plain",
        .extra_header = extra_header,
        .file = {.fd = -1, .size = 0, .content_type = NULL},
    };
}

/* RFC 7231 section 6.5.5 requires a 405 response to list the methods the
 * target does support in an Allow header. */
static routed_response_t method_not_allowed(void) {
    return simple_response(405, "Method Not Allowed", "Method Not Allowed", "Allow: GET\r\n");
}

/* Anything the built-in routes didn't claim is looked up as a file under
 * the document root. */
static routed_response_t route_static_file(const http_request_parser_t *parser) {
    static_file_t file;
    switch (static_file_open(parser->path, parser->path_len, &file)) {
    case STATIC_FILE_INVALID_PATH:
        return simple_response(400, "Bad Request", "Bad Request", NULL);
    case STATIC_FILE_NOT_FOUND:
        return simple_response(404, "Not Found", "Not Found", NULL);
    case STATIC_FILE_OK:
        break;
    }

    /* Method check deliberately after the lookup: 405 asserts that the
     * resource exists and merely doesn't accept this method, so a
     * nonexistent path must answer 404 whatever the method was. */
    if (parser->method_id != HTTP_METHOD_GET) {
        static_file_close(&file);
        return method_not_allowed();
    }

    return (routed_response_t){
        .status = 200,
        .reason = "OK",
        .body = NULL,
        .content_type = file.content_type,
        .extra_header = NULL,
        .file = file,
    };
}

static routed_response_t route_request(const http_request_parser_t *parser) {
    if (path_is(parser, "/") || path_is(parser, "/hello")) {
        if (parser->method_id != HTTP_METHOD_GET) {
            return method_not_allowed();
        }
        if (path_is(parser, "/")) {
            return simple_response(200, "OK", "Hello from my C server!", NULL);
        }
        return simple_response(200, "OK", "Hello, hello!", NULL);
    }
    return route_static_file(parser);
}

/* Both builders return what snprintf() returns: the length the response
 * *would* have had, which is how truncation is detected, see
 * set_write_buffer().
 *
 * X-Worker names the thread that produced the response. Which worker
 * answers is decided by the kernel when it picks one of the SO_REUSEPORT
 * listen sockets for an incoming connection, so without something like
 * this the distribution is invisible from the outside and layer 6 would
 * be untestable end to end (tests/test_thread_pool.py reads exactly this
 * header). It is a deliberate, and deliberately small, piece of
 * observability: a server exposing this in production would be telling
 * clients how many workers it runs, which is why nothing else about the
 * internals goes on the wire. */
static int build_response(char *out, size_t out_size, const routed_response_t *r, const char *connection_value,
                          int worker_id) {
    return snprintf(out, out_size,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "X-Worker: %d\r\n"
                     "%s"
                     "Connection: %s\r\n"
                     "\r\n"
                     "%s",
                     r->status, r->reason, r->content_type, strlen(r->body), worker_id,
                     r->extra_header != NULL ? r->extra_header : "", connection_value, r->body);
}

/* Head of a file response: same shape, but Content-Length comes from
 * fstat() and no body is appended, the body is what sendfile() streams
 * straight out of the page cache afterwards. */
static int build_file_response_head(char *out, size_t out_size, const routed_response_t *r,
                                    const char *connection_value, int worker_id) {
    return snprintf(out, out_size,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %jd\r\n"
                     "X-Worker: %d\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     r->status, r->reason, r->content_type, (intmax_t)r->file.size, worker_id, connection_value);
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

/* Streams the file body straight from the page cache to the socket:
 * sendfile(2) copies between two descriptors inside the kernel, so the
 * bytes never make the round trip into a user-space buffer and back the
 * way a read()/write() loop would.
 *
 * Passing &conn->file_offset (rather than NULL) means the kernel reads
 * from that offset and updates it by however much it transferred, while
 * leaving file_fd's own file position untouched. That is what makes
 * resuming after EAGAIN trivial: the offset already says where to pick
 * up, no lseek() and no separate bookkeeping.
 *
 * On a non-blocking socket a short transfer is entirely normal, not an
 * error, so the loop keeps going until the kernel says EAGAIN or the
 * whole file is out. Semantics match try_flush_write_buffer(): *closed
 * flags a fatal error, the return value only tells "done" from "blocked,
 * come back on EPOLLOUT". */
static bool try_sendfile_body(connection_t *conn, bool *closed) {
    *closed = false;

    while (conn->file_offset < conn->file_size) {
        off_t remaining = conn->file_size - conn->file_offset;
        size_t count = (remaining > SENDFILE_MAX_CHUNK) ? SENDFILE_MAX_CHUNK : (size_t)remaining;

        ssize_t n = sendfile(conn->fd, conn->file_fd, &conn->file_offset, count);
        if (n > 0) {
            continue; /* file_offset was advanced by the kernel */
        }
        if (n == 0) {
            /* EOF before file_size bytes: the file was truncated after
             * we fstat()ed it, so the Content-Length already on the wire
             * is now a lie. There is no way to correct a header that has
             * been sent, and a short body would desynchronize a
             * keep-alive connection, so the only honest framing signal
             * left is closing the connection. */
            fprintf(stderr, "static file shrank while being sent, closing connection\n");
            *closed = true;
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false; /* socket buffer full, resume on the next EPOLLOUT edge */
        }
        /* EPIPE lands here when the client hung up mid-download. It
         * arrives as an errno rather than a fatal signal only because
         * main() ignores SIGPIPE, sendfile() has no MSG_NOSIGNAL to opt
         * out per call the way send() does. */
        perror("sendfile");
        *closed = true;
        return false;
    }

    return true;
}

/* The whole response in order: buffered head first, then the file body.
 * The head must be fully out before sendfile() runs, otherwise a partial
 * header write would end up with body bytes spliced into the middle of
 * the headers. */
static bool try_flush_response(connection_t *conn, bool *closed) {
    if (!try_flush_write_buffer(conn, closed)) {
        return false;
    }
    if (conn->file_fd < 0) {
        return true;
    }
    return try_sendfile_body(conn, closed);
}

static bool finish_after_response_sent(event_loop_t *loop, connection_t *conn) {
    release_file_body(conn);

    if (!conn->keep_alive_after_write) {
        close_connection(loop, conn);
        return false;
    }

    http_parser_init(&conn->parser);
    conn->write_len = 0;
    conn->write_sent = 0;
    conn->last_activity = time(NULL); /* idle clock restarts: waiting for the next request isn't idle time yet */

    if (conn->awaiting_writable) {
        struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.ptr = conn};
        if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev) < 0) {
            perror("epoll_ctl(MOD EPOLLIN)");
            close_connection(loop, conn);
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
static bool send_response(event_loop_t *loop, connection_t *conn) {
    bool closed = false;
    bool done = try_flush_response(conn, &closed);
    if (closed) {
        close_connection(loop, conn);
        return false;
    }
    if (!done) {
        struct epoll_event ev = {.events = EPOLLOUT | EPOLLET, .data.ptr = conn};
        if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev) < 0) {
            perror("epoll_ctl(MOD EPOLLOUT)");
            close_connection(loop, conn);
            return false;
        }
        conn->awaiting_writable = true;
        return true;
    }
    return finish_after_response_sent(loop, conn);
}

/* data may itself be conn->pending_input (see flush_pending_write's
 * resume path), so the source and destination regions can overlap.
 * memmove, not memcpy. */
static void stash_pending_input(connection_t *conn, const char *data, size_t len) {
    memmove(conn->pending_input, data, len);
    conn->pending_input_len = len;
}

/* Adopts whatever the two builders produced, refusing a truncated one.
 * snprintf() reports the length the response *would* have needed, so a
 * value at or past the buffer size means bytes were dropped; taking it
 * as the length anyway would make write() read past write_buf. Every
 * response this server builds fits comfortably, so a failure here is a
 * bug (a new header, a longer reason phrase), not something a client can
 * trigger. Returns false so the caller can fall back to a fixed 500. */
static bool set_write_buffer(connection_t *conn, int written) {
    if (written < 0 || (size_t)written >= sizeof(conn->write_buf)) {
        return false;
    }
    conn->write_len = (size_t)written;
    conn->write_sent = 0;
    return true;
}

/* Fills conn->write_buf with the response for r and takes ownership of
 * r->file if it has one. */
static void prepare_response(connection_t *conn, const routed_response_t *r, bool keep_alive, int worker_id) {
    static const char internal_error[] = "HTTP/1.1 500 Internal Server Error\r\n"
                                         "Content-Type: text/plain\r\n"
                                         "Content-Length: 21\r\n"
                                         "Connection: close\r\n"
                                         "\r\n"
                                         "Internal Server Error";

    conn->keep_alive_after_write = keep_alive;

    const char *connection_value = keep_alive ? "keep-alive" : "close";
    int written = (r->file.fd >= 0)
                      ? build_file_response_head(conn->write_buf, sizeof(conn->write_buf), r, connection_value, worker_id)
                      : build_response(conn->write_buf, sizeof(conn->write_buf), r, connection_value, worker_id);

    if (!set_write_buffer(conn, written)) {
        fprintf(stderr, "response did not fit in %zu bytes, answering 500\n", sizeof(conn->write_buf));
        static_file_t file = r->file;
        static_file_close(&file);
        memcpy(conn->write_buf, internal_error, sizeof(internal_error) - 1);
        conn->write_len = sizeof(internal_error) - 1;
        conn->write_sent = 0;
        conn->keep_alive_after_write = false;
        return;
    }

    if (r->file.fd >= 0) {
        conn->file_fd = r->file.fd;
        conn->file_offset = 0;
        conn->file_size = r->file.size;
    }
}

/* Parses and responds to as many complete requests as are present in
 * [data, data+len), stopping early the moment a response can't be
 * flushed synchronously (send_response already arranged EPOLLOUT and
 * stashed whatever of this range is still unparsed). Returns false if
 * the connection was closed along the way: a parse error, a fatal write
 * error, or a fully flushed response whose Connection header was close. */
static bool process_input(event_loop_t *loop, connection_t *conn, const char *data, size_t len) {
    size_t offset = 0;

    while (offset < len) {
        size_t consumed = 0;
        http_parse_result_t result = http_parser_execute(&conn->parser, data + offset, len - offset, &consumed);
        offset += consumed;

        if (result == HTTP_PARSE_INCOMPLETE) {
            break; /* consumed == len - offset by construction, nothing left to look at */
        }

        bool malformed = (result == HTTP_PARSE_ERROR);
        routed_response_t r =
            malformed ? simple_response(400, "Bad Request", "Bad Request", NULL) : route_request(&conn->parser);
        /* A parse error means the request framing itself is broken, we
         * genuinely don't know where this "request" ends in the byte
         * stream, so there is no safe way to keep treating this
         * connection as HTTP. Close regardless of any Connection header
         * that made it through before things went wrong. */
        bool keep_alive = malformed ? false : http_parser_should_keep_alive(&conn->parser);

        prepare_response(conn, &r, keep_alive, loop->worker_id);

        if (!send_response(loop, conn)) {
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
static void handle_client_readable(event_loop_t *loop, connection_t *conn) {
    char buf[READ_CHUNK_SIZE];

    for (;;) {
        ssize_t n = read(conn->fd, buf, sizeof(buf));
        if (n > 0) {
            conn->last_activity = time(NULL);
            if (!process_input(loop, conn, buf, (size_t)n)) {
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
            close_connection(loop, conn);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; /* drained, this is the only correct exit from an ET read loop */
        }
        perror("read");
        close_connection(loop, conn);
        return;
    }
}

/* EPOLLOUT handler: resumes a response write that previously blocked,
 * whether it stalled in the buffered head or partway through the file
 * body. */
static void flush_pending_write(event_loop_t *loop, connection_t *conn) {
    bool closed = false;
    bool done = try_flush_response(conn, &closed);
    if (closed) {
        close_connection(loop, conn);
        return;
    }
    if (!done) {
        return; /* still can't write everything, stay armed for EPOLLOUT */
    }
    if (!finish_after_response_sent(loop, conn)) {
        return; /* closed: the response we just finished had Connection: close */
    }

    if (conn->pending_input_len > 0) {
        size_t len = conn->pending_input_len;
        conn->pending_input_len = 0; /* clear first, process_input may need to stash into it again */
        process_input(loop, conn, conn->pending_input, len);
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
 * is mandatory, not an optimization.
 *
 * With SO_REUSEPORT there is no thundering herd to worry about here:
 * this thread's listen socket has its own backlog, filled by the kernel
 * alone, so a connection queued on it will not also be visible to any
 * other worker. */
static void accept_new_connections(event_loop_t *loop) {
    for (;;) {
        int conn_fd = accept4(loop->listen_fd, NULL, NULL, SOCK_NONBLOCK);
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

        set_tcp_nodelay(conn_fd);

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
        conn->file_fd = -1; /* must follow the memset, see the field's declaration */
        http_parser_init(&conn->parser);
        conn->last_activity = time(NULL);

        /* Inserted before the epoll_ctl attempt below so that
         * close_connection() works uniformly whether registration
         * succeeds or fails. */
        list_insert(loop, conn);

        struct epoll_event ev = {
            .events = EPOLLIN | EPOLLET,
            .data.ptr = conn,
        };
        if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) < 0) {
            perror("epoll_ctl(ADD conn_fd)");
            close_connection(loop, conn);
        }
    }
}

/* Mechanism for the Slowloris mitigation documented in main.c: any
 * connection that hasn't had a successful read() in idle_timeout_seconds
 * gets closed here, whether it's stuck mid-request or just waiting idle
 * between keep-alive requests. epoll has no built-in notion of a
 * per-fd idle timeout, so this periodic linear sweep of every open
 * connection is what actually enforces one. */
static void sweep_idle_connections(event_loop_t *loop) {
    time_t now = time(NULL);
    connection_t *conn = loop->connections;
    while (conn != NULL) {
        connection_t *next = conn->next; /* close_connection frees conn, grab next first */
        if (now - conn->last_activity >= loop->idle_timeout_seconds) {
            close_connection(loop, conn);
        }
        conn = next;
    }
}

/* Shutdown, and the only reason this loop has a teardown path at all.
 * Left to process exit these would be reclaimed by the kernel anyway,
 * but freeing them explicitly is what lets the integration tests treat
 * any AddressSanitizer leak report at exit as a real bug rather than
 * expected noise.
 *
 * In-flight responses are dropped rather than drained: a client mid
 * download sees its connection close. Finishing them first would mean
 * refusing new connections while looping until every write completes,
 * with a deadline for peers that never read again, which is more
 * shutdown machinery than this layer needs. */
static void close_all_connections(event_loop_t *loop) {
    while (loop->connections != NULL) {
        close_connection(loop, loop->connections);
    }
}

/* Only for the diagnostic below: the three non-connection fds in the
 * epoll set, identified by their data.ptr tag. */
static const char *infrastructure_fd_name(const void *tag) {
    if (tag == NULL) {
        return "listen socket";
    }
    if (tag == &g_timer_tag) {
        return "idle-timeout timer";
    }
    return "shutdown eventfd";
}

static bool is_infrastructure_fd(const void *tag) {
    return tag == NULL || tag == &g_timer_tag || tag == &g_shutdown_tag;
}

int event_loop_run(const event_loop_config_t *config) {
    event_loop_t loop = {
        .epoll_fd = -1,
        .listen_fd = config->listen_fd,
        .timer_fd = -1,
        .idle_timeout_seconds = config->idle_timeout_seconds,
        .worker_id = config->worker_id,
        .connections = NULL,
    };

    if (set_nonblocking(loop.listen_fd) < 0) {
        perror("fcntl(listen_fd, O_NONBLOCK)");
        return -1;
    }

    loop.epoll_fd = epoll_create1(0);
    if (loop.epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    /* data.ptr == NULL marks the listen socket, &g_timer_tag the
     * idle-timeout timer, &g_shutdown_tag the shutdown eventfd, anything
     * else is a connection_t. events[] tells us which case we're in
     * without a separate fd lookup. */
    struct epoll_event listen_ev = {.events = EPOLLIN | EPOLLET, .data.ptr = NULL};
    if (epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, loop.listen_fd, &listen_ev) < 0) {
        perror("epoll_ctl(ADD listen_fd)");
        close(loop.epoll_fd);
        return -1;
    }

    /* A periodic timerfd instead of relying on epoll_wait's own timeout
     * argument: it decouples the sweep interval from however often the
     * loop happens to wake up for I/O, and it's just another fd in the
     * same epoll set rather than a special case in the wait call, see
     * timerfd_create(2). */
    loop.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (loop.timer_fd < 0) {
        perror("timerfd_create");
        close(loop.epoll_fd);
        return -1;
    }
    struct itimerspec sweep_interval = {
        .it_interval = {.tv_sec = IDLE_SWEEP_INTERVAL_SECONDS, .tv_nsec = 0},
        .it_value = {.tv_sec = IDLE_SWEEP_INTERVAL_SECONDS, .tv_nsec = 0},
    };
    if (timerfd_settime(loop.timer_fd, 0, &sweep_interval, NULL) < 0) {
        perror("timerfd_settime");
        close(loop.timer_fd);
        close(loop.epoll_fd);
        return -1;
    }
    struct epoll_event timer_ev = {.events = EPOLLIN | EPOLLET, .data.ptr = (void *)&g_timer_tag};
    if (epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, loop.timer_fd, &timer_ev) < 0) {
        perror("epoll_ctl(ADD timer_fd)");
        close(loop.timer_fd);
        close(loop.epoll_fd);
        return -1;
    }

    /* Deliberately level-triggered, the one fd in this set that is: the
     * same eventfd is registered in every worker's epoll instance and
     * nobody ever read()s it, so once the signal handler writes to it,
     * it stays readable and every worker sees it, whether it was already
     * parked in epoll_wait or only gets there later. Edge-triggered
     * would deliver that single write to whichever epoll instances
     * happened to be watching at that instant and lose it for the rest.
     * Level-triggered also means this fd wakes us on every iteration
     * from then on, which is harmless because the first wakeup leaves
     * the loop. */
    if (config->shutdown_fd >= 0) {
        struct epoll_event shutdown_ev = {.events = EPOLLIN, .data.ptr = (void *)&g_shutdown_tag};
        if (epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, config->shutdown_fd, &shutdown_ev) < 0) {
            perror("epoll_ctl(ADD shutdown_fd)");
            close(loop.timer_fd);
            close(loop.epoll_fd);
            return -1;
        }
    }

    struct epoll_event events[MAX_EVENTS];
    int status = 0;
    bool running = true;

    while (running) {
        int ready = epoll_wait(loop.epoll_fd, events, MAX_EVENTS, -1);
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
                if (is_infrastructure_fd(ud)) {
                    /* The listen socket, the idle-timeout timer or the
                     * shutdown eventfd is broken. None has a sane
                     * degraded mode (no more accepts, silently losing
                     * Slowloris protection, or never noticing SIGTERM),
                     * so there is no point continuing. */
                    fprintf(stderr, "worker %d: %s reported EPOLLERR/EPOLLHUP, giving up\n", loop.worker_id,
                            infrastructure_fd_name(ud));
                    status = -1;
                    running = false;
                    break;
                }
                close_connection(&loop, (connection_t *)ud);
                continue;
            }

            if (ud == NULL) {
                accept_new_connections(&loop);
            } else if (ud == (void *)&g_shutdown_tag) {
                /* Nothing to read: the fd's readability *is* the whole
                 * message, and leaving the counter alone is what keeps
                 * the other workers' epoll instances firing too. */
                running = false;
                break;
            } else if (ud == (void *)&g_timer_tag) {
                uint64_t expirations;
                ssize_t n = read(loop.timer_fd, &expirations, sizeof(expirations));
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
                sweep_idle_connections(&loop);
            } else {
                connection_t *conn = (connection_t *)ud;
                if (conn->awaiting_writable) {
                    flush_pending_write(&loop, conn);
                } else {
                    handle_client_readable(&loop, conn);
                }
            }
        }
    }

    close_all_connections(&loop);
    close(loop.timer_fd);
    close(loop.epoll_fd);
    return status;
}
