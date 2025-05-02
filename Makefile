.PHONY: build clean run
BIN = clox
CFLAGS = -Wall -Wextra -Wswitch-enum -ggdb -std=c99

build: *.c *.h
	gcc $(CFLAGS) *.c -o $(BIN)

run: build
	@./$(BIN)

clean:
	$(RM) $(BIN)
