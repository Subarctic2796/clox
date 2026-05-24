#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "common.h"
#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "vm.h"

#define ALLOCATE_OBJ(type, objectType)                                         \
    (type *)allocateObject(sizeof(type), objectType)

static Obj *allocateObject(size_t size, ObjType type) {
    Obj *object = (Obj *)reallocate(NULL, 0, size);
    object->type = type;
    object->isMarked = false;

    object->next = vm.objects;
    vm.objects = object;
#ifdef DEBUG_LOG_GC
    printf("%p allocate %zu for %s\n", (void *)object, size,
           ObjTypeString(type));
#endif // ifdef DEBUG_LOG_GC
    return object;
}

ObjBoundMethod *newBoundMethod(Value receiver, ObjClosure *method) {
    ObjBoundMethod *bound = ALLOCATE_OBJ(ObjBoundMethod, OBJ_BOUND_METHOD);
    bound->receiver = receiver;
    bound->method = method;
    return bound;
}

ObjClass *newClass(ObjString *name) {
    ObjClass *klass = ALLOCATE_OBJ(ObjClass, OBJ_CLASS);
    klass->name = name;
    initTable(&klass->methods);
    return klass;
}

ObjClosure *newClosure(ObjFn *function) {
    ObjUpvalue **upvalues = ALLOCATE(ObjUpvalue *, function->upvalueCnt);
    for (int i = 0; i < function->upvalueCnt; i++) {
        upvalues[i] = NULL;
    }

    ObjClosure *closure = ALLOCATE_OBJ(ObjClosure, OBJ_CLOSURE);
    closure->fn = function;
    closure->upvalues = upvalues;
    closure->upvalueCnt = function->upvalueCnt;
    return closure;
}

ObjFn *newFunction(void) {
    ObjFn *function = ALLOCATE_OBJ(ObjFn, OBJ_FUNCTION);
    function->arity = 0;
    function->upvalueCnt = 0;
    function->name = NULL;
    initChunk(&function->chunk);
    return function;
}

ObjInstance *newInstance(ObjClass *klass) {
    ObjInstance *instance = ALLOCATE_OBJ(ObjInstance, OBJ_INSTANCE);
    instance->klass = klass;
    initTable(&instance->fields);
    return instance;
}

ObjNative *newNative(NativeFn function) {
    ObjNative *native = ALLOCATE_OBJ(ObjNative, OBJ_NATIVE);
    native->function = function;
    return native;
}

static ObjString *allocateString(char *chars, int length, uint32_t hash) {
    ObjString *string = ALLOCATE_OBJ(ObjString, OBJ_STRING);
    string->length = length;
    string->chars = chars;
    string->hash = hash;

    pushRoot(OBJ_VAL(string)); // make sure not collect by mistake
    tableSet(&vm.strings, OBJ_VAL(string), NIL_VAL);
    popRoot();

    return string;
}

static uint32_t hashString(const char *key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

// used for dynamically allocated items
ObjString *takeString(char *chars, int length) {
    uint32_t hash = hashString(chars, length);
    ObjString *interned = tableFindString(&vm.strings, chars, length, hash);
    if (interned != NULL) {
        FREE_ARRAY(char, chars, length + 1);
        return interned;
    }
    return allocateString(chars, length, hash);
}

// used to extend the lifetime of the string for the vm
// ie for in the compiler as the tokens are views into the source
// if its a static string
ObjString *copyString(const char *chars, int length) {
    uint32_t hash = hashString(chars, length);
    ObjString *interned = tableFindString(&vm.strings, chars, length, hash);
    if (interned != NULL) return interned;
    char *heapChars = ALLOCATE(char, length + 1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';
    return allocateString(heapChars, length, hash);
}

ObjError *newError(bool recoverable, const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    size_t len = vsnprintf(NULL, 0, fmt, va);
    va_end(va);

    va_start(va, fmt);
    char *buf = ALLOCATE(char, len + 1);
    vsnprintf(buf, len + 1, fmt, va);
    va_end(va);

    ObjString *msg = takeString(buf, len);
    pushRoot(OBJ_VAL(msg)); // keep msg safe
    ObjError *err = ALLOCATE_OBJ(ObjError, OBJ_ERROR);
    popRoot();

    // set up error
    err->msg = msg;
    err->recoverable = recoverable;
    return err;
}

ObjUpvalue *newUpvalue(Value *slot) {
    ObjUpvalue *upvalue = ALLOCATE_OBJ(ObjUpvalue, OBJ_UPVALUE);
    upvalue->closed = NIL_VAL;
    upvalue->location = slot;
    upvalue->next = NULL;
    return upvalue;
}

ObjMap *newMap() {
    ObjMap *map = ALLOCATE_OBJ(ObjMap, OBJ_MAP);
    initTable(&map->items);
    return map;
}

ObjArray *newArray() {
    ObjArray *arr = ALLOCATE_OBJ(ObjArray, OBJ_ARRAY);
    initValueArray(&arr->items);
    return arr;
}

static inline void printFunction(ObjFn *func) {
    if (func->name == NULL) {
        printf("<script>");
        return;
    }
    printf("<fn %s>", func->name->chars);
}

void printObject(Value value) {
    switch (OBJ_TYPE(value)) {
    case OBJ_ARRAY: {
        printf("[");
        ValueArray elms = AS_ARRAY(value)->items;
        for (int i = 0; i < elms.cnt; i++) {
            printValue(elms.values[i]);
            if (i != elms.cnt - 1) printf(", ");
        }
        printf("]");
    } break;
    case OBJ_MAP: {
        printf("{");
        Table elms = AS_MAP(value)->items;
        bool first = true;
        for (int i = 0; i < elms.cap; i++) {
            Entry entry = elms.entries[i];
            if (IS_EMPTY(entry.key)) continue;

            if (!first) printf(", ");
            first = false;
            printValue(entry.key);
            printf(": ");
            printValue(entry.value);
        }
        printf("}");
    } break;
    case OBJ_BOUND_METHOD:
        printFunction(AS_BOUND_METHOD(value)->method->fn);
        break;
    case OBJ_INSTANCE:
        printf("%s instance", AS_INSTANCE(value)->klass->name->chars);
        break;
    case OBJ_CLOSURE:  printFunction(AS_CLOSURE(value)->fn); break;
    case OBJ_FUNCTION: printFunction(AS_FUNCTION(value)); break;
    case OBJ_CLASS:    printf("%s", AS_CLASS(value)->name->chars); break;
    case OBJ_NATIVE:   printf("<native fn>"); break;
    case OBJ_STRING:   printf("%s", AS_CSTRING(value)); break;
    case OBJ_ERROR:    printf("%s", AS_ERROR_MSG(value)); break;
    case OBJ_UPVALUE:  printf("upvalue"); break;
    }
}
