.PHONY: build clean run

BIN = clox
CFLAGS = -Wall -Wextra -Wpedantic -ggdb -std=c99
EXTRA_FLAGS = -Wswitch-enum
DEBUG_FLAGS = -ggdb -DLOX_DEBUG

debug: *.c *.h
	gcc $(CFLAGS) $(DEBUG_FLAGS) *.c -o $(BIN)

build: *.c *.h
	gcc $(CFLAGS) *.c -o $(BIN)

run: build
	@./$(BIN)

clean:
	$(RM) $(BIN)
