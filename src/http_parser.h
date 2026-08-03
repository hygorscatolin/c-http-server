#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

/* RFC 7230 doesn't cap request-line or header sizes. Every real server
 * enforces its own limits anyway, precisely so a client can't force
 * unbounded per-connection memory growth just by not sending a CRLF.
 * These values are generous for a hello server, not tuned for any real
 * deployment. */
#define HTTP_METHOD_MAX 16
#define HTTP_PATH_MAX 2048
#define HTTP_HEADER_LINE_MAX 8192
#define HTTP_MAX_HEADERS 64

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_OPTIONS,
    HTTP_METHOD_PATCH,
    HTTP_METHOD_OTHER, /* syntactically valid token, just not one we special case */
} http_method_t;

typedef enum {
    HTTP_PARSE_INCOMPLETE, /* need more bytes, keep the connection open and wait for the next epoll edge */
    HTTP_PARSE_COMPLETE,   /* request line and headers fully parsed, ready to route */
    HTTP_PARSE_ERROR,      /* malformed or over a size limit, caller must answer 400 and close */
} http_parse_result_t;

/* States are exposed because http_request_parser_t is meant to be
 * embedded directly in the caller's per-connection struct (stack or
 * heap, caller's choice), not hidden behind an opaque pointer this
 * module allocates. One parser per connection, same lifetime as the
 * socket it belongs to. */
typedef enum {
    HTTP_STATE_METHOD,
    HTTP_STATE_PATH,
    HTTP_STATE_VERSION,
    HTTP_STATE_REQUEST_LINE_LF,
    HTTP_STATE_HEADER_LINE_START,
    HTTP_STATE_HEADER_NAME,
    HTTP_STATE_HEADER_VALUE_SP,
    HTTP_STATE_HEADER_VALUE,
    HTTP_STATE_HEADER_LF,
    HTTP_STATE_HEADERS_END_LF,
    HTTP_STATE_DONE,
    HTTP_STATE_ERROR,
} http_parser_state_t;

typedef struct {
    http_parser_state_t state;

    char method[HTTP_METHOD_MAX];
    size_t method_len;
    http_method_t method_id;

    char path[HTTP_PATH_MAX];
    size_t path_len;

    char version[16];
    size_t version_len;

    /* One reusable buffer for whichever header line is currently being
     * read. header_name_len marks where the name ends and the value
     * begins inside it, see http_parser.c for why a single buffer is
     * enough instead of separate name/value storage. */
    char header_line[HTTP_HEADER_LINE_MAX];
    size_t header_line_len;
    size_t header_name_len;
    int header_count;
} http_request_parser_t;

void http_parser_init(http_request_parser_t *parser);

/* Feeds up to len new bytes into the state machine, resuming exactly
 * where the previous call left off. The result must not depend on how
 * the caller happened to split the input across read()s, a 1 byte chunk
 * and a 4KB chunk of the same request produce the same outcome. Once
 * this returns HTTP_PARSE_COMPLETE or HTTP_PARSE_ERROR the parser is
 * done. Calling it again just returns the same terminal result without
 * touching any more input. */
http_parse_result_t http_parser_execute(http_request_parser_t *parser, const char *data, size_t len);

#endif
