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
#ifdef NAN_BOXING
  if (IS_BOOL(value)) {
    printf(AS_BOOL(value) ? "true" : "false");
  } else if (IS_NIL(value)) {
    printf("nil");
  } else if (IS_EMPTY(value)) {
    printf("<empty>");
  } else if (IS_NUMBER(value)) {
    printf("%g", AS_NUMBER(value));
  } else if (IS_OBJ(value)) {
    printObject(value);
  }
#else /* ifdef NAN_BOXING */
  switch (value.type) {
  case VAL_BOOL:
    printf(AS_BOOL(value) ? "true" : "false");
    break;
  case VAL_NIL:
    printf("nil");
    break;
  case VAL_EMPTY:
    printf("<empty>");
    break;
  case VAL_NUMBER:
    printf("%g", AS_NUMBER(value));
    break;
  case VAL_OBJ:
    printObject(value);
  }
#endif
}

bool valuesEqual(Value a, Value b) {
#ifdef NAN_BOXING
  if (IS_NUMBER(a) && IS_NUMBER(b)) {
    return AS_NUMBER(a) == AS_NUMBER(b);
  }
  return a == b;
#else
  if (a.type != b.type) {
    return false;
  }
  switch (a.type) {
  case VAL_NIL:
    return true;
  case VAL_EMPTY:
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
#endif
}

static inline uint32_t hashBits(uint64_t hash) {
  // From v8's ComputeLongHash() which in turn cites:
  // Thomas Wang, Integer Hash Functions.
  // http://www.concentric.net/~Ttwang/tech/inthash.htm
  hash = ~hash + (hash << 18); // hash = (hash << 18) - hash - 1;
  hash = hash ^ (hash >> 31);
  hash = hash * 21; // hash = (hash + (hash << 2)) + (hash << 4);
  hash = hash ^ (hash >> 11);
  hash = hash + (hash << 6);
  hash = hash ^ (hash >> 22);
  return (uint32_t)(hash & 0x3fffffff);
}

static inline uint32_t hashNumber(double value) {
  return hashBits(valueToNum(value));
}

static uint32_t hashObject(Obj *object) {
  switch (object->type) {
  case OBJ_CLASS:
    return ((ObjClass *)object)->name->hash;
  case OBJ_FUNCTION: {
    ObjFn *fn = (ObjFn *)object;
    return hashNumber(fn->arity) ^ hashNumber(fn->chunk.cnt);
  }
  case OBJ_STRING:
    return ((ObjString *)object)->hash;
  case OBJ_BOUND_METHOD:
  case OBJ_CLOSURE:
  case OBJ_INSTANCE:
  case OBJ_NATIVE:
  case OBJ_UPVALUE:
  default:
    printf("unreachable");
    return 0;
  }
}

uint32_t hashValue(Value value) {
#ifdef NAN_BOXING
  if (IS_OBJ(value)) {
    return hashObject(AS_OBJ(value));
  }
  return hashBits(value);
#else
  switch (value.type) {
  case VAL_BOOL:
    return AS_BOOL(value) ? 3 : 5;
  case VAL_NIL:
    return 7;
  case VAL_EMPTY:
    return 0;
  case VAL_NUMBER:
    return hashDouble(AS_NUMBER(value));
  case VAL_OBJ:
    return hashObject(AS_OBJ(value));
  default:
    printf("unreachable");
    return 0;
  }
#endif /* ifdef NAN_BOXING */
}
