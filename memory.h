#ifndef INCLUDE_CLOX_MEMORY_H_
#define INCLUDE_CLOX_MEMORY_H_

#include "common.h"
#include <stddef.h>

#define GROW_CAP(cap) ((cap) < 8 ? 8 : (cap) * 2)

#define GROW_ARRAY(type, ptr, oldCnt, newCnt)                                  \
  (type *)reallocate(ptr, sizeof(type) * (oldCnt), sizeof(type) * (newCnt))

#define FREE_ARRAY(type, ptr, oldCnt)                                          \
  reallocate((ptr), sizeof(type) * (oldCnt), 0)

void *reallocate(void *ptr, size_t oldSize, size_t newSize);

#endif // INCLUDE_CLOX_MEMORY_H_
