.PHONY: build clean debug run release

BIN = clox
CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -std=c99
DEBUG_FLAGS = -ggdb -DLOX_DEBUG -fno-omit-frame-pointer -fsanitize=address
RELEASE_FLAGS = -O3 -flto

debug: *.c *.h
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) *.c -o $(BIN)

build: *.c *.h
	$(CC) $(CFLAGS) *.c -o $(BIN)

release: *.c *.h
	$(CC) $(CFLAGS) $(RELEASE_FLAGS) *.c -o $(BIN)

run: build
	@./$(BIN)

clean:
	$(RM) $(BIN)
