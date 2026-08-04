#!/usr/bin/env python3
"""Shared plumbing for the integration suites that drive the real server
over a real socket (tests/test_keepalive.py, tests/test_static_files.py).

Everything here exists because raw TCP has no notion of "one HTTP
response": where a response ends is something the test has to work out
from Content-Length, not something a single recv() will hand over.
"""

import contextlib
import os
import signal
import socket
import subprocess
import sys
import time

SERVER_BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "http-server")
HOST = "127.0.0.1"
PORT = 8080

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


def body_of(response: bytes) -> bytes:
    return response.split(b"\r\n\r\n", 1)[1]


def connect(recv_buffer_size=None):
    """Opens a client socket, optionally with a deliberately small
    receive buffer. Shrinking it is how a test forces the server's socket
    send buffer to fill up, which is the only way to reach the EAGAIN /
    EPOLLOUT path on a loopback connection.

    SO_RCVBUF has to be set before connect(), the window scaling factor
    is negotiated in the handshake and can't be revised afterwards.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if recv_buffer_size is not None:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, recv_buffer_size)
    sock.connect((HOST, PORT))
    return sock


@contextlib.contextmanager
def running_server(env_extra=None):
    """Starts the compiled server and guarantees it's reaped, however the
    tests exit. Output is captured so a crash (or an AddressSanitizer
    report, which is the whole point of building with it) can be printed
    with the failures instead of being interleaved into them.
    """
    env = os.environ.copy()
    env.update(env_extra or {})

    proc = subprocess.Popen([SERVER_BIN], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        time.sleep(0.4)
        if proc.poll() is not None:
            print("server exited immediately, aborting", file=sys.stderr)
            print(proc.stdout.read().decode(errors="replace"), file=sys.stderr)
            sys.exit(1)
        yield proc
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def report():
    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED:")
        for f in failures:
            print(f"  - {f}")
        sys.exit(1)
    print("all checks passed")
