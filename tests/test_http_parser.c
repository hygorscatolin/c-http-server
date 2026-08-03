/* Standalone unit tests for the http_parser state machine. No external
 * test framework: the project has zero dependencies so far and pulling
 * one in for a handful of assertions isn't worth it. */

#include <stdio.h>
#include <string.h>

#include "http_parser.h"

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        g_tests_run++;                                                                                               \
        if (!(cond)) {                                                                                               \
            g_tests_failed++;                                                                                        \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                        \
        }                                                                                                            \
    } while (0)

#define RUN(test_fn)                                                                                                 \
    do {                                                                                                             \
        printf("%s\n", #test_fn);                                                                                    \
        test_fn();                                                                                                   \
    } while (0)

static void test_simple_get_no_headers(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET / HTTP/1.1\r\n\r\n";
    http_parse_result_t r = http_parser_execute(&p, req, strlen(req));

    CHECK(r == HTTP_PARSE_COMPLETE);
    CHECK(p.method_id == HTTP_METHOD_GET);
    CHECK(p.path_len == 1 && memcmp(p.path, "/", 1) == 0);
    CHECK(p.header_count == 0);
}

static void test_get_with_headers(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nUser-Agent: test\r\n\r\n";
    http_parse_result_t r = http_parser_execute(&p, req, strlen(req));

    CHECK(r == HTTP_PARSE_COMPLETE);
    CHECK(p.method_id == HTTP_METHOD_GET);
    CHECK(p.path_len == 6 && memcmp(p.path, "/hello", 6) == 0);
    CHECK(p.header_count == 2);
}

static void test_post_method_recognized(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "POST /hello HTTP/1.1\r\n\r\n";
    http_parse_result_t r = http_parser_execute(&p, req, strlen(req));

    CHECK(r == HTTP_PARSE_COMPLETE);
    CHECK(p.method_id == HTTP_METHOD_POST);
}

/* The whole point of an incremental parser: feeding one byte per call
 * must produce the exact same result as feeding it all at once. */
static void test_partial_byte_by_byte(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t len = strlen(req);
    http_parse_result_t r = HTTP_PARSE_INCOMPLETE;

    for (size_t i = 0; i < len; i++) {
        r = http_parser_execute(&p, req + i, 1);
        if (i + 1 < len) {
            CHECK(r == HTTP_PARSE_INCOMPLETE);
        }
    }

    CHECK(r == HTTP_PARSE_COMPLETE);
    CHECK(p.method_id == HTTP_METHOD_GET);
    CHECK(p.path_len == 6 && memcmp(p.path, "/hello", 6) == 0);
    CHECK(p.header_count == 1);
}

/* Same request, but split at points that land mid-token and mid-CRLF
 * (right between '\r' and '\n'), which is exactly where a naive
 * line-buffering parser tends to break. */
static void test_partial_arbitrary_chunks(void) {
    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nX-A: 1\r\n\r\n";
    size_t len = strlen(req);

    /* Split offsets chosen by hand against the string above: mid method
     * ("GE"|"T ..."), mid version ("...HTTP"|"/1.1..."), right between
     * the request line's '\r' and '\n', right after "Host: " (name and
     * OWS consumed, value not started), and mid header name ("X-"|"A: 1"). */
    size_t splits[] = {2, 15, 20, 27, 40};
    size_t num_splits = sizeof(splits) / sizeof(splits[0]);

    http_request_parser_t p;
    http_parser_init(&p);

    size_t offset = 0;
    http_parse_result_t r = HTTP_PARSE_INCOMPLETE;
    for (size_t s = 0; s <= num_splits; s++) {
        size_t end = (s < num_splits) ? splits[s] : len;
        size_t chunk_len = end - offset;
        if (chunk_len > 0) {
            r = http_parser_execute(&p, req + offset, chunk_len);
        }
        offset = end;
    }

    CHECK(r == HTTP_PARSE_COMPLETE);
    CHECK(p.method_id == HTTP_METHOD_GET);
    CHECK(p.path_len == 6 && memcmp(p.path, "/hello", 6) == 0);
    CHECK(p.header_count == 2);
}

static void test_incomplete_waits_for_more_data(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\n";
    http_parse_result_t r = http_parser_execute(&p, req, strlen(req));

    /* No blank line yet, must not be COMPLETE or ERROR. */
    CHECK(r == HTTP_PARSE_INCOMPLETE);

    r = http_parser_execute(&p, "\r\n", 2);
    CHECK(r == HTTP_PARSE_COMPLETE);
}

static void test_oversized_method(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    char req[64];
    size_t n = 0;
    for (int i = 0; i < 30; i++) {
        req[n++] = 'A';
    }
    req[n++] = ' ';
    req[n++] = '/';
    memcpy(req + n, " HTTP/1.1\r\n\r\n", 13);
    n += 13;

    http_parse_result_t r = http_parser_execute(&p, req, n);
    CHECK(r == HTTP_PARSE_ERROR);
}

static void test_oversized_path(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    char req[HTTP_PATH_MAX + 64];
    size_t n = 0;
    memcpy(req, "GET /", 5);
    n += 5;
    for (size_t i = 0; i < HTTP_PATH_MAX; i++) {
        req[n++] = 'a';
    }
    memcpy(req + n, " HTTP/1.1\r\n\r\n", 13);
    n += 13;

    http_parse_result_t r = http_parser_execute(&p, req, n);
    CHECK(r == HTTP_PARSE_ERROR);
}

/* "Header sem fim": a header value that keeps growing and never sees a
 * CRLF. Must be rejected once it crosses the line-length limit rather
 * than accepted, or worse, waited on forever. */
static void test_header_line_without_terminator_exceeds_limit(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *prefix = "GET / HTTP/1.1\r\nX-Long: ";
    http_parse_result_t r = http_parser_execute(&p, prefix, strlen(prefix));
    CHECK(r == HTTP_PARSE_INCOMPLETE);

    char filler[HTTP_HEADER_LINE_MAX + 16];
    memset(filler, 'z', sizeof(filler));
    r = http_parser_execute(&p, filler, sizeof(filler));

    CHECK(r == HTTP_PARSE_ERROR);
}

/* Same scenario, but staying under the limit: a header line with no
 * CRLF yet must be INCOMPLETE, not an error. Distinguishes "not done"
 * from "malformed". */
static void test_header_line_without_terminator_under_limit_is_incomplete(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET / HTTP/1.1\r\nX-Partial: still-going";
    http_parse_result_t r = http_parser_execute(&p, req, strlen(req));

    CHECK(r == HTTP_PARSE_INCOMPLETE);
}

static void test_too_many_headers(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *request_line = "GET / HTTP/1.1\r\n";
    http_parse_result_t r = http_parser_execute(&p, request_line, strlen(request_line));
    CHECK(r == HTTP_PARSE_INCOMPLETE);

    /* One more header than the limit allows, the loop stops as soon as
     * the parser stops saying INCOMPLETE. */
    for (int i = 0; i < HTTP_MAX_HEADERS + 1 && r == HTTP_PARSE_INCOMPLETE; i++) {
        char hdr[32];
        int hlen = snprintf(hdr, sizeof(hdr), "X-%d: 1\r\n", i);
        r = http_parser_execute(&p, hdr, (size_t)hlen);
    }

    CHECK(r == HTTP_PARSE_ERROR);
}

static void test_invalid_byte_in_method(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    /* Embedded NUL and control byte, built as an explicit byte array
     * since a C string literal can't carry a NUL through strlen(). */
    unsigned char req[] = {'G', 'E', 0x01, 'T', ' ', '/', ' ', 'H', 'T', 'T', 'P', '/', '1', '.', '1', '\r', '\n', '\r', '\n'};

    http_parse_result_t r = http_parser_execute(&p, (const char *)req, sizeof(req));
    CHECK(r == HTTP_PARSE_ERROR);
}

static void test_invalid_byte_in_path(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    unsigned char req[] = {'G', 'E', 'T', ' ', '/', 0x00, 'x', ' ', 'H', 'T', 'T', 'P', '/', '1', '.',
                            '1', '\r', '\n', '\r', '\n'};

    http_parse_result_t r = http_parser_execute(&p, (const char *)req, sizeof(req));
    CHECK(r == HTTP_PARSE_ERROR);
}

static void test_bare_lf_instead_of_crlf_is_rejected(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET / HTTP/1.1\nHost: x\r\n\r\n";
    http_parse_result_t r = http_parser_execute(&p, req, strlen(req));
    CHECK(r == HTTP_PARSE_ERROR);
}

static void test_double_space_between_method_and_path_is_rejected(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET  / HTTP/1.1\r\n\r\n";
    http_parse_result_t r = http_parser_execute(&p, req, strlen(req));
    CHECK(r == HTTP_PARSE_ERROR);
}

static void test_unknown_version_is_rejected(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET / HTTP/xyz\r\n\r\n";
    http_parse_result_t r = http_parser_execute(&p, req, strlen(req));
    CHECK(r == HTTP_PARSE_ERROR);
}

static void test_http_1_0_is_syntactically_accepted(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET / HTTP/1.0\r\n\r\n";
    http_parse_result_t r = http_parser_execute(&p, req, strlen(req));
    CHECK(r == HTTP_PARSE_COMPLETE);
}

static void test_execute_after_error_stays_error(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *bad = "get / HTTP/1.1\r\n\r\n"; /* lowercase method: rejected */
    CHECK(http_parser_execute(&p, bad, strlen(bad)) == HTTP_PARSE_ERROR);
    CHECK(http_parser_execute(&p, "more data", 9) == HTTP_PARSE_ERROR);
}

static void test_execute_after_complete_stays_complete(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET / HTTP/1.1\r\n\r\n";
    CHECK(http_parser_execute(&p, req, strlen(req)) == HTTP_PARSE_COMPLETE);
    CHECK(http_parser_execute(&p, "GET /again HTTP/1.1\r\n\r\n", 24) == HTTP_PARSE_COMPLETE);
}

int main(void) {
    RUN(test_simple_get_no_headers);
    RUN(test_get_with_headers);
    RUN(test_post_method_recognized);
    RUN(test_partial_byte_by_byte);
    RUN(test_partial_arbitrary_chunks);
    RUN(test_incomplete_waits_for_more_data);
    RUN(test_oversized_method);
    RUN(test_oversized_path);
    RUN(test_header_line_without_terminator_exceeds_limit);
    RUN(test_header_line_without_terminator_under_limit_is_incomplete);
    RUN(test_too_many_headers);
    RUN(test_invalid_byte_in_method);
    RUN(test_invalid_byte_in_path);
    RUN(test_bare_lf_instead_of_crlf_is_rejected);
    RUN(test_double_space_between_method_and_path_is_rejected);
    RUN(test_unknown_version_is_rejected);
    RUN(test_http_1_0_is_syntactically_accepted);
    RUN(test_execute_after_error_stays_error);
    RUN(test_execute_after_complete_stays_complete);

    printf("\n%d checks run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
