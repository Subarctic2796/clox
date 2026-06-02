#ifndef INCLUDE_CLOX_OBJECT_H_
#define INCLUDE_CLOX_OBJECT_H_

#include <math.h>

#include "chunk.h"
#include "common.h"
#include "table.h"
#include "value.h"

#define OBJ_TYPE(value) (AS_OBJ(value)->type)

#define IS_BOUND_METHOD(value) isObjType(value, OBJ_BOUND_METHOD)
#define IS_CLASS(value)        isObjType(value, OBJ_CLASS)
#define IS_CLOSURE(value)      isObjType(value, OBJ_CLOSURE)
#define IS_FUNCTION(value)     isObjType(value, OBJ_FUNCTION)
#define IS_INSTANCE(value)     isObjType(value, OBJ_INSTANCE)
#define IS_NATIVE(value)       isObjType(value, OBJ_NATIVE)
#define IS_STRING(value)       isObjType(value, OBJ_STRING)
#define IS_ERROR(value)        isObjType(value, OBJ_ERROR)
#define IS_ARRAY(value)        isObjType(value, OBJ_ARRAY)
#define IS_MAP(value)          isObjType(value, OBJ_MAP)

#define AS_BOUND_METHOD(value) ((ObjBoundMethod *)AS_OBJ(value))
#define AS_CLASS(value)        ((ObjClass *)AS_OBJ(value))
#define AS_CLOSURE(value)      ((ObjClosure *)AS_OBJ(value))
#define AS_FUNCTION(value)     ((ObjFn *)AS_OBJ(value))
#define AS_INSTANCE(value)     ((ObjInstance *)AS_OBJ(value))
#define AS_NATIVE(value)       (((ObjNative *)AS_OBJ(value))->function)
#define AS_ARRAY(value)        ((ObjArray *)AS_OBJ(value))
#define AS_MAP(value)          ((ObjMap *)AS_OBJ(value))
#define AS_STRING(value)       ((ObjString *)AS_OBJ(value))
#define AS_CSTRING(value)      (((ObjString *)AS_OBJ(value))->chars)
#define AS_ERROR(value)        ((ObjError *)AS_OBJ(value))
#define AS_ERROR_MSG(value)    (((ObjError *)AS_OBJ(value))->msg->chars)

typedef enum {
    OBJ_BOUND_METHOD,
    OBJ_CLASS,
    OBJ_CLOSURE,
    OBJ_FUNCTION,
    OBJ_INSTANCE,
    OBJ_NATIVE,
    OBJ_STRING,
    OBJ_ERROR,
    OBJ_UPVALUE,
    OBJ_ARRAY,
    OBJ_MAP,
} ObjType;

static inline const char *ObjTypeString(ObjType t) {
    static const char *strings[] = {
        [OBJ_BOUND_METHOD] = "OBJ_BOUND_METHOD",
        [OBJ_CLASS] = "OBJ_CLASS",
        [OBJ_CLOSURE] = "OBJ_CLOSURE",
        [OBJ_FUNCTION] = "OBJ_FUNCTION",
        [OBJ_INSTANCE] = "OBJ_INSTANCE",
        [OBJ_NATIVE] = "OBJ_NATIVE",
        [OBJ_STRING] = "OBJ_STRING",
        [OBJ_ERROR] = "OBJ_ERROR",
        [OBJ_UPVALUE] = "OBJ_UPVALUE",
        [OBJ_ARRAY] = "OBJ_ARRAY",
        [OBJ_MAP] = "OBJ_MAP",
    };
    return strings[t];
}

struct Obj {
    ObjType type;
    bool isMarked;
    struct Obj *next;
};

typedef struct {
    Obj obj;
    int arity;
    int upvalueCnt;
    Chunk chunk;
    ObjString *name;
} ObjFn;

typedef Value (*NativeFn)(VM *vm, int argc, Value *args);

typedef struct {
    Obj obj;
    NativeFn function;
} ObjNative;

struct ObjString {
    Obj obj;
    int length;
    uint32_t hash;
    char *chars;
};

typedef struct {
    Obj obj;
    ObjString *msg;
    bool recoverable;
} ObjError;

typedef struct ObjUpvalue {
    Obj obj;
    Value *location;
    Value closed;
    struct ObjUpvalue *next;
} ObjUpvalue;

typedef struct {
    Obj obj;
    ObjFn *fn;
    ObjUpvalue **upvalues;
    int upvalueCnt;
} ObjClosure;

typedef struct {
    Obj obj;
    ObjString *name;
    Table methods;
} ObjClass;

typedef struct {
    Obj obj;
    ObjClass *klass;
    Table fields;
} ObjInstance;

typedef struct {
    Obj obj;
    Value receiver;
    ObjClosure *method;
} ObjBoundMethod;

typedef struct {
    Obj obj;
    ValueArray items;
} ObjArray;

typedef struct {
    Obj obj;
    Table items;
} ObjMap;

ObjArray *newArray(VM *vm);
ObjMap *newMap(VM *vm);
ObjBoundMethod *newBoundMethod(VM *vm, Value reciever, ObjClosure *method);
ObjClass *newClass(VM *vm, ObjString *name);
ObjClosure *newClosure(VM *vm, ObjFn *fn);
ObjFn *newFunction(VM *vm);
ObjInstance *newInstance(VM *vm, ObjClass *klass);
ObjNative *newNative(VM *vm, NativeFn function);
ObjUpvalue *newUpvalue(VM *vm, Value *slot);
ObjError *newError(VM *vm, bool recoverable, const char *fmt, ...);

// used for dynamically allocated items
ObjString *takeString(VM *vm, char *chars, int length);

// used to extend the lifetime of the string for the vm
// ie for in the compiler as the tokens are views into the source
// if its a static string
ObjString *copyString(VM *vm, const char *chars, int length);

// for string literals
#define CONST_STRING(txt) copyString(vm, txt, sizeof(txt) - 1)

void printObject(Value value);
int objectStringLength(Value value);
int objectToStringX(Value value, char *buf, int offset);
ObjString *objectToString(VM *vm, Value value);

static inline void appendToArray(VM *vm, ObjArray *arr, Value value) {
    writeValueArray(vm, &arr->items, value);
}

static inline void storeToArray(ObjArray *arr, int index, Value value) {
    arr->items.values[index] = value;
}

static inline Value indexFromArray(ObjArray *arr, int index) {
    return arr->items.values[index];
}

static inline void deleteFromArray(ObjArray *arr, int index) {
    for (int i = index; i < arr->items.cnt - 1; i++) {
        arr->items.values[i] = arr->items.values[i + 1];
    }
    arr->items.values[arr->items.cnt--] = NIL_VAL;
}

// returns -1 if value is not a number
// returns -2 if value is not an integer
// returns -3 if index is out of bounds
// returns a +ve num on success
static inline int isValidIndex(Value value, int len) {
    if (!IS_NUMBER(value)) return -1;
    double index = AS_NUMBER(value);
    if (trunc(index) != index) return -2;
    if ((int)index >= 0 && (int)index < len) return (int)index;
    return -3;
}

static inline bool isObjType(Value value, ObjType type) {
    return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

static inline bool isIndexable(Value value) {
    return IS_STRING(value) || IS_ARRAY(value) || IS_MAP(value);
}

static inline bool isHashable(Value value) {
    return IS_NUMBER(value) || IS_BOOL(value) || IS_NIL(value) ||
           IS_STRING(value) || IS_ERROR(value) || IS_INSTANCE(value);
}

#endif // INCLUDE_CLOX_OBJECT_H_
