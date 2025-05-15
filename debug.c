#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "chunk.h"
#include "debug.h"
#include "object.h"
#include "value.h"

void disassembleChunk(Chunk *chunk, const char *name) {
  printf("== %s ==\n", name);
  for (int offset = 0; offset < chunk->cnt;) {
    offset = disassembleInst(chunk, offset);
  }
}

static inline int simpleInst(const char *name, int offset) {
  printf("%s\n", name);
  return offset + 1;
}

static inline int byteInst(const char *name, Chunk *chunk, int offset) {
  uint8_t slot = chunk->code[offset + 1];
  printf("%-16s %4d\n", name, slot);
  return offset + 2;
}

static inline int jumpInst(const char *name, int sign, Chunk *chunk,
                           int offset) {
  uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
  jump |= chunk->code[offset + 2];
  printf("%-16s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
  return offset + 3;
}

static inline int constantInst(const char *name, Chunk *chunk, int offset) {
  uint8_t constIdx = chunk->code[offset + 1];
  printf("%-16s %4d '", name, constIdx);
  printValue(chunk->constants.values[constIdx]);
  printf("'\n");
  return offset + 2;
}

static inline int invokeInst(const char *name, Chunk *chunk, int offset) {
  uint8_t idx = chunk->code[offset + 1];
  uint8_t argCnt = chunk->code[offset + 2];
  printf("%-16s (%d args) %4d '", name, argCnt, idx);
  printValue(chunk->constants.values[idx]);
  printf("'\n");
  return offset + 3;
}

int disassembleInst(Chunk *chunk, int offset) {
  printf("%04d ", offset);
  if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
    printf("   | ");
  } else {
    printf("%4d ", chunk->lines[offset]);
  }

  OpCode inst = (OpCode)chunk->code[offset];
  switch (inst) {
  case OP_CONSTANT:
    return constantInst("OP_CONSTANT", chunk, offset);
  case OP_NIL:
    return simpleInst("OP_NIL", offset);
  case OP_FALSE:
    return simpleInst("OP_FALSE", offset);
  case OP_GET_LOCAL:
    return byteInst("OP_GET_LOCAL", chunk, offset);
  case OP_SET_LOCAL:
    return byteInst("OP_SET_LOCAL", chunk, offset);
  case OP_GET_GLOBAL:
    return constantInst("OP_GET_GLOBAL", chunk, offset);
  case OP_DEFINE_GLOBAL:
    return constantInst("OP_DEFINE_GLOBAL", chunk, offset);
  case OP_SET_GLOBAL:
    return constantInst("OP_SET_GLOBAL", chunk, offset);
  case OP_GET_UPVALUE:
    return byteInst("OP_GET_UPVALUE", chunk, offset);
  case OP_SET_UPVALUE:
    return byteInst("OP_SET_UPVALUE", chunk, offset);
  case OP_GET_PROPERTY:
    return constantInst("OP_GET_PROPERTY", chunk, offset);
  case OP_SET_PROPERTY:
    return constantInst("OP_SET_PROPERTY", chunk, offset);
  case OP_GET_SUPER:
    return constantInst("OP_GET_SUPER", chunk, offset);
  case OP_EQUAL:
    return simpleInst("OP_EQUAL", offset);
  case OP_GREATER:
    return simpleInst("OP_GREATER", offset);
  case OP_LESS:
    return simpleInst("OP_LESS", offset);
  case OP_TRUE:
    return simpleInst("OP_TRUE", offset);
  case OP_ADD:
    return simpleInst("OP_ADD", offset);
  case OP_SUBTRACT:
    return simpleInst("OP_SUBTRACT", offset);
  case OP_MULTIPLY:
    return simpleInst("OP_MULTIPLY", offset);
  case OP_DIVIDE:
    return simpleInst("OP_DIVIDE", offset);
  case OP_NOT:
    return simpleInst("OP_NOT", offset);
  case OP_NEGATE:
    return simpleInst("OP_NEGATE", offset);
  case OP_PRINT:
    return simpleInst("OP_PRINT", offset);
  case OP_POP:
    return simpleInst("OP_POP", offset);
  case OP_JUMP:
    return jumpInst("OP_JUMP", 1, chunk, offset);
  case OP_JUMP_IF_FALSE:
    return jumpInst("OP_JUMP_IF_FALSE", 1, chunk, offset);
  case OP_LOOP:
    return jumpInst("OP_LOOP", -1, chunk, offset);
  case OP_CALL:
    return byteInst("OP_CALL", chunk, offset);
  case OP_INVOKE:
    return invokeInst("OP_INVOKE", chunk, offset);
  case OP_SUPER_INVOKE:
    return invokeInst("OP_SUPER_INVOKE", chunk, offset);
  case OP_CLOSURE: {
    offset++;
    uint8_t idx = chunk->code[offset++];
    printf("%-16s %4d ", "OP_CLOSURE", idx);
    printValue(chunk->constants.values[idx]);
    printf("\n");

    ObjFunction *function = AS_FUNCTION(chunk->constants.values[idx]);
    for (int j = 0; j < function->upvalueCnt; j++) {
      int isLocal = chunk->code[offset++];
      int index = chunk->code[offset++];
      printf("%04d      |                     %s %d\n", offset - 2,
             isLocal ? "local" : "upvalue", index);
    }
    return offset;
  }
  case OP_CLOSE_UPVALUE:
    return simpleInst("OP_CLOSE_UPVALUE", offset);
  case OP_RETURN:
    return simpleInst("OP_RETURN", offset);
  case OP_CLASS:
    return constantInst("OP_CLASS", chunk, offset);
  case OP_INHERIT:
    return simpleInst("OP_INHERIT", offset);
  case OP_METHOD:
    return constantInst("OP_METHOD", chunk, offset);
  default:
    printf("Unknown opcode %d\n", inst);
    return offset + 1;
  }
}
