#ifndef INCLUDE_CLOX_CHUNK_H_
#define INCLUDE_CLOX_CHUNK_H_

#include "common.h"
#include "value.h"

typedef enum {
  OP_CONSTANT,
  OP_ADD,
  OP_SUBTRACT,
  OP_MULTIPLY,
  OP_DIVIDE,
  OP_NEGATE,
  OP_RETURN,
} OpCode;

typedef struct {
  int cnt, cap;
  uint8_t *code;
  int *lines; // code and lines must be the same size
  ValueArray constants;
} Chunk;

void initChunk(Chunk *chunk);
void freeChunk(Chunk *chunk);
void writeChunk(Chunk *chunk, uint8_t byte, int line);
int addConst(Chunk *chunk, Value value);

#endif // INCLUDE_CLOX_CHUNK_H_
