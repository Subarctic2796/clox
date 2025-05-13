.PHONY: build clean run

BIN = clox
CC = gcc
# CC = clang
CFLAGS = -Wall -Wextra -Wpedantic -ggdb -std=c99 -fsanitize=address
CFLAGS += -fno-omit-frame-pointer
EXTRA_FLAGS = -Wswitch-enum
DEBUG_FLAGS = -ggdb -DLOX_DEBUG

debug: *.c *.h
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) *.c -o $(BIN)

build: *.c *.h
	$(CC) $(CFLAGS) *.c -o $(BIN)

run: build
	@./$(BIN)

clean:
	$(RM) $(BIN)
