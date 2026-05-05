#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

#include "chunk.h"
#include "common.h"
#include "compiler.h"
#include "table.h"
#include "value.h"

#ifdef DEBUG_TRACE_EXECUTION
#include "debug.h"
#endif

#include "memory.h"
#include "object.h"
#include "vm.h"

VM vm = {0};

static inline void push(Value value) { *vm.sp++ = value; }
static inline Value pop(void) { return *(--vm.sp); }
static inline Value peek(int dist) { return vm.sp[-1 - dist]; }

#define CHECK_ARITY_NATIVE(arity)                                              \
    if (argc != arity) {                                                       \
        return ERROR_VAL(false, "Expected " #arity " arguments but got %d",    \
                         argc);                                                \
    }

static Value clockNative(int argc, Value *args) {
    (void)argc;
    (void)args;
    return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

// append a value to the array
static Value appendNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(2);
    if (!IS_ARRAY(args[0])) {
        return ERROR_VAL(false, "Can only append to arrays");
    }

    appendToArray(AS_ARRAY(args[0]), args[1]);
    return NIL_VAL;
}

// delete an item from the array or map at index
static Value deleteNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(2);
    if (!(IS_ARRAY(args[0]) || IS_MAP(args[0]))) {
        return ERROR_VAL(false,
                         "Can only use 'delete' on maps and arrays, got %s",
                         typeofValue(args[0]));
    }

    if (IS_ARRAY(args[0])) {
        ObjArray *arr = AS_ARRAY(args[0]);
        int index = isValidIndex(args[1], arr->items.cnt);

        if (index == -1) {
            return ERROR_VAL(false, "Can only use numbers to index arrays");
        } else if (index == -2) {
            return ERROR_VAL(false,
                             "can only use integers to index into arrays");
        } else if (index == -3) {
            return ERROR_VAL(false, "index out of bounds");
        }

        deleteFromArray(arr, index);
        return NIL_VAL;
    } else if (IS_MAP(args[0])) {
        ObjMap *map = AS_MAP(args[0]);
        Value key = args[1];
        if (!isHashable(key)) {
            return ERROR_VAL(false, "%s is an unhashable type",
                             typeofValue(key));
        }

        tableDelete(&map->items, key);
        return NIL_VAL;
    }
    return NIL_VAL;
}

// returns length of a string, array or map
static Value lenNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(1);

    if (IS_STRING(args[0])) {
        return NUMBER_VAL(AS_STRING(args[0])->length);
    } else if (IS_ARRAY(args[0])) {
        return NUMBER_VAL(AS_ARRAY(args[0])->items.cnt);
    } else if (IS_MAP(args[0])) {
        return NUMBER_VAL(AS_MAP(args[0])->items.cnt);
    }
    return ERROR_VAL(false,
                     "Can only take the length of strings, arrays, and maps");
}

static Value errorNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(1);
    return ERROR_VAL(AS_BOOL(args[0]), "this is a recoverable error");
}

static Value typeofNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(1);
    Value v = args[0];
    if (IS_CLASS(v)) {
        return OBJ_VAL(AS_CLASS(v)->name);
    } else if (IS_INSTANCE(v)) {
        ObjString *name = AS_INSTANCE(v)->klass->name;

        int len = name->length + 9;
        char *buf = ALLOCATE(char, len + 1);
        snprintf(buf, len + 1, "%s instance", name->chars);

        return OBJ_VAL(takeString(buf, len));
    }
    const char *str = typeofValue(v);
    return OBJ_VAL(copyString(str, (int)strnlen(str, 17)));
}

static Value keysNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(1);
    Value v = args[0];
    if (!IS_MAP(v)) {
        return ERROR_VAL(false, "'keys' can only be used on maps, got %s",
                         typeofValue(v));
    }

    Table map = AS_MAP(v)->items;

    ObjArray *arr = newArray();
    pushRoot(OBJ_VAL(arr));
    for (int i = 0; i < map.cap; i++) {
        Entry entry = map.entries[i];
        if (IS_EMPTY(entry.key)) continue;
        appendToArray(arr, entry.key);
    }
    popRoot();
    return OBJ_VAL(arr);
}

typedef struct {
    const char *name;
    const NativeFn fn;
} NativeClassFn;

static Value iterInitNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(1);
    if (!isIndexable(args[0])) {
        return ERROR_VAL(
            false, "Can only create iterators from strings, arrays, and maps");
    }

    ObjInstance *inst = AS_INSTANCE(args[-1]);

    ObjString *obj = copyString("obj", 3);
    pushRoot(OBJ_VAL(obj));
    ObjString *idx = copyString("_index", 6);
    pushRoot(OBJ_VAL(idx));

    // add obj and _index to the instance's fields
    tableSet(&inst->fields, OBJ_VAL(obj), args[0]);
    tableSet(&inst->fields, OBJ_VAL(idx), NUMBER_VAL(0));

    popRoot(); // obj
    popRoot(); // idx

    return OBJ_VAL(inst);
}

static Value iterNextNative(int argc, Value *args) {
#define OBJ_HASH 3343205242
#define IDX_HASH 1364385362

    CHECK_ARITY_NATIVE(0);

    ObjInstance *iter = AS_INSTANCE(args[-1]);
    ObjString *_objStr = tableFindString(&iter->fields, "obj", 3, OBJ_HASH);
    ObjString *_idxStr = tableFindString(&iter->fields, "_index", 6, IDX_HASH);

    Value obj, idx;
    tableGet(&iter->fields, OBJ_VAL(_objStr), &obj);
    tableGet(&iter->fields, OBJ_VAL(_idxStr), &idx);

    int index = AS_NUMBER(idx);
    // update index
    tableSet(&iter->fields, OBJ_VAL(_idxStr), NUMBER_VAL(index + 1));

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (OBJ_TYPE(obj)) {
    case OBJ_STRING: return BOOL_VAL(index < AS_STRING(obj)->length);
    case OBJ_ARRAY:  return BOOL_VAL(index < AS_ARRAY(obj)->items.cnt);
    case OBJ_MAP:    {
        printf("todo iterNextNative[map]\n");
        abort();
    } break;
    default: return NIL_VAL;
    }
#pragma GCC diagnostic pop

    return NIL_VAL;

#undef OBJ_HASH
#undef IDX_HASH
}

static Value iterValueNative(int argc, Value *args) {
#define OBJ_HASH 3343205242
#define IDX_HASH 1364385362

    CHECK_ARITY_NATIVE(0);

    ObjInstance *iter = AS_INSTANCE(args[-1]);
    ObjString *_objStr = tableFindString(&iter->fields, "obj", 3, OBJ_HASH);
    ObjString *_idxStr = tableFindString(&iter->fields, "_index", 6, IDX_HASH);

    Value obj, idx;
    tableGet(&iter->fields, OBJ_VAL(_objStr), &obj);
    tableGet(&iter->fields, OBJ_VAL(_idxStr), &idx);

    int index = AS_NUMBER(idx) - 1;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (OBJ_TYPE(obj)) {
    case OBJ_STRING: return OBJ_VAL(copyString(AS_CSTRING(obj) + index, 1));
    case OBJ_ARRAY:  return AS_ARRAY(obj)->items.values[index];
    case OBJ_MAP:    {
        printf("todo iterValueNative[map]\n");
        abort();
    } break;
    default: return NIL_VAL;
    }
#pragma GCC diagnostic pop

    return NIL_VAL;

#undef OBJ_HASH
#undef IDX_HASH
}

static Value iterIndexNative(int argc, Value *args) {
#define IDX_HASH 1364385362

    CHECK_ARITY_NATIVE(0);

    ObjInstance *iter = AS_INSTANCE(args[-1]);
    ObjString *_idxStr = tableFindString(&iter->fields, "_index", 6, IDX_HASH);

    Value idx;
    tableGet(&iter->fields, OBJ_VAL(_idxStr), &idx);
    return NUMBER_VAL(AS_NUMBER(idx) - 1);

#undef IDX_HASH
}

static void defineNativeClass(const char *name, int nfns,
                              const NativeClassFn *fns) {
    // add class to globals
    ObjString *kname = copyString(name, (int)strnlen(name, 1024));
    pushRoot(OBJ_VAL(kname));
    ObjClass *klass = newClass(kname);
    pushRoot(OBJ_VAL(klass));
    tableSet(&vm.globals, OBJ_VAL(kname), OBJ_VAL(klass));

    // add native functions to the class
    for (int i = 0; i < nfns; i++) {
        NativeClassFn fn = fns[i];
        ObjString *fname = copyString(fn.name, (int)strnlen(fn.name, 1024));
        pushRoot(OBJ_VAL(fname));
        ObjNative *native = newNative(fn.fn);
        pushRoot(OBJ_VAL(native));
        tableSet(&klass->methods, OBJ_VAL(fname), OBJ_VAL(native));
        popRoot(); // fn name
        popRoot(); // fn
    }

    popRoot(); // class name
    popRoot(); // class
}

static void resetStack(void) {
    vm.sp = vm.stack;
    vm.frameCount = 0;
    vm.openUpvalues = NULL;
    vm.tempCnt = 0;
}

static void runtimeError(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = vm.frameCount - 1; i >= 0; i--) {
        CallFrame *frame = &vm.frames[i];
        ObjFn *fn = frame->closure->fn;
        // '-1' because READ_BYTE already advanced the ip
        size_t inst = frame->ip - fn->chunk.code - 1;
        fprintf(stderr, "[line %d] in ", getLine(&fn->chunk, inst));
        if (fn->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", fn->name->chars);
        }
    }

    resetStack();
}

static void defineNative(const char *name, NativeFn function) {
    ObjString *nativeName = copyString(name, (int)strnlen(name, 1024));
    pushRoot(OBJ_VAL(nativeName));
    ObjNative *fn = newNative(function);
    pushRoot(OBJ_VAL(fn));
    tableSet(&vm.globals, OBJ_VAL(nativeName), OBJ_VAL(fn));
    popRoot(); // pop native ptr
    popRoot(); // pop native name
}

static const NativeClassFn ITER_FNS[] = {
    {"init", iterInitNative},
    {"next", iterNextNative},
    {"value", iterValueNative},
    {"index", iterIndexNative},
};

void initVM(void) {
    resetStack();
    vm.objects = NULL;
    vm.bytesAllocated = 0;
    vm.nextGC = 1024 * 1024; // 1mib

    vm.grayCnt = 0;
    vm.grayCap = 0;
    vm.grayStack = NULL;

    vm.tempCnt = 0;

    initTable(&vm.globals);

    initTable(&vm.strings);

    vm.initString = NULL;
    vm.initString = copyString("init", 4);

    defineNative("clock", clockNative);
    defineNative("append", appendNative);
    defineNative("delete", deleteNative);
    defineNative("len", lenNative);
    defineNative("error", errorNative);
    defineNative("typeof", typeofNative);
    defineNative("keys", keysNative);

    defineNativeClass("Iter", 4, ITER_FNS);
}

void freeVM(void) {
    freeTable(&vm.globals);
    freeTable(&vm.strings);
    vm.initString = NULL;
    freeObjects();
}

static bool call(ObjClosure *closure, int argc) {
    if (argc != closure->fn->arity) {
        runtimeError("Expected %d arguments but got %d", closure->fn->arity,
                     argc);
        return false;
    }

    if (vm.frameCount == FRAMES_MAX) {
        runtimeError("Stack overflow");
        return false;
    }

    // updates the VM's frame ip
    CallFrame *frame = &vm.frames[vm.frameCount++];
    frame->closure = closure;
    frame->ip = closure->fn->chunk.code;
    frame->slots = vm.sp - argc - 1;
    return true;
}

static bool callValue(Value callee, int argCnt) {
    if (IS_OBJ(callee)) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
        switch (OBJ_TYPE(callee)) {
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
            vm.sp[-argCnt - 1] = bound->receiver;
            return call(bound->method, argCnt);
        }
        case OBJ_CLASS: {
            ObjClass *klass = AS_CLASS(callee);
            vm.sp[-argCnt - 1] = OBJ_VAL(newInstance(klass));
            Value init;
            if (tableGet(&klass->methods, OBJ_VAL(vm.initString), &init)) {
                if (IS_CLOSURE(init)) return call(AS_CLOSURE(init), argCnt);

                NativeFn native = AS_NATIVE(init);
                Value result = native(argCnt, vm.sp - argCnt);
                if (IS_ERROR(result) && !AS_ERROR(result)->recoverable) {
                    runtimeError(AS_ERROR_MSG(result));
                    return false;
                }
                vm.sp -= argCnt + 1;
                push(result);
                return true;
            } else if (argCnt != 0) {
                runtimeError("Expected 0 arguments but got %d", argCnt);
                return false;
            }
            return true;
        }
        case OBJ_CLOSURE: return call(AS_CLOSURE(callee), argCnt);
        case OBJ_NATIVE:  {
            NativeFn native = AS_NATIVE(callee);
            Value result = native(argCnt, vm.sp - argCnt);
            if (IS_ERROR(result) && !AS_ERROR(result)->recoverable) {
                runtimeError(AS_ERROR_MSG(result));
                return false;
            }
            vm.sp -= argCnt + 1;
            push(result);
            return true;
        }
        default: break; // non-callable object type
        }
#pragma GCC diagnostic pop
    }
    runtimeError("Can only call functions and classes");
    return false;
}

static inline bool invokeFromClass(ObjClass *klass, Value name, int argc) {
    Value method;
    if (!tableGet(&klass->methods, name, &method)) {
        runtimeError("Undefined property '%s'", AS_CSTRING(name));
        return false;
    }
    // return call(AS_CLOSURE(method), argc);
    return callValue(method, argc);
}

static bool invoke(Value name, int argCnt) {
    Value receiver = peek(argCnt);

    if (!IS_INSTANCE(receiver)) {
        runtimeError("Only instances have methods");
        return false;
    }

    ObjInstance *instance = AS_INSTANCE(receiver);

    Value value;
    if (tableGet(&instance->fields, name, &value)) {
        vm.sp[-argCnt - 1] = value;
        return callValue(value, argCnt);
    }

    return invokeFromClass(instance->klass, name, argCnt);
}

static bool bindMethod(ObjClass *klass, Value name) {
    Value method;
    if (!tableGet(&klass->methods, name, &method)) {
        runtimeError("Undefined property '%s'", AS_CSTRING(name));
        return false;
    }

    ObjBoundMethod *bound = newBoundMethod(peek(0), AS_CLOSURE(method));
    pop(); // pop 'this'
    push(OBJ_VAL(bound));
    return true;
}

static ObjUpvalue *captureUpvalue(Value *local) {
    ObjUpvalue *prv = NULL;
    ObjUpvalue *upvalue = vm.openUpvalues;
    while (upvalue != NULL && upvalue->location > local) {
        prv = upvalue;
        upvalue = upvalue->next;
    }

    if (upvalue != NULL && upvalue->location == local) return upvalue;

    ObjUpvalue *createdUpvalue = newUpvalue(local);
    createdUpvalue->next = upvalue;

    if (prv == NULL) {
        vm.openUpvalues = createdUpvalue;
    } else {
        prv->next = createdUpvalue;
    }

    return createdUpvalue;
}

static void closeUpvalues(Value *last) {
    while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
        ObjUpvalue *upvalue = vm.openUpvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm.openUpvalues = upvalue->next;
    }
}

static void defineMethod(Value name) {
    Value method = peek(0);
    ObjClass *klass = AS_CLASS(peek(1));
    tableSet(&klass->methods, name, method);
    pop();
}

static inline bool isFalsey(Value value) {
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate(void) {
    ObjString *b = AS_STRING(peek(0));
    ObjString *a = AS_STRING(peek(1));

    int length = a->length + b->length;
    char *chars = ALLOCATE(char, length + 1);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    ObjString *result = takeString(chars, length);
    vm.sp -= 2; // pop the 2 strings ontop of the stack
    push(OBJ_VAL(result));
}

static InterpretResult run(void) {
    CallFrame *frame = &vm.frames[vm.frameCount - 1];

#define PUSH(value) (*vm.sp++ = value)
#define POP()       (*(--vm.sp))
#define PEEK(dist)  (*(vm.sp - 1 - dist))
#define READ_BYTE() (*frame->ip++)
#define READ_SHORT()                                                           \
    (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONST()  (frame->closure->fn->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONST())
#define BINARY_OP(valueType, op)                                               \
    do {                                                                       \
        if (!IS_NUMBER(PEEK(0)) || !IS_NUMBER(PEEK(1))) {                      \
            runtimeError("Operands must be numbers");                          \
            return INTERPRET_RUNTIME_ERR;                                      \
        }                                                                      \
        double b = AS_NUMBER(POP());                                           \
        double a = AS_NUMBER(POP());                                           \
        PUSH(valueType(a op b));                                               \
    } while (false)

#ifdef DEBUG_TRACE_EXECUTION
#define TRACE_EXECUTION()                                                      \
    do {                                                                       \
        printf("          ");                                                  \
        for (Value *slot = vm.stack; slot < vm.sp; slot++) {                   \
            printf("[ ");                                                      \
            printValue(*slot);                                                 \
            printf(" ]");                                                      \
        }                                                                      \
        printf("\n");                                                          \
        disassembleInst(&frame->closure->fn->chunk,                            \
                        (int)(frame->ip - frame->closure->fn->chunk.code));    \
    } while (false)
#else
#define TRACE_EXECUTION()                                                      \
    do {                                                                       \
    } while (false)
#endif

    uint8_t inst;
    for (;;) {
        TRACE_EXECUTION();
        switch (inst = (OpCode)READ_BYTE()) {
        case OP_CONSTANT: {
            Value constant = READ_CONST();
            PUSH(constant);
        } break;
        case OP_NIL:       PUSH(NIL_VAL); break;
        case OP_TRUE:      PUSH(BOOL_VAL(true)); break;
        case OP_FALSE:     PUSH(BOOL_VAL(false)); break;
        case OP_POP:       (void)POP(); break;
        case OP_GET_INDEX: {
            if (!isIndexable(PEEK(1))) {
                runtimeError("%s is not an indexable type",
                             typeofValue(PEEK(1)));
                return INTERPRET_RUNTIME_ERR;
            }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
            switch (OBJ_TYPE(PEEK(1))) {
            case OBJ_STRING: {
                Value index_ = POP();
                ObjString *str = AS_STRING(POP());

                int index = isValidIndex(index_, str->length);
                if (index == -1) {
                    runtimeError("Can only use numbers to index strings");
                    return INTERPRET_RUNTIME_ERR;
                } else if (index == -2) {
                    runtimeError("can only use integers to index into strings");
                    return INTERPRET_RUNTIME_ERR;
                } else if (index == -3) {
                    runtimeError("index out of bounds");
                    return INTERPRET_RUNTIME_ERR;
                }

                ObjString *result = copyString(str->chars + index, 1);
                PUSH(OBJ_VAL(result));
            } break;
            case OBJ_ARRAY: {
                Value index_ = POP();
                ObjArray *arr = AS_ARRAY(POP());

                int index = isValidIndex(index_, arr->items.cnt);
                if (index == -1) {
                    runtimeError("Can only use numbers to index arrays");
                    return INTERPRET_RUNTIME_ERR;
                } else if (index == -2) {
                    runtimeError("can only use integers to index into arrays");
                    return INTERPRET_RUNTIME_ERR;
                } else if (index == -3) {
                    runtimeError("index out of bounds");
                    return INTERPRET_RUNTIME_ERR;
                }

                PUSH(indexFromArray(arr, index));
            } break;
            case OBJ_MAP: {
                Value key = POP();
                if (!isHashable(key)) {
                    runtimeError("%s is an unhashable type", typeofValue(key));
                    return INTERPRET_RUNTIME_ERR;
                }

                ObjMap *map = AS_MAP(POP());
                Value result = NIL_VAL;
                tableGet(&map->items, key, &result);
                PUSH(result);
            } break;
            default: break;
            }
#pragma GCC diagnostic pop
        } break;
        case OP_SET_INDEX: {
            if (!isIndexable(PEEK(2))) {
                runtimeError("%s is not an indexable type",
                             typeofValue(PEEK(1)));
                return INTERPRET_RUNTIME_ERR;
            }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
            switch (OBJ_TYPE(PEEK(2))) {
            case OBJ_STRING: {
                runtimeError("Can not use index setting on strings");
                return INTERPRET_RUNTIME_ERR;
            } break;
            case OBJ_ARRAY: {
                Value value = POP();
                Value index_ = POP();
                ObjArray *arr = AS_ARRAY(POP());

                int index = isValidIndex(index_, arr->items.cnt);
                if (index == -1) {
                    runtimeError("Can only use numbers to index arrays");
                    return INTERPRET_RUNTIME_ERR;
                } else if (index == -2) {
                    runtimeError("can only use integers to index into arrays");
                    return INTERPRET_RUNTIME_ERR;
                } else if (index == -3) {
                    runtimeError("index out of bounds");
                    return INTERPRET_RUNTIME_ERR;
                }

                storeToArray(arr, index, value);
                PUSH(value);
            } break;
            case OBJ_MAP: {
                Value value = POP();
                Value key = POP();
                if (!isHashable(key)) {
                    runtimeError("%s is an unhashable type", typeofValue(key));
                    return INTERPRET_RUNTIME_ERR;
                }

                ObjMap *map = AS_MAP(PEEK(0));
                tableSet(&map->items, key, value);
                (void)POP(); // pop the map
                PUSH(value);
            } break;
            default: break;
            }
#pragma GCC diagnostic pop
        } break;
        case OP_GET_LOCAL: {
            uint8_t slot = READ_BYTE();
            PUSH(frame->slots[slot]);
        } break;
        case OP_SET_LOCAL: {
            uint8_t slot = READ_BYTE();
            frame->slots[slot] = PEEK(0);
        } break;
        case OP_GET_GLOBAL: {
            Value name = READ_CONST();
            Value value;
            if (!tableGet(&vm.globals, name, &value)) {
                runtimeError("Undefined variable '%s'", AS_CSTRING(name));
                return INTERPRET_RUNTIME_ERR;
            }
            PUSH(value);
        } break;
        case OP_DEFINE_GLOBAL: {
            Value name = READ_CONST();
            tableSet(&vm.globals, name, PEEK(0));
            (void)POP();
        } break;
        case OP_SET_GLOBAL: {
            Value name = READ_CONST();
            if (tableSet(&vm.globals, name, PEEK(0))) {
                tableDelete(&vm.globals, name);
                runtimeError("Undefined variable '%s'", AS_CSTRING(name));
                return INTERPRET_RUNTIME_ERR;
            }
        } break;
        case OP_GET_UPVALUE: {
            uint8_t slot = READ_BYTE();
            PUSH(*frame->closure->upvalues[slot]->location);
        } break;
        case OP_SET_UPVALUE: {
            uint8_t slot = READ_BYTE();
            *frame->closure->upvalues[slot]->location = PEEK(0);
        } break;
        case OP_GET_PROPERTY: {
            if (!IS_INSTANCE(PEEK(0))) {
                runtimeError("Only instances have properties");
                return INTERPRET_RUNTIME_ERR;
            }

            ObjInstance *instance = AS_INSTANCE(PEEK(0));
            Value name = READ_CONST();
            Value value;
            if (tableGet(&instance->fields, name, &value)) {
                (void)POP(); // instance
                PUSH(value);
                break;
            }

            if (!bindMethod(instance->klass, name)) {
                return INTERPRET_RUNTIME_ERR;
            }
        } break;
        case OP_SET_PROPERTY: {
            if (!IS_INSTANCE(PEEK(1))) {
                runtimeError("Only instances have fields");
                return INTERPRET_RUNTIME_ERR;
            }
            ObjInstance *instance = AS_INSTANCE(PEEK(1));
            tableSet(&instance->fields, READ_CONST(), PEEK(0));
            Value value = POP();
            (void)POP(); // instance
            PUSH(value);
        } break;
        case OP_GET_SUPER: {
            Value name = READ_CONST();
            ObjClass *superclass = AS_CLASS(POP());

            if (!bindMethod(superclass, name)) return INTERPRET_RUNTIME_ERR;
        } break;
        case OP_EQUAL: {
            Value b = POP();
            Value a = POP();
            PUSH(BOOL_VAL(valuesEqual(a, b)));
        } break;
        case OP_GREATER: BINARY_OP(BOOL_VAL, >); break;
        case OP_LESS:    BINARY_OP(BOOL_VAL, <); break;
        case OP_ADD:     {
            if (IS_STRING(PEEK(0)) && IS_STRING(PEEK(1))) {
                concatenate();
            } else if (IS_NUMBER(PEEK(0)) && IS_NUMBER(PEEK(1))) {
                double b = AS_NUMBER(POP());
                double a = AS_NUMBER(POP());
                PUSH(NUMBER_VAL(a + b));
            } else {
                runtimeError("Operands must be two numbers or two strings");
                return INTERPRET_RUNTIME_ERR;
            }
        } break;
        case OP_MOD: {
            if (!IS_NUMBER(PEEK(0)) || !IS_NUMBER(PEEK(1))) {
                runtimeError("Operands must be numbers");
                return INTERPRET_RUNTIME_ERR;
            }
            double b = AS_NUMBER(POP());
            double a = AS_NUMBER(POP());
            PUSH(NUMBER_VAL(fmod(a, b)));
        } break;
        case OP_SUBTRACT: BINARY_OP(NUMBER_VAL, -); break;
        case OP_MULTIPLY: BINARY_OP(NUMBER_VAL, *); break;
        case OP_DIVIDE:   BINARY_OP(NUMBER_VAL, /); break;
        case OP_NOT:      vm.sp[-1] = BOOL_VAL(isFalsey(vm.sp[-1])); break;
        case OP_NEGATE:   {
            if (!IS_NUMBER(PEEK(0))) {
                runtimeError("Operand must be a number");
                return INTERPRET_RUNTIME_ERR;
            }
            vm.sp[-1] = NUMBER_VAL(-AS_NUMBER(vm.sp[-1]));
        } break;
        case OP_PRINT: {
#ifdef LOX_DEBUG
            printf("\033[1;33m");
#endif /* ifdef LOX_DEBUG */
            printValue(POP());
#ifdef LOX_DEBUG
            printf("\033[0m");
#endif /* ifdef LOX_DEBUG */
            printf("\n");
        } break;
        case OP_JUMP: {
            uint16_t offset = READ_SHORT();
            frame->ip += offset;
        } break;
        case OP_JUMP_IF_FALSE: {
            uint16_t offset = READ_SHORT();
            if (isFalsey(PEEK(0))) frame->ip += offset;
        } break;
        case OP_LOOP: {
            uint16_t offset = READ_SHORT();
            frame->ip -= offset;
        } break;
        case OP_CALL: {
            int argCnt = READ_BYTE();
            if (!callValue(PEEK(argCnt), argCnt)) return INTERPRET_RUNTIME_ERR;
            frame = &vm.frames[vm.frameCount - 1];
        } break;
        case OP_INVOKE: {
            Value method = READ_CONST();
            int argCnt = READ_BYTE();
            if (!invoke(method, argCnt)) return INTERPRET_RUNTIME_ERR;
            frame = &vm.frames[vm.frameCount - 1];
        } break;
        case OP_SUPER_INVOKE: {
            Value method = READ_CONST();
            int argCnt = READ_BYTE();
            ObjClass *superclass = AS_CLASS(POP());
            if (!invokeFromClass(superclass, method, argCnt)) {
                return INTERPRET_RUNTIME_ERR;
            }
            frame = &vm.frames[vm.frameCount - 1];
        } break;
        case OP_CLOSURE: {
            ObjFn *function = AS_FUNCTION(READ_CONST());
            ObjClosure *closure = newClosure(function);
            PUSH(OBJ_VAL(closure));
            for (int i = 0; i < closure->upvalueCnt; i++) {
                uint8_t isLocal = READ_BYTE();
                uint8_t index = READ_BYTE();
                if (isLocal) {
                    closure->upvalues[i] = captureUpvalue(frame->slots + index);
                } else {
                    closure->upvalues[i] = frame->closure->upvalues[index];
                }
            }
        } break;
        case OP_CLOSE_UPVALUE: {
            closeUpvalues(vm.sp - 1);
            (void)POP();
        } break;
        case OP_RETURN: {
            Value result = POP();
            closeUpvalues(frame->slots);
            vm.frameCount--;
            if (vm.frameCount == 0) {
                (void)POP();
                return INTERPRET_OK;
            }

            vm.sp = frame->slots;
            PUSH(result);
            frame = &vm.frames[vm.frameCount - 1];
        } break;
        case OP_BUILD_ARRAY: {
            int cnt = READ_BYTE();

            ObjArray *arr = newArray();
            pushRoot(OBJ_VAL(arr));
            for (Value *i = vm.sp - cnt; i < vm.sp; i++) {
                appendToArray(arr, *i);
            }
            popRoot();

            vm.sp -= cnt;
            PUSH(OBJ_VAL(arr));
        } break;
        case OP_BUILD_MAP: {
            int cnt = READ_BYTE() * 2;

            ObjMap *map = newMap();
            pushRoot(OBJ_VAL(map));
            for (Value *i = vm.sp - cnt; i < vm.sp; i += 2) {
                Value key = *i;
                if (!isHashable(key)) {
                    runtimeError("%s is an unhashable type", typeofValue(key));
                    return INTERPRET_RUNTIME_ERR;
                }
                Value val = *(i + 1);
                tableSet(&map->items, key, val);
            }
            popRoot();

            vm.sp -= cnt;
            PUSH(OBJ_VAL(map));
        } break;
        case OP_CLASS:   PUSH(OBJ_VAL(newClass(READ_STRING()))); break;
        case OP_INHERIT: {
            Value superclass = PEEK(1);
            if (!IS_CLASS(superclass)) {
                runtimeError("Superclass must be a class");
                return INTERPRET_RUNTIME_ERR;
            }

            ObjClass *subclass = AS_CLASS(PEEK(0));
            tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
            (void)POP(); // subclass
        } break;
        case OP_METHOD: defineMethod(READ_CONST()); break;
        }
    }

#undef PUSH
#undef POP
#undef PEEK
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONST
#undef READ_STRING
#undef BINARY_OP
#undef TRACE_EXECUTION

    return INTERPRET_OK;
}

InterpretResult interpret(const char *source) {
    ObjFn *function = compile(source);
    if (function == NULL) return INTERPRET_COMPILE_ERR;

    pushRoot(OBJ_VAL(function));
    ObjClosure *closure = newClosure(function);
    popRoot();
    push(OBJ_VAL(closure));
    call(closure, 0);

    return run();
}
