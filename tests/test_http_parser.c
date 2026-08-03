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

/* Most tests don't care how many bytes were consumed, only the result.
 * This hides the out-param for them; tests that do care about consumed
 * (pipelining, keep-alive reuse) call http_parser_execute directly. */
static http_parse_result_t exec_all(http_request_parser_t *p, const char *data, size_t len) {
    size_t consumed = 0;
    return http_parser_execute(p, data, len, &consumed);
}

static void test_simple_get_no_headers(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET / HTTP/1.1\r\n\r\n";
    http_parse_result_t r = exec_all(&p, req, strlen(req));

    CHECK(r == HTTP_PARSE_COMPLETE);
    CHECK(p.method_id == HTTP_METHOD_GET);
    CHECK(p.path_len == 1 && memcmp(p.path, "/", 1) == 0);
    CHECK(p.header_count == 0);
}

static void test_get_with_headers(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nUser-Agent: test\r\n\r\n";
    http_parse_result_t r = exec_all(&p, req, strlen(req));

    CHECK(r == HTTP_PARSE_COMPLETE);
    CHECK(p.method_id == HTTP_METHOD_GET);
    CHECK(p.path_len == 6 && memcmp(p.path, "/hello", 6) == 0);
    CHECK(p.header_count == 2);
}

static void test_post_method_recognized(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "POST /hello HTTP/1.1\r\n\r\n";
    http_parse_result_t r = exec_all(&p, req, strlen(req));

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
        r = exec_all(&p, req + i, 1);
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
            r = exec_all(&p, req + offset, chunk_len);
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
    http_parse_result_t r = exec_all(&p, req, strlen(req));

    /* No blank line yet, must not be COMPLETE or ERROR. */
    CHECK(r == HTTP_PARSE_INCOMPLETE);

    r = exec_all(&p, "\r\n", 2);
    CHECK(r == HTTP_PARSE_COMPLETE);
}

/* *consumed must equal len whenever the parser reports INCOMPLETE:
 * there is nothing left over to hand a "next request" if this one
 * hasn't even finished. */
static void test_consumed_equals_len_when_incomplete(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET /hello HTTP/1.1\r\nHost: x\r\n";
    size_t len = strlen(req);
    size_t consumed = 0;
    http_parse_result_t r = http_parser_execute(&p, req, len, &consumed);

    CHECK(r == HTTP_PARSE_INCOMPLETE);
    CHECK(consumed == len);
}

/* Two full requests handed to execute() in a single call, as they'd
 * arrive from one read() on a pipelining client. The parser must report
 * exactly where the first one ends so the caller can reset and feed it
 * the remainder, this is what makes keep-alive with pipelining possible
 * without losing or corrupting the second request's bytes. */
static void test_consumed_reports_leftover_for_pipelined_bytes(void) {
    const char *first = "GET / HTTP/1.1\r\n\r\n";
    const char *second = "GET /hello HTTP/1.1\r\n\r\n";
    char buf[128];
    size_t first_len = strlen(first);
    size_t second_len = strlen(second);
    memcpy(buf, first, first_len);
    memcpy(buf + first_len, second, second_len);
    size_t total_len = first_len + second_len;

    http_request_parser_t p;
    http_parser_init(&p);
    size_t consumed = 0;
    http_parse_result_t r = http_parser_execute(&p, buf, total_len, &consumed);

    CHECK(r == HTTP_PARSE_COMPLETE);
    CHECK(consumed == first_len);
    CHECK(p.path_len == 1 && memcmp(p.path, "/", 1) == 0);

    /* Simulates event_loop.c's process_input(): reset for the next
     * request and feed it exactly the leftover bytes. */
    http_parser_init(&p);
    size_t second_consumed = 0;
    r = http_parser_execute(&p, buf + consumed, total_len - consumed, &second_consumed);

    CHECK(r == HTTP_PARSE_COMPLETE);
    CHECK(second_consumed == second_len);
    CHECK(p.path_len == 6 && memcmp(p.path, "/hello", 6) == 0);
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

    http_parse_result_t r = exec_all(&p, req, n);
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

    http_parse_result_t r = exec_all(&p, req, n);
    CHECK(r == HTTP_PARSE_ERROR);
}

/* "Header sem fim": a header value that keeps growing and never sees a
 * CRLF. Must be rejected once it crosses the line-length limit rather
 * than accepted, or worse, waited on forever. */
static void test_header_line_without_terminator_exceeds_limit(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *prefix = "GET / HTTP/1.1\r\nX-Long: ";
    http_parse_result_t r = exec_all(&p, prefix, strlen(prefix));
    CHECK(r == HTTP_PARSE_INCOMPLETE);

    char filler[HTTP_HEADER_LINE_MAX + 16];
    memset(filler, 'z', sizeof(filler));
    r = exec_all(&p, filler, sizeof(filler));

    CHECK(r == HTTP_PARSE_ERROR);
}

/* Same scenario, but staying under the limit: a header line with no
 * CRLF yet must be INCOMPLETE, not an error. Distinguishes "not done"
 * from "malformed". */
static void test_header_line_without_terminator_under_limit_is_incomplete(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET / HTTP/1.1\r\nX-Partial: still-going";
    http_parse_result_t r = exec_all(&p, req, strlen(req));

    CHECK(r == HTTP_PARSE_INCOMPLETE);
}

static void test_too_many_headers(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *request_line = "GET / HTTP/1.1\r\n";
    http_parse_result_t r = exec_all(&p, request_line, strlen(request_line));
    CHECK(r == HTTP_PARSE_INCOMPLETE);

    /* One more header than the limit allows, the loop stops as soon as
     * the parser stops saying INCOMPLETE. */
    for (int i = 0; i < HTTP_MAX_HEADERS + 1 && r == HTTP_PARSE_INCOMPLETE; i++) {
        char hdr[32];
        int hlen = snprintf(hdr, sizeof(hdr), "X-%d: 1\r\n", i);
        r = exec_all(&p, hdr, (size_t)hlen);
    }

    CHECK(r == HTTP_PARSE_ERROR);
}

static void test_invalid_byte_in_method(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    /* Embedded NUL and control byte, built as an explicit byte array
     * since a C string literal can't carry a NUL through strlen(). */
    unsigned char req[] = {'G', 'E', 0x01, 'T', ' ', '/', ' ', 'H', 'T', 'T', 'P', '/', '1', '.', '1', '\r', '\n', '\r', '\n'};

    http_parse_result_t r = exec_all(&p, (const char *)req, sizeof(req));
    CHECK(r == HTTP_PARSE_ERROR);
}

static void test_invalid_byte_in_path(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    unsigned char req[] = {'G', 'E', 'T', ' ', '/', 0x00, 'x', ' ', 'H', 'T', 'T', 'P', '/', '1', '.',
                            '1', '\r', '\n', '\r', '\n'};

    http_parse_result_t r = exec_all(&p, (const char *)req, sizeof(req));
    CHECK(r == HTTP_PARSE_ERROR);
}

static void test_bare_lf_instead_of_crlf_is_rejected(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET / HTTP/1.1\nHost: x\r\n\r\n";
    http_parse_result_t r = exec_all(&p, req, strlen(req));
    CHECK(r == HTTP_PARSE_ERROR);
}

static void test_double_space_between_method_and_path_is_rejected(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET  / HTTP/1.1\r\n\r\n";
    http_parse_result_t r = exec_all(&p, req, strlen(req));
    CHECK(r == HTTP_PARSE_ERROR);
}

static void test_unknown_version_is_rejected(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET / HTTP/xyz\r\n\r\n";
    http_parse_result_t r = exec_all(&p, req, strlen(req));
    CHECK(r == HTTP_PARSE_ERROR);
}

static void test_http_1_0_is_syntactically_accepted(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET / HTTP/1.0\r\n\r\n";
    http_parse_result_t r = exec_all(&p, req, strlen(req));
    CHECK(r == HTTP_PARSE_COMPLETE);
    CHECK(p.http_major == 1 && p.http_minor == 0);
}

static void test_execute_after_error_stays_error(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *bad = "get / HTTP/1.1\r\n\r\n"; /* lowercase method: rejected */
    CHECK(exec_all(&p, bad, strlen(bad)) == HTTP_PARSE_ERROR);
    CHECK(exec_all(&p, "more data", 9) == HTTP_PARSE_ERROR);
}

static void test_execute_after_complete_stays_complete(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req = "GET / HTTP/1.1\r\n\r\n";
    CHECK(exec_all(&p, req, strlen(req)) == HTTP_PARSE_COMPLETE);
    CHECK(exec_all(&p, "GET /again HTTP/1.1\r\n\r\n", 24) == HTTP_PARSE_COMPLETE);
}

/* --- keep-alive negotiation (RFC 7230 section 6.3) --- */

static void test_http11_defaults_to_keep_alive(void) {
    http_request_parser_t p;
    http_parser_init(&p);
    const char *req = "GET / HTTP/1.1\r\n\r\n";
    CHECK(exec_all(&p, req, strlen(req)) == HTTP_PARSE_COMPLETE);
    CHECK(http_parser_should_keep_alive(&p) == true);
}

static void test_http11_connection_close_overrides_default(void) {
    http_request_parser_t p;
    http_parser_init(&p);
    const char *req = "GET / HTTP/1.1\r\nConnection: close\r\n\r\n";
    CHECK(exec_all(&p, req, strlen(req)) == HTTP_PARSE_COMPLETE);
    CHECK(http_parser_should_keep_alive(&p) == false);
}

static void test_http10_defaults_to_close(void) {
    http_request_parser_t p;
    http_parser_init(&p);
    const char *req = "GET / HTTP/1.0\r\n\r\n";
    CHECK(exec_all(&p, req, strlen(req)) == HTTP_PARSE_COMPLETE);
    CHECK(http_parser_should_keep_alive(&p) == false);
}

static void test_http10_connection_keepalive_overrides_default(void) {
    http_request_parser_t p;
    http_parser_init(&p);
    const char *req = "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
    CHECK(exec_all(&p, req, strlen(req)) == HTTP_PARSE_COMPLETE);
    CHECK(http_parser_should_keep_alive(&p) == true);
}

/* Header field names are case-insensitive (RFC 7230 3.2), and so are
 * the connection-option tokens themselves (RFC 7230 6.1). */
static void test_connection_header_is_case_insensitive(void) {
    http_request_parser_t p;
    http_parser_init(&p);
    const char *req = "GET / HTTP/1.1\r\nCONNECTION: CLOSE\r\n\r\n";
    CHECK(exec_all(&p, req, strlen(req)) == HTTP_PARSE_COMPLETE);
    CHECK(http_parser_should_keep_alive(&p) == false);
}

/* A Connection header that names neither token (a typo, or an option
 * this server doesn't recognize) must not accidentally flip the
 * decision, the version's own default still applies. */
static void test_unrecognized_connection_token_keeps_default(void) {
    http_request_parser_t p;
    http_parser_init(&p);
    const char *req = "GET / HTTP/1.1\r\nConnection: upgrade\r\n\r\n";
    CHECK(exec_all(&p, req, strlen(req)) == HTTP_PARSE_COMPLETE);
    CHECK(http_parser_should_keep_alive(&p) == true); /* HTTP/1.1 default, unaffected */
}

/* Simulates exactly what event_loop.c does on a persistent connection:
 * reset the same parser struct after one request completes and reuse it
 * for the next one. Nothing from the first request (method, path,
 * header count, Connection directive) may leak into the second. */
static void test_parser_reset_does_not_leak_state_between_requests(void) {
    http_request_parser_t p;
    http_parser_init(&p);

    const char *req1 = "POST /first HTTP/1.1\r\nConnection: close\r\nX-A: 1\r\nX-B: 2\r\n\r\n";
    CHECK(exec_all(&p, req1, strlen(req1)) == HTTP_PARSE_COMPLETE);
    CHECK(p.method_id == HTTP_METHOD_POST);
    CHECK(p.header_count == 3);
    CHECK(http_parser_should_keep_alive(&p) == false);

    http_parser_init(&p);
    const char *req2 = "GET /second HTTP/1.1\r\n\r\n";
    CHECK(exec_all(&p, req2, strlen(req2)) == HTTP_PARSE_COMPLETE);
    CHECK(p.method_id == HTTP_METHOD_GET);
    CHECK(p.path_len == 7 && memcmp(p.path, "/second", 7) == 0);
    CHECK(p.header_count == 0);
    /* No Connection header this time, must fall back to the HTTP/1.1
     * default rather than remembering req1's "close". */
    CHECK(http_parser_should_keep_alive(&p) == true);
}

int main(void) {
    RUN(test_simple_get_no_headers);
    RUN(test_get_with_headers);
    RUN(test_post_method_recognized);
    RUN(test_partial_byte_by_byte);
    RUN(test_partial_arbitrary_chunks);
    RUN(test_incomplete_waits_for_more_data);
    RUN(test_consumed_equals_len_when_incomplete);
    RUN(test_consumed_reports_leftover_for_pipelined_bytes);
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
    RUN(test_http11_defaults_to_keep_alive);
    RUN(test_http11_connection_close_overrides_default);
    RUN(test_http10_defaults_to_close);
    RUN(test_http10_connection_keepalive_overrides_default);
    RUN(test_connection_header_is_case_insensitive);
    RUN(test_unrecognized_connection_token_keeps_default);
    RUN(test_parser_reset_does_not_leak_state_between_requests);

    printf("\n%d checks run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
