#ifndef INCLUDE_CLOX_VM_H_
#define INCLUDE_CLOX_VM_H_

#include "chunk.h"
#include "common.h"
#include "object.h"
#include "table.h"
#include "value.h"

#define FRAMES_MAX     64
#define STACK_MAX      (FRAMES_MAX * UINT8_COUNT)
#define TEMP_ROOTS_MAX 8

typedef struct {
    ObjClosure *closure;
    uint8_t *ip;
    Value *slots;
} CallFrame;

typedef struct VM {
    CallFrame frames[FRAMES_MAX];
    int frameCount;

    Value stack[STACK_MAX];
    Value *sp;
    Table globalNames;
    ValueArray globalValues;
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

void initVM(VM *vm);
void freeVM(VM *vm);
InterpretResult interpret(VM *vm, const char *source);
static inline void pushRoot(VM *vm, Value value) {
    vm->tempRoots[vm->tempCnt++] = value;
}
static inline void popRoot(VM *vm) { vm->tempCnt--; }
static inline void push(VM *vm, Value value) { *vm->sp++ = value; }
static inline Value pop(VM *vm) { return *(--vm->sp); }
static inline Value peek(VM *vm, int dist) { return vm->sp[-1 - dist]; }

#endif // INCLUDE_CLOX_VM_H_
