#ifndef INCLUDE_CLOX_VM_H_
#define INCLUDE_CLOX_VM_H_

#include "chunk.h"
#include "common.h"
#include "object.h"
#include "table.h"
#include "value.h"

#define FRAMES_MAX 64
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)
#define TEMP_ROOTS_MAX 8

typedef struct {
  ObjClosure *closure;
  uint8_t *ip;
  Value *slots;
} CallFrame;

typedef struct {
  CallFrame frames[FRAMES_MAX];
  int frameCount;

  Value stack[STACK_MAX];
  Value *sp;
  Table globals;
  Table strings;
  ObjString *initString;
  ObjUpvalue *openUpvalues;

  size_t bytesAllocated;
  size_t nextGC;
  Obj *objects;

  int grayCnt;
  int grayCap;
  Obj **grayStack;

  Value tempRoots[TEMP_ROOTS_MAX];
  int tempCnt;
} VM;

typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERR,
  INTERPRET_RUNTIME_ERR,
} InterpretResult;

extern VM vm;

void initVM(void);
void freeVM(void);
InterpretResult interpret(const char *source);
static inline void pushRoot(Value value) { vm.tempRoots[vm.tempCnt++] = value; }
static inline void popRoot(void) { vm.tempCnt--; }

#endif // INCLUDE_CLOX_VM_H_
