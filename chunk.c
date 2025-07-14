#include "chunk.h"
#include "memory.h"
#include "value.h"

void initChunk(Chunk *chunk) {
  chunk->cap = 0;
  chunk->cnt = 0;
  chunk->code = NULL;
  chunk->lineCap = 0;
  chunk->lineCnt = 0;
  chunk->lines = NULL;
  initValueArray(&chunk->constants);
}

void writeChunk(Chunk *chunk, uint8_t byte, int line) {
  // grow code
  if (chunk->cap < chunk->cnt + 1) {
    int oldCap = chunk->cap;
    chunk->cap = GROW_CAP(oldCap);
    chunk->code = GROW_ARRAY(uint8_t, chunk->code, oldCap, chunk->cap);
  }
  chunk->code[chunk->cnt++] = byte;

  // see if still on same line
  if (chunk->lineCnt > 0 && chunk->lines[chunk->lineCnt - 1].line == line) {
    return;
  }

  // add a new line
  if (chunk->lineCap < chunk->lineCnt + 1) {
    int oldCap = chunk->lineCap;
    chunk->lineCap = GROW_CAP(oldCap);
    chunk->lines = GROW_ARRAY(LineInfo, chunk->lines, oldCap, chunk->lineCap);
  }
  chunk->lines[chunk->lineCnt++] = ((LineInfo){chunk->lineCnt - 1, line});
}

int addConst(Chunk *chunk, Value value) {
  writeValueArray(&chunk->constants, value);
  return chunk->constants.cnt - 1;
}

int getLine(Chunk *chunk, int instruction) {
  int start = 0;
  int end = chunk->lineCnt - 1;
  int len = end;

  for (;;) {
    int mid = (start + end) / 2;
    LineInfo *line = &chunk->lines[mid];
    if (instruction < line->offset) {
      end = mid - 1;
    } else if (mid == len || instruction < chunk->lines[mid + 1].offset) {
      return line->line;
    } else {
      start = mid + 1;
    }
  }
}

void freeChunk(Chunk *chunk) {
  FREE_ARRAY(uint8_t, chunk->code, chunk->cap);
  FREE_ARRAY(LineInfo, chunk->lines, chunk->lineCap);
  freeValueArray(&chunk->constants);
  initChunk(chunk);
}
