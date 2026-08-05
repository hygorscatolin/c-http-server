CC = gcc
# -O1 is not about speed here. A few warnings only exist once the
# optimizer has run, because they depend on the data-flow analysis it
# does: -Wformat-truncation, -Wmaybe-uninitialized and the -Wstringop-*
# family are all silently disabled at -O0. It is also the level
# AddressSanitizer is normally built at, being enough to keep the
# instrumented binary usable without folding away the frames a report
# needs.
#
# -pthread is a compile flag as much as a link flag: it defines
# _REENTRANT and turns on the thread-safe paths in libc's headers, so
# passing it only to the linker is a subtly broken build rather than a
# style choice. AddressSanitizer's data-race blindness is worth knowing
# about here: it does not detect them at all, that is ThreadSanitizer's
# job, and the two cannot be enabled at once. Layer 6 answers this by
# construction rather than by tooling, see worker_pool.c: workers share
# no mutable state, so there is nothing for a race detector to find.
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined -pthread -Isrc
LDFLAGS = -fsanitize=address,undefined -pthread

# The build for measuring, and only for measuring. Same warning set (a
# release that compiles with fewer checks than the debug build is a
# release nobody has actually checked), same -pthread, no sanitizers,
# -O2 instead of -O1.
#
# Sanitizers come out because with them in, a benchmark measures the
# instrumentation rather than the server. AddressSanitizer checks every
# load and store against its shadow memory, which alone tends to cost
# around 2x CPU and 3x RSS, and it replaces malloc with an allocator that
# adds redzones and a quarantine, so freed memory is not reused promptly:
# this server malloc/frees one connection_t per accepted connection, so
# that is squarely on the hot path. UndefinedBehaviorSanitizer adds a
# branch to arithmetic and to every pointer it can. None of that is
# uniform overhead, it is heavier exactly where the code does the most
# work, which distorts the comparison *between* scenarios and not just
# the absolute numbers. Sanitizer figures would say more about GCC than
# about epoll or sendfile.
#
# What comes out with them is all the memory-error detection, so this
# binary is not the one to trust for correctness: every test target above
# deliberately keeps running against the instrumented $(BIN). That is
# also why this is a separate output file rather than a flag toggle on
# the same one, the two can coexist and a stale uninstrumented binary can
# never be silently handed to the test suite.
#
# -O2 is what you would actually ship, and the level whose extra
# data-flow analysis surfaces warnings -O1 does not reach, so building
# this target is a small static-analysis pass in its own right.
RELEASE_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread -Isrc
RELEASE_LDFLAGS = -pthread

SRC = src/main.c src/event_loop.c src/http_parser.c src/static_files.c src/worker_pool.c
HDR = src/event_loop.h src/http_parser.h src/static_files.h src/worker_pool.h
BIN = http-server
RELEASE_BIN = http-server-release

PARSER_TEST_SRC = tests/test_http_parser.c src/http_parser.c
PARSER_TEST_BIN = test_http_parser

STATIC_TEST_SRC = tests/test_static_files.c src/static_files.c
STATIC_TEST_BIN = test_static_files

.PHONY: all release run test test-keepalive test-static test-threads test-all benchmark clean

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

release: $(RELEASE_BIN)

$(RELEASE_BIN): $(SRC) $(HDR)
	$(CC) $(RELEASE_CFLAGS) -o $(RELEASE_BIN) $(SRC) $(RELEASE_LDFLAGS)

run: all
	./$(BIN)

# Unit tests: linked against the module sources directly, no server process.
test: $(PARSER_TEST_BIN) $(STATIC_TEST_BIN)
	./$(PARSER_TEST_BIN)
	./$(STATIC_TEST_BIN)

$(PARSER_TEST_BIN): $(PARSER_TEST_SRC) src/http_parser.h
	$(CC) $(CFLAGS) -o $(PARSER_TEST_BIN) $(PARSER_TEST_SRC) $(LDFLAGS)

$(STATIC_TEST_BIN): $(STATIC_TEST_SRC) src/static_files.h
	$(CC) $(CFLAGS) -o $(STATIC_TEST_BIN) $(STATIC_TEST_SRC) $(LDFLAGS)

# Integration tests: keep-alive negotiation, pipelining, and the idle
# timeout, exercised against the real binary over a real socket. Not
# part of `test` because it starts the actual server process (needs
# port 8080 free) instead of just linking against the parser source.
test-keepalive: all
	python3 tests/test_keepalive.py

# Same deal for layer 5: sendfile() backpressure and path traversal over
# the wire only mean something against a running server.
test-static: all
	python3 tests/test_static_files.py

# And layer 6: which worker answered is only observable from outside the
# process, so this one is necessarily an integration test too.
test-threads: all
	python3 tests/test_thread_pool.py

test-all: test test-keepalive test-static test-threads

# Builds the release binary itself, so `make benchmark` on a clean tree
# is enough. Everything else about the run (scenarios, durations, where
# the numbers land) is in the script.
benchmark:
	./scripts/benchmark.sh

clean:
	rm -f $(BIN) $(RELEASE_BIN) $(PARSER_TEST_BIN) $(STATIC_TEST_BIN)
