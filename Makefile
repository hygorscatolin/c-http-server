CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined -Isrc
LDFLAGS = -fsanitize=address,undefined

SRC = src/main.c src/event_loop.c src/http_parser.c
HDR = src/event_loop.h src/http_parser.h
BIN = http-server

TEST_SRC = tests/test_http_parser.c src/http_parser.c
TEST_BIN = test_http_parser

.PHONY: all run test test-keepalive test-all clean

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

run: all
	./$(BIN)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) src/http_parser.h
	$(CC) $(CFLAGS) -o $(TEST_BIN) $(TEST_SRC) $(LDFLAGS)

# Integration tests: keep-alive negotiation, pipelining, and the idle
# timeout, exercised against the real binary over a real socket. Not
# part of `test` because it starts the actual server process (needs
# port 8080 free) instead of just linking against the parser source.
test-keepalive: all
	python3 tests/test_keepalive.py

test-all: test test-keepalive

clean:
	rm -f $(BIN) $(TEST_BIN)
