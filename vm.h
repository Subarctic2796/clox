#ifndef INCLUDE_CLOX_VM_H_
#define INCLUDE_CLOX_VM_H_

#include "chunk.h"
#include "common.h"
#include "value.h"

#define STACK_MAX 256

typedef struct {
  Chunk *chunk;
  uint8_t *ip;
  Value stack[STACK_MAX];
  Value *stackTop;
} VM;

typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERR,
  INTERPRET_RUNTIME_ERR,
} InterpretResult;

void initVM();
void freeVM();
InterpretResult interpret(const char *source);
void push(Value value);
Value pop();

#endif // INCLUDE_CLOX_VM_H_
