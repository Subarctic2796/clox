#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "debug.h"
#include "vm.h"

static void repl() {
  char line[1024];
  for (;;) {
    printf("> ");
    if (!fgets(line, sizeof(line), stdin)) {
      printf("\n");
      break;
    }
    interpret(line);
  }
}

static char *readFile(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    fprintf(stderr, "Could not open file '%s'\n", path);
    exit(74);
  }

  fseek(f, 0L, SEEK_END);
  size_t fsize = ftell(f);
  rewind(f);

  char *buffer = (char *)malloc(fsize + 1);
  if (buffer == NULL) {
    fprintf(stderr, "buy more RAM lol\n");
    exit(74);
  }
  size_t nread = fread(buffer, sizeof(char), fsize, f);
  if (nread < fsize) {
    fprintf(stderr, "Could not read file '%s'\n", path);
    exit(74);
  }
  buffer[nread] = '\0';

  fclose(f);
  return buffer;
}

static void runFile(const char *path) {
  char *src = readFile(path);
  InterpretResult result = interpret(src);
  free(src);

  switch (result) {
  case INTERPRET_COMPILE_ERR:
    exit(65);
  case INTERPRET_RUNTIME_ERR:
    exit(70);
  case INTERPRET_OK:
    return;
  default:
    fprintf(stderr, "[unreachable] can't have any other VM exit code\n");
    exit(64);
  }
}

int main(int argc, char *argv[]) {
  initVM();

  switch (argc) {
  case 1:
    repl();
    break;
  case 2:
    runFile(argv[1]);
    break;
  default:
    fprintf(stderr, "Usage: clox [path]\n");
    exit(64);
  }

  freeVM();
  return EXIT_SUCCESS;
}
