.PHONY: build clean debug run release

BIN = clox
CC = gcc
# CC = clang
CFLAGS = -Wall -Wextra -Wpedantic
DEBUG_FLAGS = -ggdb -DLOX_DEBUG -fno-omit-frame-pointer -fsanitize=address
RELEASE_FLAGS = -O3 -flto -march=native

SRC = src

debug: $(SRC)/*.c $(SRC)/*.h
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) $(SRC)/*.c -o $(BIN)

build: $(SRC)/*.c $(SRC)/*.h
	$(CC) $(CFLAGS) $(SRC)/*.c -o $(BIN)

release: $(SRC)/*.c $(SRC)/*.h
	$(CC) $(CFLAGS) $(RELEASE_FLAGS) $(SRC)/*.c -o $(BIN)

run: build
	@./$(BIN)

clean:
	$(RM) $(BIN)
