CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined
LDFLAGS = -fsanitize=address,undefined

SRC = src/main.c
BIN = http-server

.PHONY: all run clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

run: all
	./$(BIN)

clean:
	rm -f $(BIN)
