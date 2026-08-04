#!/usr/bin/env python3
"""Integration tests for layer 5: serving files out of the document root
with sendfile(), and refusing to serve anything outside it.

The path-sanitization rules themselves are pinned down in
tests/test_static_files.c, which can enumerate hostile paths far more
cheaply. What only a running server can show is the rest: that the
headers describe the bytes that actually arrive, that a body too large
for the socket buffer still comes out whole (the EAGAIN / EPOLLOUT
backpressure path through sendfile), and that a client vanishing
mid-download doesn't take the process with it.

The server is pointed at a scratch document root rather than the
repository's public/, so the suite can put things in it that don't
belong in git: a multi-megabyte file, and a symlink escaping the root.
"""

import os
import shutil
import socket
import struct
import tempfile
import time

from http_test_utils import (ResponseReader, body_of, check, connect, header_value, parse_status, report,
                             running_server)

PUBLIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "public")

# Comfortably past any plausible socket send buffer, so the server is
# guaranteed to run out of room mid-file and have to resume on EPOLLOUT.
BIG_FILE_SIZE = 4 * 1024 * 1024
# Squeezing the client's receive window makes that certain rather than
# merely likely, and keeps the server parked in the resume path long
# enough for it to matter.
SMALL_RECV_BUFFER = 4096

root = None  # scratch document root, filled in by main()


def read_public_file(relative_path):
    with open(os.path.join(root, relative_path), "rb") as f:
        return f.read()


def get(path, extra_headers=b"", method=b"GET", timeout=2.0, recv_buffer_size=None):
    """One request on its own connection, closed on the way out."""
    with connect(recv_buffer_size) as s:
        reader = ResponseReader(s)
        s.sendall(method + b" " + path + b" HTTP/1.1\r\nHost: x\r\n" + extra_headers + b"\r\n")
        return reader.read_one(timeout=timeout)


def test_html_file_is_served_with_matching_headers_and_body():
    print("test_html_file_is_served_with_matching_headers_and_body")
    expected = read_public_file("index.html")
    r = get(b"/index.html")
    check(parse_status(r) == 200, "/index.html is 200")
    check(header_value(r, "Content-Type") == "text/html; charset=utf-8", "Content-Type comes from the .html extension")
    check(header_value(r, "Content-Length") == str(len(expected)), "Content-Length is the file size from fstat()")
    check(body_of(r) == expected, "body is the file byte for byte")


def test_text_file_is_served():
    print("test_text_file_is_served")
    expected = read_public_file("hello.txt")
    r = get(b"/hello.txt")
    check(parse_status(r) == 200, "/hello.txt is 200")
    check(header_value(r, "Content-Type") == "text/plain; charset=utf-8", "Content-Type comes from the .txt extension")
    check(body_of(r) == expected, "body is the file byte for byte")


def test_nested_path_is_served():
    print("test_nested_path_is_served")
    expected = read_public_file("assets/style.css")
    r = get(b"/assets/style.css")
    check(parse_status(r) == 200, "a file in a subdirectory is served")
    check(header_value(r, "Content-Type") == "text/css; charset=utf-8", "Content-Type comes from the .css extension")
    check(body_of(r) == expected, "body is the file byte for byte")


def test_unknown_extension_is_octet_stream():
    print("test_unknown_extension_is_octet_stream")
    r = get(b"/blob.unknown")
    check(parse_status(r) == 200, "a file with an unmapped extension is still served")
    check(header_value(r, "Content-Type") == "application/octet-stream",
          "unmapped extensions default to application/octet-stream instead of being sniffed")


def test_query_string_is_ignored_when_resolving():
    print("test_query_string_is_ignored_when_resolving")
    r = get(b"/hello.txt?v=2")
    check(parse_status(r) == 200, "a query string doesn't stop the file from being found")
    check(body_of(r) == read_public_file("hello.txt"), "and doesn't change which file it is")


def test_builtin_routes_still_win_over_the_filesystem():
    print("test_builtin_routes_still_win_over_the_filesystem")
    r = get(b"/")
    check(parse_status(r) == 200, "/ is still the built-in route")
    check(b"Hello from my C server!" in r, "/ is not resolved to public/index.html")
    r = get(b"/hello")
    check(b"Hello, hello!" in r, "/hello is still the built-in route")


def test_missing_file_is_404():
    print("test_missing_file_is_404")
    check(parse_status(get(b"/nope.txt")) == 404, "a file that isn't there is 404")
    check(parse_status(get(b"/assets/nope.css")) == 404, "same in a subdirectory")


def test_directory_is_404():
    print("test_directory_is_404")
    check(parse_status(get(b"/assets")) == 404, "a directory is 404, there are no listings")
    check(parse_status(get(b"/assets/")) == 404, "and a trailing slash doesn't change that")


def test_path_traversal_is_400():
    print("test_path_traversal_is_400")
    # 400 rather than 404 because a ".." segment makes the request itself
    # defective: no legitimate client sends one, and the server never
    # looked at the filesystem, so "not found" would be asserting
    # something it never checked. See static_file_open() in
    # src/static_files.c.
    check(parse_status(get(b"/../Makefile")) == 400, "a literal ../ traversal is 400")
    check(parse_status(get(b"/../../etc/passwd")) == 400, "climbing several levels is 400")
    check(parse_status(get(b"/assets/../hello.txt")) == 400,
          "even a ../ that would have landed back inside the root is rejected")


def test_encoded_path_traversal_is_400():
    print("test_encoded_path_traversal_is_400")
    # The bypass a resolver that inspects the raw request-target falls
    # for: the dots only look like dots after percent-decoding, which is
    # exactly what open() would have seen.
    check(parse_status(get(b"/%2e%2e/Makefile")) == 400, "%2e%2e is decoded before the .. check, so it's 400")
    check(parse_status(get(b"/..%2fMakefile")) == 400, "an encoded separator doesn't hide the traversal either")
    check(parse_status(get(b"/%2e%2e%2fsrc%2fmain.c")) == 400, "fully encoded traversal is 400")


def test_traversal_response_does_not_leak_the_file():
    print("test_traversal_response_does_not_leak_the_file")
    r = get(b"/../Makefile")
    check(b"gcc" not in r and b"CFLAGS" not in r, "the rejected response contains none of the target file")


def test_malformed_percent_encoding_is_400():
    print("test_malformed_percent_encoding_is_400")
    check(parse_status(get(b"/hello%2")) == 400, "a truncated escape is 400")
    check(parse_status(get(b"/hello%zz.txt")) == 400, "a non-hex escape is 400")
    check(parse_status(get(b"/hello.txt%00.png")) == 400, "an encoded NUL is 400, never truncated at")


def test_symlink_escaping_the_root_is_404():
    print("test_symlink_escaping_the_root_is_404")
    # Nothing in this request-target is suspicious, so the ".." rule
    # can't help; the containment check on the opened fd is what stops
    # it. 404 and not 400 precisely because the request was well formed:
    # answering anything else would confirm the target exists.
    r = get(b"/escape.txt")
    check(parse_status(r) == 404, "a symlink pointing outside the document root is 404")
    check(b"root:" not in r and b"127.0.0.1" not in r, "and none of the target file comes back")


def test_symlink_inside_the_root_is_served():
    print("test_symlink_inside_the_root_is_served")
    r = get(b"/link.txt")
    check(parse_status(r) == 200, "a symlink that stays inside the root is served normally")
    check(body_of(r) == read_public_file("hello.txt"), "and serves the file it points at")


def test_non_get_method_on_a_file_is_405():
    print("test_non_get_method_on_a_file_is_405")
    r = get(b"/index.html", method=b"POST")
    check(parse_status(r) == 405, "POST to an existing file is 405")
    check(header_value(r, "Allow") == "GET", "405 says which method would have worked")
    # 404 outranks 405: claiming "method not allowed" would confirm that
    # a resource exists there.
    check(parse_status(get(b"/nope.txt", method=b"POST")) == 404, "POST to a missing file is 404, not 405")


def test_traversal_does_not_break_the_connection():
    print("test_traversal_does_not_break_the_connection")
    # A rejected path is not a framing error: the request parsed
    # perfectly, so the connection stays usable, unlike the 400 for a
    # malformed request (see tests/test_keepalive.py).
    with connect() as s:
        reader = ResponseReader(s)
        s.sendall(b"GET /../Makefile HTTP/1.1\r\nHost: x\r\n\r\n")
        check(parse_status(reader.read_one()) == 400, "traversal attempt is answered 400")
        s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n")
        check(parse_status(reader.read_one()) == 200, "the same connection still serves the next request")


def test_large_file_survives_backpressure():
    print("test_large_file_survives_backpressure")
    expected = read_public_file("big.bin")
    with connect(SMALL_RECV_BUFFER) as s:
        reader = ResponseReader(s)
        s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n")
        # Let the server fill the socket and hit EAGAIN in sendfile()
        # before a single byte is drained, so the rest of the transfer
        # has to come from the EPOLLOUT resume path.
        time.sleep(0.3)
        r = reader.read_one(timeout=20.0)
    check(parse_status(r) == 200, f"a {BIG_FILE_SIZE // (1024 * 1024)}MiB file is served")
    check(header_value(r, "Content-Length") == str(BIG_FILE_SIZE), "Content-Length matches the file size")
    check(body_of(r) == expected, "every byte arrives, in order, across the sendfile() resumes")


def test_keep_alive_still_works_after_a_large_file():
    print("test_keep_alive_still_works_after_a_large_file")
    # The interesting part is the bookkeeping: the file fd has to be
    # released and the parser reset before the next request, even though
    # this response finished in the EPOLLOUT handler rather than inline.
    with connect(SMALL_RECV_BUFFER) as s:
        reader = ResponseReader(s)
        s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n")
        time.sleep(0.3)
        r1 = reader.read_one(timeout=20.0)
        check(len(body_of(r1)) == BIG_FILE_SIZE, "large file fully received")

        s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n")
        r2 = reader.read_one()
        check(parse_status(r2) == 200, "the same connection serves a second request afterwards")
        check(body_of(r2) == read_public_file("hello.txt"), "and serves the right file")


def test_pipelined_static_requests():
    print("test_pipelined_static_requests")
    with connect() as s:
        reader = ResponseReader(s)
        s.sendall(b"GET /hello.txt HTTP/1.1\r\n\r\nGET /assets/style.css HTTP/1.1\r\n\r\n")
        r1 = reader.read_one()
        r2 = reader.read_one()
        check(body_of(r1) == read_public_file("hello.txt"), "first pipelined file response is correct")
        check(body_of(r2) == read_public_file("assets/style.css"), "second pipelined file response is correct")


def test_client_disconnecting_mid_download_does_not_kill_the_server():
    print("test_client_disconnecting_mid_download_does_not_kill_the_server")
    # Writing to a socket the peer has reset raises SIGPIPE, whose
    # default action would end the process; sendfile() has no
    # MSG_NOSIGNAL, so main() ignores the signal outright. Without that,
    # this test kills the server and every check after it fails.
    s = connect(SMALL_RECV_BUFFER)
    s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n")
    s.recv(1024)  # let the transfer get going
    # SO_LINGER with a zero timeout makes close() send an RST instead of
    # a FIN, so the server's next sendfile() fails hard (EPIPE /
    # ECONNRESET) rather than draining politely into a half-closed
    # socket.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()

    time.sleep(0.3)
    r = get(b"/hello.txt")
    check(parse_status(r) == 200, "the server is still serving after a client vanished mid-download")


def build_scratch_root(root_dir):
    """public/ plus the things that can't live in the repository."""
    for name in os.listdir(PUBLIC_DIR):
        source = os.path.join(PUBLIC_DIR, name)
        destination = os.path.join(root_dir, name)
        if os.path.isdir(source):
            shutil.copytree(source, destination)
        else:
            shutil.copy(source, destination)

    with open(os.path.join(root_dir, "blob.unknown"), "wb") as f:
        f.write(b"no extension mapping for this one\n")

    # Deterministic, non-repeating content: a file of identical bytes
    # would still look intact if the server duplicated or dropped a
    # chunk while resuming a partial sendfile().
    with open(os.path.join(root_dir, "big.bin"), "wb") as f:
        f.write(bytes((i * 7 + i // 251) % 256 for i in range(BIG_FILE_SIZE)))

    os.symlink("hello.txt", os.path.join(root_dir, "link.txt"))
    # Absolute, and pointing at a file that exists on any Linux box.
    os.symlink("/etc/hosts", os.path.join(root_dir, "escape.txt"))


def main():
    global root
    with tempfile.TemporaryDirectory(prefix="http-server-static-") as root_dir:
        root = root_dir
        build_scratch_root(root_dir)

        with running_server({"HTTP_SERVER_PUBLIC_ROOT": root_dir}):
            test_html_file_is_served_with_matching_headers_and_body()
            test_text_file_is_served()
            test_nested_path_is_served()
            test_unknown_extension_is_octet_stream()
            test_query_string_is_ignored_when_resolving()
            test_builtin_routes_still_win_over_the_filesystem()
            test_missing_file_is_404()
            test_directory_is_404()
            test_path_traversal_is_400()
            test_encoded_path_traversal_is_400()
            test_traversal_response_does_not_leak_the_file()
            test_malformed_percent_encoding_is_400()
            test_symlink_escaping_the_root_is_404()
            test_symlink_inside_the_root_is_served()
            test_non_get_method_on_a_file_is_405()
            test_traversal_does_not_break_the_connection()
            test_large_file_survives_backpressure()
            test_keep_alive_still_works_after_a_large_file()
            test_pipelined_static_requests()
            test_client_disconnecting_mid_download_does_not_kill_the_server()

    report()


if __name__ == "__main__":
    main()
