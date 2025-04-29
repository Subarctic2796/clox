#include "debug.h"
#include "chunk.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "value.h"

void disassembleChunk(Chunk *chunk, const char *name) {
  printf("== %s ==\n", name);
  for (int offset = 0; offset < chunk->cnt;) {
    offset = disassembleInst(chunk, offset);
  }
}

static int simpleInst(const char *name, int offset) {
  printf("%s\n", name);
  return offset + 1;
}

static int constantInst(const char *name, Chunk *chunk, int offset) {
  uint8_t constIdx = chunk->code[offset + 1];
  printf("%-16s %4d '", name, constIdx);
  printValue(chunk->constants.values[constIdx]);
  printf("'\n");
  return offset + 2;
}

int disassembleInst(Chunk *chunk, int offset) {
  printf("%04d ", offset);
  if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
    printf("   | ");
  } else {
    printf("%4d ", chunk->lines[offset]);
  }

  uint8_t inst = chunk->code[offset];
  switch (inst) {
  case OP_CONSTANT:
    return constantInst("OP_CONSTANT", chunk, offset);
  case OP_RETURN:
    return simpleInst("OP_RETURN", offset);
  default:
    printf("Unknown opcode %d\n", inst);
    return offset + 1;
  }
}
