#ifndef INCLUDE_CLOX_MEMORY_H_
#define INCLUDE_CLOX_MEMORY_H_

#include "common.h"
#include "object.h"

#define ALLOCATE(type, cnt)                                                    \
    (type *)reallocate(vm, NULL, 0, sizeof(type) * (cnt))

#define FREE(type, ptr) reallocate(vm, ptr, sizeof(type), 0)

#define GROW_CAP(cap) ((cap) < 8 ? 8 : (cap) * 2)

#define GROW_ARRAY(type, ptr, oldCnt, newCnt)                                  \
    (type *)reallocate(vm, ptr, sizeof(type) * (oldCnt),                       \
                       sizeof(type) * (newCnt))

#define FREE_ARRAY(type, ptr, oldCnt)                                          \
    reallocate(vm, (ptr), sizeof(type) * (oldCnt), 0)

typedef struct VM VM;

void *reallocate(VM *vm, void *ptr, size_t oldSize, size_t newSize);
void markObject(VM *vm, Obj *object);
void markValue(VM *vm, Value value);
void collectGarbage(VM *vm);
void freeObjects(VM *vm);

#endif // INCLUDE_CLOX_MEMORY_H_
