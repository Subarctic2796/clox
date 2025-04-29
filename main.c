#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "chunk.h"
#include "debug.h"

int main(int argc, char *argv[]) {
  Chunk chunk = {0};

  initChunk(&chunk);

  int constIdx = addConst(&chunk, 1.2);
  writeChunk(&chunk, OP_CONSTANT, 123);
  writeChunk(&chunk, constIdx, 123);

  writeChunk(&chunk, OP_RETURN, 123);

  disassembleChunk(&chunk, "test chunk");

  freeChunk(&chunk);

  return EXIT_SUCCESS;
}
