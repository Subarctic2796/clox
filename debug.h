#ifndef INCLUDE_CLOX_DEBUG_H_
#define INCLUDE_CLOX_DEBUG_H_

#include "chunk.h"

void disassembleChunk(Chunk *chunk, const char *name);
int disassembleInst(Chunk *chunk, int offset);

#endif // INCLUDE_CLOX_DEBUG_H_
