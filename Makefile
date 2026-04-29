.PHONY: build clean debug run release

BIN = clox
CC = gcc
# CC = clang
CFLAGS = -Wall -Wextra -Wpedantic -Wswitch-enum
DEBUG_FLAGS = -ggdb -DLOX_DEBUG -fno-omit-frame-pointer -fsanitize=address
RELEASE_FLAGS = -ULOX_DEBUG -O3 -flto -march=native
LDFLAGS = -lm

SRC = src

debug: $(SRC)/*.c $(SRC)/*.h
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) $(LDFLAGS) $(SRC)/*.c -o $(BIN)

build: $(SRC)/*.c $(SRC)/*.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRC)/*.c -o $(BIN)

release: $(SRC)/*.c $(SRC)/*.h
	$(CC) $(CFLAGS) $(RELEASE_FLAGS) $(LDFLAGS) $(SRC)/*.c -o $(BIN)

run: build
	@./$(BIN)

clean:
	$(RM) $(BIN)
