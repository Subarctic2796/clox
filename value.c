#include "value.h"
#include "memory.h"
#include <stdlib.h>

void initValueArray(ValueArray *array) {
  array->values = NULL;
  array->cap = 0;
  array->cnt = 0;
}

void freeValueArray(ValueArray *array) {
  FREE_ARRAY(uint8_t, array->values, array->cap);
  initValueArray(array);
}

void writeValueArray(ValueArray *array, Value value) {
  if (array->cap < array->cnt + 1) {
    size_t oldCap = array->cap;
    array->cap = GROW_CAP(oldCap);
    array->values = GROW_ARRAY(Value, array->values, oldCap, array->cap);
  }
  array->values[array->cnt++] = value;
}

void printValue(Value value) { printf("%g", value); }
