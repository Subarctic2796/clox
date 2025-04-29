.PHONY: build clean run
BIN = clox

build: *.c *.h
	gcc -Wall -Wextra -Wswitch-enum -ggdb *.c -o $(BIN)

run: build
	@./$(BIN)

clean:
	$(RM) $(BIN)
