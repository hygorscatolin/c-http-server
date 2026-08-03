#include "http_parser.h"

#include <string.h>
#include <strings.h> /* strncasecmp: header names and Connection tokens are case-insensitive, RFC 7230 3.2/6.1 */

static int is_method_char(unsigned char c) {
    /* Real methods are uppercase tokens by convention (GET, POST, ...).
     * RFC 7230's token grammar is technically looser, but restricting to
     * A-Z keeps this predictable and gives lowercase or mixed-case input
     * a clean rejection instead of quietly accepting something a client
     * almost certainly didn't mean to send. */
    return c >= 'A' && c <= 'Z';
}

static int is_path_char(unsigned char c) {
    /* Excludes space (the request-target's own terminator), the other
     * ASCII control characters, and DEL. Everything else, including
     * high-bit bytes from a UTF-8 encoded path, is passed through. */
    return c > 0x20 && c != 0x7F;
}

static int is_header_name_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static int is_header_value_char(unsigned char c) {
    /* SP and HTAB are legal inside a value, everything else below 0x20
     * (and DEL) is a control character with no business in a header. */
    return c == '\t' || (c >= 0x20 && c != 0x7F);
}

static int is_valid_version(const char *v, size_t len) {
    /* Exactly "HTTP/" DIGIT "." DIGIT per RFC 7230 section 2.6. Any
     * minor version is accepted syntactically. A server that doesn't
     * understand a client's minor version should still reply using the
     * version it does support rather than reject the request outright,
     * which is what the router does here regardless of what's parsed. */
    return len == 8 && v[0] == 'H' && v[1] == 'T' && v[2] == 'T' && v[3] == 'P' && v[4] == '/' && v[5] >= '0' &&
           v[5] <= '9' && v[6] == '.' && v[7] >= '0' && v[7] <= '9';
}

static http_method_t classify_method(const char *m, size_t len) {
    static const struct {
        const char *name;
        http_method_t id;
    } known[] = {
        {"GET", HTTP_METHOD_GET},         {"POST", HTTP_METHOD_POST},     {"PUT", HTTP_METHOD_PUT},
        {"DELETE", HTTP_METHOD_DELETE},   {"HEAD", HTTP_METHOD_HEAD},     {"OPTIONS", HTTP_METHOD_OPTIONS},
        {"PATCH", HTTP_METHOD_PATCH},
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (len == strlen(known[i].name) && memcmp(m, known[i].name, len) == 0) {
            return known[i].id;
        }
    }
    return HTTP_METHOD_OTHER;
}

static bool header_name_is(const http_request_parser_t *p, const char *name) {
    size_t len = strlen(name);
    return p->header_name_len == len && strncasecmp(p->header_line, name, len) == 0;
}

/* Called right after a header line completes, if its name is
 * "Connection". Scans the value as a comma-separated list of tokens
 * (RFC 7230 section 6.1, e.g. "keep-alive" or, in principle,
 * "close, foo") and records whichever of "close" / "keep-alive" it
 * finds. A client sending both is nonsensical, but rather than guess
 * which one is "right" we just let the last recognized token in the
 * list win, matching how real parsers typically process repeated or
 * conflicting directives. */
static void note_connection_header(http_request_parser_t *p) {
    const char *value = p->header_line + p->header_name_len;
    const char *cursor = value;

    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        const char *tok_start = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            cursor++;
        }
        const char *tok_end = cursor;
        while (tok_end > tok_start && (tok_end[-1] == ' ' || tok_end[-1] == '\t')) {
            tok_end--;
        }
        size_t tok_len = (size_t)(tok_end - tok_start);
        if (tok_len == 5 && strncasecmp(tok_start, "close", 5) == 0) {
            p->connection_directive = HTTP_CONN_CLOSE;
        } else if (tok_len == 10 && strncasecmp(tok_start, "keep-alive", 10) == 0) {
            p->connection_directive = HTTP_CONN_KEEPALIVE;
        }
    }
}

void http_parser_init(http_request_parser_t *parser) {
    memset(parser, 0, sizeof(*parser));
    parser->state = HTTP_STATE_METHOD;
}

http_parse_result_t http_parser_execute(http_request_parser_t *p, const char *data, size_t len, size_t *consumed) {
    if (p->state == HTTP_STATE_DONE) {
        *consumed = 0;
        return HTTP_PARSE_COMPLETE;
    }
    if (p->state == HTTP_STATE_ERROR) {
        *consumed = 0;
        return HTTP_PARSE_ERROR;
    }

    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)data[i];
        int advance = 1;

        switch (p->state) {
        case HTTP_STATE_METHOD:
            if (c == ' ') {
                if (p->method_len == 0) {
                    p->state = HTTP_STATE_ERROR;
                    break;
                }
                p->method_id = classify_method(p->method, p->method_len);
                p->state = HTTP_STATE_PATH;
            } else if (is_method_char(c)) {
                if (p->method_len >= sizeof(p->method) - 1) {
                    p->state = HTTP_STATE_ERROR; /* method too long */
                    break;
                }
                p->method[p->method_len++] = (char)c;
                p->method[p->method_len] = '\0';
            } else {
                p->state = HTTP_STATE_ERROR; /* not a token character */
            }
            break;

        case HTTP_STATE_PATH:
            if (c == ' ') {
                if (p->path_len == 0) {
                    p->state = HTTP_STATE_ERROR; /* e.g. two spaces after the method */
                    break;
                }
                p->state = HTTP_STATE_VERSION;
            } else if (is_path_char(c)) {
                if (p->path_len >= sizeof(p->path) - 1) {
                    p->state = HTTP_STATE_ERROR; /* path too long */
                    break;
                }
                p->path[p->path_len++] = (char)c;
                p->path[p->path_len] = '\0';
            } else {
                p->state = HTTP_STATE_ERROR;
            }
            break;

        case HTTP_STATE_VERSION:
            if (c == '\r') {
                if (!is_valid_version(p->version, p->version_len)) {
                    p->state = HTTP_STATE_ERROR;
                    break;
                }
                /* is_valid_version already confirmed this exact layout,
                 * "HTTP/" DIGIT "." DIGIT, so indexing straight in is safe. */
                p->http_major = p->version[5] - '0';
                p->http_minor = p->version[7] - '0';
                p->state = HTTP_STATE_REQUEST_LINE_LF;
            } else if (c >= 0x21 && c <= 0x7E) {
                if (p->version_len >= sizeof(p->version) - 1) {
                    p->state = HTTP_STATE_ERROR;
                    break;
                }
                p->version[p->version_len++] = (char)c;
                p->version[p->version_len] = '\0';
            } else {
                p->state = HTTP_STATE_ERROR; /* no legal whitespace inside the version token */
            }
            break;

        case HTTP_STATE_REQUEST_LINE_LF:
            /* Strict CRLF only. RFC 7230 section 3.5 allows a lenient
             * server to tolerate a bare LF, but accepting it quietly
             * blurs where a line actually ends, which is exactly the
             * kind of ambiguity request-smuggling bugs live in. Layer 3
             * chooses strictness over leniency here. */
            p->state = (c == '\n') ? HTTP_STATE_HEADER_LINE_START : HTTP_STATE_ERROR;
            break;

        case HTTP_STATE_HEADER_LINE_START:
            if (c == '\r') {
                p->state = HTTP_STATE_HEADERS_END_LF; /* blank line: end of headers */
            } else if (is_header_name_char(c)) {
                if (p->header_count >= HTTP_MAX_HEADERS) {
                    p->state = HTTP_STATE_ERROR;
                    break;
                }
                p->header_line_len = 0;
                p->header_line[p->header_line_len++] = (char)c;
                p->header_line[p->header_line_len] = '\0';
                p->state = HTTP_STATE_HEADER_NAME;
            } else {
                /* Also catches obsolete line folding (RFC 7230 section
                 * 3.2.4), a header value continued onto the next line by
                 * starting it with whitespace. Folding is deprecated
                 * specifically because parsers that disagree about where
                 * a header ends open the door to request smuggling, so
                 * this parser rejects it outright rather than support it. */
                p->state = HTTP_STATE_ERROR;
            }
            break;

        case HTTP_STATE_HEADER_NAME:
            if (c == ':') {
                p->header_name_len = p->header_line_len;
                p->state = HTTP_STATE_HEADER_VALUE_SP;
            } else if (is_header_name_char(c)) {
                if (p->header_line_len >= sizeof(p->header_line) - 1) {
                    p->state = HTTP_STATE_ERROR; /* header line too long */
                    break;
                }
                p->header_line[p->header_line_len++] = (char)c;
                p->header_line[p->header_line_len] = '\0';
            } else {
                p->state = HTTP_STATE_ERROR; /* e.g. a space before the colon */
            }
            break;

        case HTTP_STATE_HEADER_VALUE_SP:
            if (c == ' ' || c == '\t') {
                /* consume optional whitespace between ':' and the value */
            } else {
                p->state = HTTP_STATE_HEADER_VALUE;
                advance = 0; /* re-run this same byte now that we're in HEADER_VALUE */
            }
            break;

        case HTTP_STATE_HEADER_VALUE:
            if (c == '\r') {
                p->state = HTTP_STATE_HEADER_LF;
            } else if (is_header_value_char(c)) {
                if (p->header_line_len >= sizeof(p->header_line) - 1) {
                    p->state = HTTP_STATE_ERROR; /* header line too long, this is also what catches a value that never ends */
                    break;
                }
                p->header_line[p->header_line_len++] = (char)c;
                p->header_line[p->header_line_len] = '\0';
            } else {
                p->state = HTTP_STATE_ERROR;
            }
            break;

        case HTTP_STATE_HEADER_LF:
            if (c == '\n') {
                if (header_name_is(p, "Connection")) {
                    note_connection_header(p);
                }
                p->header_count++;
                p->state = HTTP_STATE_HEADER_LINE_START;
            } else {
                p->state = HTTP_STATE_ERROR;
            }
            break;

        case HTTP_STATE_HEADERS_END_LF:
            p->state = (c == '\n') ? HTTP_STATE_DONE : HTTP_STATE_ERROR;
            break;

        case HTTP_STATE_DONE:
        case HTTP_STATE_ERROR:
            /* Unreachable: guarded at function entry, and every other
             * case returns immediately below before looping back here. */
            break;
        }

        if (advance) {
            i++;
        }

        if (p->state == HTTP_STATE_ERROR) {
            *consumed = i;
            return HTTP_PARSE_ERROR;
        }
        if (p->state == HTTP_STATE_DONE) {
            /* Any bytes left in data[i..len) belong to whatever comes
             * next on this connection: a pipelined request if the caller
             * supports keep-alive, or a body this layer still doesn't
             * parse. *consumed tells the caller exactly where this
             * request ended so it can feed the remainder to a freshly
             * reset parser instead of losing or misparsing it. */
            *consumed = i;
            return HTTP_PARSE_COMPLETE;
        }
    }

    *consumed = len;
    return HTTP_PARSE_INCOMPLETE;
}

bool http_parser_should_keep_alive(const http_request_parser_t *p) {
    if (p->connection_directive == HTTP_CONN_CLOSE) {
        return false;
    }
    if (p->connection_directive == HTTP_CONN_KEEPALIVE) {
        return true;
    }
    return p->http_major == 1 && p->http_minor >= 1;
}
