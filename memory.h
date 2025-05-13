#ifndef INCLUDE_CLOX_MEMORY_H_
#define INCLUDE_CLOX_MEMORY_H_

#include <stddef.h>

#include "common.h"
#include "object.h"

#define ALLOCATE(type, cnt) (type *)reallocate(NULL, 0, sizeof(type) * (cnt))

#define FREE(type, ptr) reallocate(ptr, sizeof(type), 0)

#define GROW_CAP(cap) ((cap) < 16 ? 16 : (cap) * 2)

#define GROW_ARRAY(type, ptr, oldCnt, newCnt)                                  \
  (type *)reallocate(ptr, sizeof(type) * (oldCnt), sizeof(type) * (newCnt))

#define FREE_ARRAY(type, ptr, oldCnt)                                          \
  reallocate((ptr), sizeof(type) * (oldCnt), 0)

void *reallocate(void *ptr, size_t oldSize, size_t newSize);
void markObject(Obj *object);
void markValue(Value value);
void collectGarbage(void);
void freeObjects(void);

#endif // INCLUDE_CLOX_MEMORY_H_
