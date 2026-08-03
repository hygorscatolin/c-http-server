#!/usr/bin/env python3
"""Integration tests for layer 4: keep-alive negotiation, partial-write
buffering staying transparent to the client, and the Slowloris idle
timeout. These need a real running server (raw TCP timing and multiple
requests per socket aren't things the parser unit tests can exercise),
so unlike tests/test_http_parser.c this drives the actual compiled
binary instead of linking against the source directly.
"""

import os
import signal
import socket
import subprocess
import sys
import time

SERVER_BIN = os.path.join(os.path.dirname(__file__), "..", "http-server")
HOST = "127.0.0.1"
PORT = 8080
# Kept short so the idle-timeout tests don't make the suite slow. The
# server reads this from the environment, see main.c.
IDLE_TIMEOUT_SECONDS = 2
# Must match IDLE_SWEEP_INTERVAL_SECONDS in event_loop.c: the sweep is
# periodic and not driven by any single connection's deadline, so a
# connection can wait up to one full interval past idle_timeout_seconds
# before anyone notices it. Not configurable via environment, hence the
# duplicated literal here rather than reading it back from the server.
SWEEP_INTERVAL_SECONDS = 5

failures = []


def check(condition, description):
    status = "ok" if condition else "FAIL"
    print(f"  [{status}] {description}")
    if not condition:
        failures.append(description)


class ResponseReader:
    """Reads exactly one HTTP response at a time off a socket, using
    Content-Length to find the boundary. Needed because recv() has no
    notion of "one response": on a pipelined or keep-alive connection a
    single recv() can return two responses concatenated, or half of one,
    depending entirely on TCP segmentation and timing, not on anything
    the server does. A naive one-recv-per-response test would be racy.
    """

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def _fill(self, timeout):
        self.sock.settimeout(timeout)
        chunk = self.sock.recv(65536)
        if chunk == b"":
            return False
        self.buf += chunk
        return True

    def read_one(self, timeout=2.0):
        while b"\r\n\r\n" not in self.buf:
            if not self._fill(timeout):
                raise ConnectionError("peer closed before headers completed")
        head_end = self.buf.index(b"\r\n\r\n") + 4
        content_length = int(header_value(self.buf[:head_end], "Content-Length") or "0")
        total_len = head_end + content_length
        while len(self.buf) < total_len:
            if not self._fill(timeout):
                raise ConnectionError("peer closed before body completed")
        response, self.buf = self.buf[:total_len], self.buf[total_len:]
        return response

    def expect_eof(self, timeout=2.0):
        if self.buf:
            return False
        self.sock.settimeout(timeout)
        try:
            chunk = self.sock.recv(65536)
        except (socket.timeout, TimeoutError):
            # Didn't close within the window: still open, which for this
            # check is exactly as much a failure as getting real data
            # back would be. Report it that way instead of crashing the
            # whole suite on an uncaught exception.
            return False
        return chunk == b""


def parse_status(response: bytes) -> int:
    first_line = response.split(b"\r\n", 1)[0]
    return int(first_line.split(b" ")[1])


def header_value(response: bytes, name: str):
    head = response.split(b"\r\n\r\n", 1)[0]
    for line in head.split(b"\r\n")[1:]:
        if b":" not in line:
            continue
        k, v = line.split(b":", 1)
        if k.strip().lower() == name.lower().encode():
            return v.strip().decode()
    return None


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
    env = os.environ.copy()
    env["HTTP_SERVER_IDLE_TIMEOUT_SECONDS"] = str(IDLE_TIMEOUT_SECONDS)

    proc = subprocess.Popen([SERVER_BIN], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        time.sleep(0.4)
        if proc.poll() is not None:
            print("server exited immediately, aborting", file=sys.stderr)
            sys.exit(1)

        test_http11_keep_alive_serves_two_requests_on_one_socket()
        test_connection_close_header_closes_after_one_response()
        test_http10_defaults_to_close()
        test_http10_connection_keepalive_stays_open()
        test_malformed_request_closes_even_if_keep_alive_requested()
        test_pipelined_requests_on_one_connection()
        test_active_connection_survives_past_one_timeout_window()
        test_idle_connection_is_closed_after_timeout()
        test_idle_timeout_also_applies_between_keepalive_requests()
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED:")
        for f in failures:
            print(f"  - {f}")
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
