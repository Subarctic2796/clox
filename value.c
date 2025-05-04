#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "value.h"

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

void printValue(Value value) {
  switch (value.type) {
  case VAL_BOOL:
    printf(AS_BOOL(value) ? "true" : "false");
    break;
  case VAL_NIL:
    printf("nil");
    break;
  case VAL_NUMBER:
    printf("%g", AS_NUMBER(value));
    break;
  case VAL_OBJ:
    printObject(value);
  }
}

bool valuesEqual(Value a, Value b) {
  if (a.type != b.type) {
    return false;
  }
  switch (a.type) {
  case VAL_NIL:
    return true;
  case VAL_BOOL:
    return AS_BOOL(a) == AS_BOOL(b);
  case VAL_NUMBER:
    return AS_NUMBER(a) == AS_NUMBER(b);
  case VAL_OBJ:
    return AS_OBJ(a) == AS_OBJ(b);
  default:
    return false; // unreachable
  }
}
