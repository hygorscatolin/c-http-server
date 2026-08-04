#!/usr/bin/env python3
"""Integration tests for layer 6: one event loop per thread, each on its
own SO_REUSEPORT listen socket.

Almost nothing about this layer is visible from inside a single request,
which is why the server tags every response with X-Worker (see
build_response in event_loop.c). That header is what turns "the kernel
distributed these connections across threads" from an assertion in a
comment into something a test can actually observe.

Two things are deliberately not asserted here. Throughput: measuring a
speedup is a benchmark, and one that would be worthless on a loaded CI
box with an unknown CPU count. And an exact worker-by-worker
distribution: which socket the kernel picks comes from hashing the
connection's 4-tuple, so the split over a few dozen connections is
random, not uniform. What is safe to require is that *more than one*
worker gets used, which over 24 connections fails by chance about once
in ten million runs.
"""

import signal
import socket
import subprocess

from http_test_utils import ResponseReader, check, connect, header_value, parse_status, report, running_server

# Fixed rather than detected, so the assertions below don't depend on how
# many CPUs the machine running the suite happens to have. This is the
# override HTTP_SERVER_WORKERS exists for, see worker_pool.c.
WORKER_COUNT = 4

# Enough connections that landing them all on one worker is essentially
# impossible (4 workers, 24 connections: 4 * (1/4)**24), while staying
# far below any fd limit.
CONCURRENT_CONNECTIONS = 24


def request_on_each_of(sockets):
    """Sends one request on every socket *before* reading any response,
    so all the connections are genuinely open and in flight at the same
    time. Doing it one socket at a time would be served correctly by a
    single-threaded server too, and would prove nothing about layer 6.
    """
    readers = [ResponseReader(s) for s in sockets]
    for s in sockets:
        s.sendall(b"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n")
    return [reader.read_one(timeout=5.0) for reader in readers]


def test_all_concurrent_connections_are_served():
    print("test_all_concurrent_connections_are_served")
    sockets = [connect() for _ in range(CONCURRENT_CONNECTIONS)]
    try:
        responses = request_on_each_of(sockets)
        check(len(responses) == CONCURRENT_CONNECTIONS,
              f"all {CONCURRENT_CONNECTIONS} simultaneously open connections got a response")
        check(all(parse_status(r) == 200 for r in responses), "every concurrent response is 200")
        check(all(b"Hello, hello!" in r for r in responses), "every concurrent response has the right body")
    finally:
        for s in sockets:
            s.close()


def test_connections_are_spread_over_several_workers():
    print("test_connections_are_spread_over_several_workers")
    sockets = [connect() for _ in range(CONCURRENT_CONNECTIONS)]
    try:
        responses = request_on_each_of(sockets)
        workers = [header_value(r, "X-Worker") for r in responses]
        check(all(w is not None for w in workers), "every response identifies the worker that produced it")

        distinct = sorted(set(w for w in workers if w is not None), key=int)
        print(f"  (workers seen: {distinct})")
        check(len(distinct) > 1, f"more than one worker served the {CONCURRENT_CONNECTIONS} connections")
        check(all(0 <= int(w) < WORKER_COUNT for w in distinct),
              f"every worker id is in range 0..{WORKER_COUNT - 1}")
    finally:
        for s in sockets:
            s.close()


def test_process_really_runs_one_thread_per_worker(proc):
    print("test_process_really_runs_one_thread_per_worker")
    # The header could in principle be produced by one thread pretending
    # to be several, so check the kernel's own count too. The main thread
    # is parked in pthread_join for the life of the server, hence the +1.
    with open(f"/proc/{proc.pid}/status", encoding="ascii") as f:
        threads = next(int(line.split()[1]) for line in f if line.startswith("Threads:"))
    print(f"  (kernel reports {threads} threads)")
    check(threads >= WORKER_COUNT + 1, f"process has at least {WORKER_COUNT} worker threads plus main")


def test_keepalive_connection_stays_on_one_worker():
    print("test_keepalive_connection_stays_on_one_worker")
    # The kernel hashes the connection's 4-tuple, which does not change
    # over the life of a connection, so every request on one socket must
    # come back from the same worker. This is not a nicety: the whole
    # reason event_loop.c can keep per-connection state without a lock is
    # that a connection belongs to exactly one thread from accept to
    # close.
    with connect() as s:
        reader = ResponseReader(s)
        seen = []
        for _ in range(5):
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            r = reader.read_one()
            check(parse_status(r) == 200, "keep-alive request on a pooled server is 200")
            seen.append(header_value(r, "X-Worker"))
        check(len(set(seen)) == 1, f"all requests on one connection came from the same worker (saw {sorted(set(seen))})")


def test_static_files_still_work_under_the_pool():
    print("test_static_files_still_work_under_the_pool")
    # static_files_init() resolves the document root once before any
    # thread exists and every worker only ever reads it afterwards. If
    # that were not so, this is where it would show up.
    sockets = [connect() for _ in range(8)]
    try:
        readers = [ResponseReader(s) for s in sockets]
        for s in sockets:
            s.sendall(b"GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n")
        responses = [reader.read_one(timeout=5.0) for reader in readers]
        check(all(parse_status(r) == 200 for r in responses), "concurrent static file requests are all 200")
        bodies = set(r.split(b"\r\n\r\n", 1)[1] for r in responses)
        check(len(bodies) == 1, "every worker served identical bytes for the same file")
    finally:
        for s in sockets:
            s.close()


def test_single_worker_configuration():
    print("test_single_worker_configuration")
    # The degenerate pool: still goes through the whole SO_REUSEPORT
    # path, so a bug there is not hidden by "N == 1 happens to work".
    with running_server({"HTTP_SERVER_WORKERS": "1"}):
        with connect() as s:
            reader = ResponseReader(s)
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            r = reader.read_one()
            check(parse_status(r) == 200, "a one-worker pool serves requests")
            check(header_value(r, "X-Worker") == "0", "the only worker is worker 0")


def test_invalid_worker_count_falls_back_to_detection():
    print("test_invalid_worker_count_falls_back_to_detection")
    # A typo in the override must not take the server down, it should
    # complain and fall back to the CPU count, see worker_pool_worker_count.
    with running_server({"HTTP_SERVER_WORKERS": "not-a-number"}) as proc:
        with connect() as s:
            reader = ResponseReader(s)
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            check(parse_status(reader.read_one()) == 200, "server still serves with an invalid HTTP_SERVER_WORKERS")
        check(proc.poll() is None, "server did not exit over an invalid HTTP_SERVER_WORKERS")


def test_sigterm_shuts_down_every_worker():
    print("test_sigterm_shuts_down_every_worker")
    with running_server({"HTTP_SERVER_WORKERS": str(WORKER_COUNT)}) as proc:
        # An open connection on the way out, so shutdown has to reclaim a
        # connection that is still registered with some worker's epoll.
        # Leaking it would be reported by AddressSanitizer at exit, which
        # is precisely what the exit code below is checking for: LSan
        # exits 23, not 0.
        lingering = connect()
        lingering.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        ResponseReader(lingering).read_one()

        proc.send_signal(signal.SIGTERM)
        try:
            code = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            code = None
        lingering.close()

        check(code == 0, f"every thread left its event loop after SIGTERM and the process exited 0 (got {code})")
        if code is not None:
            output = proc.stdout.read().decode(errors="replace")
            check("Sanitizer" not in output, "no sanitizer report on the way out")
            if "Sanitizer" in output:
                print(output)

        # Nothing should be listening any more: every worker closed its
        # own socket, not just whichever one handled the signal.
        refused = False
        try:
            connect().close()
        except (ConnectionRefusedError, socket.timeout, TimeoutError):
            refused = True
        check(refused, "no worker is still accepting connections after shutdown")


def main():
    with running_server({"HTTP_SERVER_WORKERS": str(WORKER_COUNT)}) as proc:
        test_process_really_runs_one_thread_per_worker(proc)
        test_all_concurrent_connections_are_served()
        test_connections_are_spread_over_several_workers()
        test_keepalive_connection_stays_on_one_worker()
        test_static_files_still_work_under_the_pool()

    # These three need a server of their own: different configuration, or
    # a shutdown they drive themselves.
    test_single_worker_configuration()
    test_invalid_worker_count_falls_back_to_detection()
    test_sigterm_shuts_down_every_worker()

    report()


if __name__ == "__main__":
    main()
