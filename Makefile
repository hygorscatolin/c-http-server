CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined -Isrc
LDFLAGS = -fsanitize=address,undefined

SRC = src/main.c src/event_loop.c src/http_parser.c
HDR = src/event_loop.h src/http_parser.h
BIN = http-server

TEST_SRC = tests/test_http_parser.c src/http_parser.c
TEST_BIN = test_http_parser

.PHONY: all run test clean

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

run: all
	./$(BIN)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) src/http_parser.h
	$(CC) $(CFLAGS) -o $(TEST_BIN) $(TEST_SRC) $(LDFLAGS)

clean:
	rm -f $(BIN) $(TEST_BIN)
