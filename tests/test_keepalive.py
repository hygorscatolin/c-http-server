#!/usr/bin/env python3
"""Integration tests for layer 4: keep-alive negotiation, partial-write
buffering staying transparent to the client, and the Slowloris idle
timeout. These need a real running server (raw TCP timing and multiple
requests per socket aren't things the parser unit tests can exercise),
so unlike tests/test_http_parser.c this drives the actual compiled
binary instead of linking against the source directly.
"""

import socket
import time

from http_test_utils import HOST, PORT, ResponseReader, check, header_value, parse_status, report, running_server

# Kept short so the idle-timeout tests don't make the suite slow. The
# server reads this from the environment, see main.c.
IDLE_TIMEOUT_SECONDS = 2
# Must match IDLE_SWEEP_INTERVAL_SECONDS in event_loop.c: the sweep is
# periodic and not driven by any single connection's deadline, so a
# connection can wait up to one full interval past idle_timeout_seconds
# before anyone notices it. Not configurable via environment, hence the
# duplicated literal here rather than reading it back from the server.
SWEEP_INTERVAL_SECONDS = 5


def test_http11_keep_alive_serves_two_requests_on_one_socket():
    print("test_http11_keep_alive_serves_two_requests_on_one_socket")
    with socket.create_connection((HOST, PORT)) as s:
        reader = ResponseReader(s)
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        r1 = reader.read_one()
        check(parse_status(r1) == 200, "first response is 200")
        check(header_value(r1, "Connection") == "keep-alive", "first response says Connection: keep-alive")

        # If the server had closed after the first response this second
        # send would raise BrokenPipeError/ConnectionResetError, or
        # read_one() below would raise ConnectionError (EOF) instead of
        # returning a real response.
        s.sendall(b"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n")
        r2 = reader.read_one()
        check(parse_status(r2) == 200, "second response on the same socket is 200")
        check(b"Hello, hello!" in r2, "second response routed to /hello")


def test_connection_close_header_closes_after_one_response():
    print("test_connection_close_header_closes_after_one_response")
    with socket.create_connection((HOST, PORT)) as s:
        reader = ResponseReader(s)
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        r1 = reader.read_one()
        check(parse_status(r1) == 200, "response is 200")
        check(header_value(r1, "Connection") == "close", "response echoes Connection: close")

        # Server should have closed its end already: reading further
        # must see EOF, not hang and not return another response.
        check(reader.expect_eof(), "socket is closed after Connection: close")


def test_http10_defaults_to_close():
    print("test_http10_defaults_to_close")
    with socket.create_connection((HOST, PORT)) as s:
        reader = ResponseReader(s)
        s.sendall(b"GET / HTTP/1.0\r\n\r\n")
        r1 = reader.read_one()
        check(parse_status(r1) == 200, "response is 200")
        check(header_value(r1, "Connection") == "close", "HTTP/1.0 with no header defaults to close")
        check(reader.expect_eof(), "socket is closed after an HTTP/1.0 request with no Connection header")


def test_http10_connection_keepalive_stays_open():
    print("test_http10_connection_keepalive_stays_open")
    with socket.create_connection((HOST, PORT)) as s:
        reader = ResponseReader(s)
        s.sendall(b"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
        r1 = reader.read_one()
        check(parse_status(r1) == 200, "response is 200")
        check(header_value(r1, "Connection") == "keep-alive", "HTTP/1.0 + Connection: keep-alive is honored")

        s.sendall(b"GET /hello HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
        r2 = reader.read_one()
        check(parse_status(r2) == 200, "second HTTP/1.0 keep-alive request still works")


def test_malformed_request_closes_even_if_keep_alive_requested():
    print("test_malformed_request_closes_even_if_keep_alive_requested")
    with socket.create_connection((HOST, PORT)) as s:
        reader = ResponseReader(s)
        # Lowercase method is malformed per the parser's grammar. A
        # Connection header can't be trusted once framing itself broke.
        s.sendall(b"get / HTTP/1.1\r\nConnection: keep-alive\r\n\r\n")
        r1 = reader.read_one()
        check(parse_status(r1) == 400, "malformed request gets 400")
        check(reader.expect_eof(), "connection is closed after a 400 regardless of the Connection header")


def test_pipelined_requests_on_one_connection():
    print("test_pipelined_requests_on_one_connection")
    with socket.create_connection((HOST, PORT)) as s:
        reader = ResponseReader(s)
        # Both requests sent back-to-back in a single write(), simulating
        # a client that pipelines rather than waiting for each response.
        # A single recv() on the client side can legitimately return
        # both responses concatenated, or split them anywhere, that's
        # exactly why ResponseReader exists instead of one recv() per
        # expected response.
        s.sendall(b"GET / HTTP/1.1\r\n\r\nGET /hello HTTP/1.1\r\n\r\n")
        r1 = reader.read_one()
        check(parse_status(r1) == 200, "first pipelined response is 200")
        check(b"Hello from my C server!" in r1, "first pipelined response is for /")

        r2 = reader.read_one()
        check(parse_status(r2) == 200, "second pipelined response is 200")
        check(b"Hello, hello!" in r2, "second pipelined response is for /hello")


def test_idle_connection_is_closed_after_timeout():
    print("test_idle_connection_is_closed_after_timeout")
    # Worst case a connection can wait idle_timeout + one full sweep
    # interval before being reclaimed, plus slack for scheduling jitter
    # in a possibly loaded sandbox.
    upper_bound = IDLE_TIMEOUT_SECONDS + SWEEP_INTERVAL_SECONDS + 3
    with socket.create_connection((HOST, PORT)) as s:
        # Send nothing at all: the classic Slowloris opening move.
        start = time.monotonic()
        s.settimeout(upper_bound)
        data = s.recv(65536)
        elapsed = time.monotonic() - start
        check(data == b"", "server closes a connection that never sends anything")
        check(elapsed < upper_bound, f"closed within {upper_bound}s of the {IDLE_TIMEOUT_SECONDS}s timeout (took {elapsed:.1f}s)")


def test_idle_timeout_also_applies_between_keepalive_requests():
    print("test_idle_timeout_also_applies_between_keepalive_requests")
    with socket.create_connection((HOST, PORT)) as s:
        reader = ResponseReader(s)
        s.sendall(b"GET / HTTP/1.1\r\n\r\n")
        r1 = reader.read_one()
        check(parse_status(r1) == 200, "first response is 200")

        # Now go idle without sending a second request.
        check(reader.expect_eof(timeout=IDLE_TIMEOUT_SECONDS + SWEEP_INTERVAL_SECONDS + 3),
              "idle keep-alive connection is eventually closed too, not held forever")


def test_active_connection_survives_past_one_timeout_window():
    print("test_active_connection_survives_past_one_timeout_window")
    with socket.create_connection((HOST, PORT)) as s:
        reader = ResponseReader(s)
        # Keep sending requests slower than one per timeout window would
        # suggest, but always well within it, proving the idle clock
        # resets on activity instead of being an absolute connection
        # lifetime cap.
        for _ in range(3):
            time.sleep(IDLE_TIMEOUT_SECONDS * 0.6)
            s.sendall(b"GET / HTTP/1.1\r\n\r\n")
            r = reader.read_one()
            check(parse_status(r) == 200, "connection still serving requests past one timeout window's worth of elapsed time")


def main():
    with running_server({"HTTP_SERVER_IDLE_TIMEOUT_SECONDS": str(IDLE_TIMEOUT_SECONDS)}):
        test_http11_keep_alive_serves_two_requests_on_one_socket()
        test_connection_close_header_closes_after_one_response()
        test_http10_defaults_to_close()
        test_http10_connection_keepalive_stays_open()
        test_malformed_request_closes_even_if_keep_alive_requested()
        test_pipelined_requests_on_one_connection()
        test_active_connection_survives_past_one_timeout_window()
        test_idle_connection_is_closed_after_timeout()
        test_idle_timeout_also_applies_between_keepalive_requests()

    report()


if __name__ == "__main__":
    main()
