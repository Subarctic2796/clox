BIN = clox

build: *.c *.h
	gcc -Wall -Wextra -ggdb *.c
