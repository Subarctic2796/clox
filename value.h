#ifndef INCLUDE_CLOX_VALUE_H_
#define INCLUDE_CLOX_VALUE_H_

#include "common.h"

typedef double Value;

typedef struct {
  size_t cap, cnt;
  Value *values;
} ValueArray;

void initValueArray(ValueArray *array);
void writeValueArray(ValueArray *array, Value value);
void freeValueArray(ValueArray *array);
void printValue(Value value);

#endif // INCLUDE_CLOX_VALUE_H_
