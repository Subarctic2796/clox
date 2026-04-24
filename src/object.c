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

// used to extend the lifetime of the string
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

void appendToArray(ObjArray *arr, Value value) {
    writeValueArray(&arr->items, value);
}

void storeToArray(ObjArray *arr, int index, Value value) {
    arr->items.values[index] = value;
}

Value indexFromArray(ObjArray *arr, int index) {
    return arr->items.values[index];
}

void deleteFromArray(ObjArray *arr, int index) {
    for (int i = index; i < arr->items.cnt - 1; i++) {
        arr->items.values[i] = arr->items.values[i + 1];
    }
    arr->items.values[arr->items.cnt--] = NIL_VAL;
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

typedef struct {
    char *items;
    size_t cnt, cap;
} StringBuilder;

// we manage our own memory here just to make life easier
static inline int sb_appendf(StringBuilder *sb, const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    // NOTE: the new_capacity needs to be +1 because of the null terminator.
    // However, further below we increase sb->count by n, not n + 1.
    // This is because we don't want the sb to include the null terminator. The
    // user can always sb_append_null() if they want it
    if (sb->cnt + n + 1 > sb->cap) {
        sb->cap = sb->cap == 0 ? 16 : sb->cap * 2;

        while ((sb->cnt + n + 1) > sb->cap) {
            sb->cap *= 2;
        }

        sb->items = (char *)realloc(sb->items, sb->cap * sizeof(*sb->items));
        if (sb->items == NULL) {
            printf("oh no not enough memory");
            exit(1);
        }
    }

    char *dest = sb->items + sb->cnt;
    va_start(args, fmt);
    vsnprintf(dest, n + 1, fmt, args);
    va_end(args);

    sb->cnt += n;

    return n;
}

static inline int getFnStrLen(ObjFn *func) {
    return func->name == NULL ? 8 : 5 + func->name->length;
}

static inline int getValueStrLen(Value value);

static inline int getObjectStrLen(Value value) {
    switch (OBJ_TYPE(value)) {
    case OBJ_NATIVE:   return 11;
    case OBJ_UPVALUE:  return 7;
    case OBJ_CLOSURE:  return getFnStrLen(AS_CLOSURE(value)->fn);
    case OBJ_FUNCTION: return getFnStrLen(AS_FUNCTION(value));
    case OBJ_BOUND_METHOD:
        return getFnStrLen(AS_BOUND_METHOD(value)->method->fn);
    case OBJ_CLASS:    return AS_CLASS(value)->name->length;
    case OBJ_INSTANCE: return AS_INSTANCE(value)->klass->name->length + 9;
    case OBJ_STRING:   return AS_STRING(value)->length;
    case OBJ_ERROR:    return AS_ERROR(value)->msg->length;
    case OBJ_ARRAY:    {
        ValueArray elms = AS_ARRAY(value)->items;
        if (elms.cnt == 0) return 2;

        int len = 2 + 2 * (elms.cnt - 1);
        for (int i = 0; i < elms.cnt; i++) {
            len += getValueStrLen(elms.values[i]);
        }
        return len;
    }
    case OBJ_MAP: {
        Table elms = AS_MAP(value)->items;
        if (elms.cnt == 0) return 2;

        int len = 2 + 2 * (elms.cnt - 1) + 2 * elms.cnt;
        for (int i = 0; i < elms.cap; i++) {
            Entry entry = elms.entries[i];
            if (IS_EMPTY(entry.key)) continue;

            len += getValueStrLen(entry.key);
            len += getValueStrLen(entry.value);
        }
        return len;
    }
    }
}

static inline int getValueStrLen(Value value) {
#ifdef NAN_BOXING
    if (IS_BOOL(value)) {
        return AS_BOOL(value) ? 4 : 5;
    } else if (IS_NIL(value)) {
        return 3;
    } else if (IS_EMPTY(value)) {
        return 7;
    } else if (IS_NUMBER(value)) {
        return snprintf(NULL, 0, "%.14g", AS_NUMBER(value));
    } else if (IS_OBJ(value)) {
        return getObjectStrLen(value);
    }
#else /* ifdef NAN_BOXING */
    switch (value.type) {
    case VAL_NIL:    return 3;
    case VAL_EMPTY:  return 7;
    case VAL_BOOL:   return AS_BOOL(value) ? 4 : 5;
    case VAL_NUMBER: return snprintf(NULL, 0, "%.14g", AS_NUMBER(value));
    case VAL_OBJ:    return getObjectStrLen(value);
    }
#endif
    printf("unreachable\n");
    abort();
}

static inline ObjString *functionToString(ObjFn *func) {
    if (func->name == NULL) return copyString("<script>", 8);
    int len = 5 + func->name->length;
    char *buf = ALLOCATE(char, len + 1);
    snprintf(buf, len + 1, "<fn %s>", func->name->chars);
    return takeString(buf, len);
}

ObjString *objectToString(Value value) {
    // TODO: for arrays and maps we currently using a StringBuilder,
    // this is leading to 2 issues
    //  1) the StringBuilder is managing its own memory:
    //      this just makes stringifing arrays and maps more hairy as
    //      can't rely on the GC to fix our mistakes
    //  2) the StringBuilder is over allocating
    //      this forces us to make a copy of the StringBuilder's buffer
    //      instead of just reusing that buffer

    switch (OBJ_TYPE(value)) {
    case OBJ_ARRAY: {
        ValueArray elms = AS_ARRAY(value)->items;
        if (elms.cnt == 0) return copyString("[]", 2);

        // half manage our own memory
        StringBuilder sb = {0};
        sb_appendf(&sb, "[");
        for (int i = 0; i < elms.cnt; i++) {
            ObjString *str = valueToString(elms.values[i]);
            pushRoot(OBJ_VAL(str)); // make sure its safe
            sb_appendf(&sb, "%s", str->chars);
            popRoot();

            if (i != elms.cnt - 1) sb_appendf(&sb, ", ");
        }
        sb_appendf(&sb, "]N");
        sb.items[--sb.cnt] = '\0';

        sb.items = (char *)realloc(sb.items, sizeof(char) * sb.cnt + 1);
        ObjString *str = takeString(sb.items, sb.cnt);

        // ObjString *str = copyString(sb.items, sb.cnt);
        // free(sb.items);
        return str;
    }
    case OBJ_MAP: {
        Table elms = AS_MAP(value)->items;
        if (elms.cnt == 0) return copyString("{}", 2);

        // half manage our own memory
        StringBuilder sb = {0};
        sb_appendf(&sb, "{");
        bool first = true;
        for (int i = 0; i < elms.cap; i++) {
            Entry entry = elms.entries[i];
            if (IS_EMPTY(entry.key)) continue;

            if (!first) sb_appendf(&sb, ", ");
            first = false;

            ObjString *keyStr = valueToString(entry.key);
            pushRoot(OBJ_VAL(keyStr));
            sb_appendf(&sb, "%s", keyStr->chars);
            popRoot();

            sb_appendf(&sb, ": ");

            ObjString *valStr = valueToString(entry.value);
            pushRoot(OBJ_VAL(valStr));
            sb_appendf(&sb, "%s", valStr->chars);
            popRoot();
        }
        sb_appendf(&sb, "}N");
        sb.items[--sb.cnt] = '\0';

        ObjString *str = copyString(sb.items, sb.cnt);
        free(sb.items);
        return str;
    }
    case OBJ_INSTANCE: {
        ObjString *className = AS_INSTANCE(value)->klass->name;
        int len = 9 + className->length;
        char *buf = ALLOCATE(char, len + 1);
        snprintf(buf, len + 1, "%s instance", className->chars);
        return takeString(buf, len);
    }
    case OBJ_NATIVE:   return copyString("<native fn>", 11);
    case OBJ_UPVALUE:  return copyString("upvalue", 7);
    case OBJ_CLASS:    return AS_CLASS(value)->name;
    case OBJ_STRING:   return AS_STRING(value);
    case OBJ_ERROR:    return AS_ERROR(value)->msg;
    case OBJ_CLOSURE:  return functionToString(AS_CLOSURE(value)->fn);
    case OBJ_FUNCTION: return functionToString(AS_FUNCTION(value));
    case OBJ_BOUND_METHOD:
        return functionToString(AS_BOUND_METHOD(value)->method->fn);
    default: printf("unreachable\n"); return NULL;
    }
}
