#include "chunk.h"
#include "memory.h"
#include "value.h"
#include <stdint.h>
#include <stdlib.h>

void initChunk(Chunk *chunk) {
  chunk->cap = 0;
  chunk->cnt = 0;
  chunk->code = NULL;
  initValueArray(&chunk->constants);
}

void writeChunk(Chunk *chunk, uint8_t byte, int line) {
  if (chunk->cap < chunk->cnt + 1) {
    size_t oldCap = chunk->cap;
    chunk->cap = GROW_CAP(oldCap);
    chunk->code = GROW_ARRAY(uint8_t, chunk->code, oldCap, chunk->cap);
    chunk->lines = GROW_ARRAY(int, chunk->lines, oldCap, chunk->cap);
  }
  chunk->code[chunk->cnt] = byte;
  chunk->lines[chunk->cnt] = line;
  chunk->cnt++;
}

int addConst(Chunk *chunk, Value value) {
  writeValueArray(&chunk->constants, value);
  return chunk->constants.cnt - 1;
}

void freeChunk(Chunk *chunk) {
  FREE_ARRAY(uint8_t, chunk->code, chunk->cap);
  FREE_ARRAY(int, chunk->lines, chunk->cap);
  freeValueArray(&chunk->constants);
  initChunk(chunk);
}
