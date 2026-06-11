#include <stdarg.h>
#include <stdlib.h>

#include "chunk.h"
#include "common.h"
#include "compiler.h"
#include "memory.h"
#include "natives.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "vm.h"

#ifdef DEBUG_TRACE_EXECUTION
#include "debug.h"
#endif

VM vm = {0};

static inline void resetStack(VM *vm) {
    vm->sp = vm->stack;
    vm->frameCount = 0;
    vm->openUpvalues = NULL;
    vm->tempCnt = 0;
}

static void runtimeError(VM *vm, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = vm->frameCount - 1; i >= 0; i--) {
        CallFrame *frame = &vm->frames[i];
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

    resetStack(vm);
}

void initVM(VM *vm) {
    // zero the VM
    *vm = (VM){0};
    resetStack(vm);

    // set up VM state that should not be a zero value
    vm->nextGC = 1024 * 1024; // 1mib

    initTable(&vm->globalNames);
    initValueArray(&vm->globalValues);
    initTable(&vm->strings);

    vm->initString = CONST_STRING("init");

    defineAllNatives(vm);
}

void freeVM(VM *vm) {
    freeTable(vm, &vm->globalNames);
    freeValueArray(vm, &vm->globalValues);
    freeTable(vm, &vm->strings);
    vm->initString = NULL;
    freeObjects(vm);
}

static const char *findGlobalNameFromIndex(const VM *vm, int index) {
    for (int i = 0; i < vm->globalNames.cap; i++) {
        Entry entry = vm->globalNames.entries[i];
        if (IS_EMPTY(entry.key)) continue;

        if (index == (int)AS_NUMBER(entry.value)) return AS_CSTRING(entry.key);
    }
    return NULL;
}

static bool call(VM *vm, ObjClosure *closure, int argc) {
    if (argc != closure->fn->arity) {
        runtimeError(vm, "Expected %d arguments but got %d", closure->fn->arity,
                     argc);
        return false;
    }

    if (vm->frameCount == FRAMES_MAX) {
        runtimeError(vm, "Stack overflow");
        return false;
    }

    // updates the VM's frame ip
    CallFrame *frame = &vm->frames[vm->frameCount++];
    frame->closure = closure;
    frame->ip = closure->fn->chunk.code;
    frame->slots = vm->sp - argc - 1;
    return true;
}

static bool callValue(VM *vm, Value callee, int argCnt) {
    if (IS_OBJ(callee)) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
        switch (OBJ_TYPE(callee)) {
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
            vm->sp[-argCnt - 1] = bound->receiver;
            return call(vm, bound->method, argCnt);
        }
        case OBJ_CLASS: {
            ObjClass *klass = AS_CLASS(callee);
            vm->sp[-argCnt - 1] = OBJ_VAL(newInstance(vm, klass));
            Value init = EMPTY_VAL;
            if (tableGet(&klass->methods, OBJ_VAL(vm->initString), &init)) {
                if (IS_CLOSURE(init)) return call(vm, AS_CLOSURE(init), argCnt);

                NativeFn native = AS_NATIVE(init);
                Value result = native(vm, argCnt, vm->sp - argCnt);
                if (IS_ERROR(result) && !AS_ERROR(result)->recoverable) {
                    runtimeError(vm, AS_ERROR_MSG(result));
                    return false;
                }
                vm->sp -= argCnt + 1;
                push(vm, result);
                return true;
            } else if (argCnt != 0) {
                runtimeError(vm, "Expected 0 arguments but got %d", argCnt);
                return false;
            }
            return true;
        }
        case OBJ_CLOSURE: return call(vm, AS_CLOSURE(callee), argCnt);
        case OBJ_NATIVE:  {
            NativeFn native = AS_NATIVE(callee);
            Value result = native(vm, argCnt, vm->sp - argCnt);
            if (IS_ERROR(result) && !AS_ERROR(result)->recoverable) {
                runtimeError(vm, AS_ERROR_MSG(result));
                return false;
            }
            vm->sp -= argCnt + 1;
            push(vm, result);
            return true;
        }
        default: break; // non-callable object type
        }
#pragma GCC diagnostic pop
    }
    runtimeError(vm, "Can only call functions and classes");
    return false;
}

static inline bool invokeFromClass(VM *vm, ObjClass *klass, Value name,
                                   int argc) {
    Value method = EMPTY_VAL;
    if (!tableGet(&klass->methods, name, &method)) {
        runtimeError(vm, "Undefined property '%s'", AS_CSTRING(name));
        return false;
    }
    return callValue(vm, method, argc);
}

static bool invoke(VM *vm, Value name, int argCnt) {
    Value receiver = peek(vm, argCnt);

    if (!IS_INSTANCE(receiver)) {
        runtimeError(vm, "Only instances have methods");
        return false;
    }

    ObjInstance *instance = AS_INSTANCE(receiver);

    Value value = EMPTY_VAL;
    if (tableGet(&instance->fields, name, &value)) {
        vm->sp[-argCnt - 1] = value;
        return callValue(vm, value, argCnt);
    }

    return invokeFromClass(vm, instance->klass, name, argCnt);
}

static bool bindMethod(VM *vm, ObjClass *klass, Value name) {
    Value method = EMPTY_VAL;
    if (!tableGet(&klass->methods, name, &method)) {
        runtimeError(vm, "Undefined property '%s'", AS_CSTRING(name));
        return false;
    }

    ObjBoundMethod *bound = newBoundMethod(vm, peek(vm, 0), AS_CLOSURE(method));
    pop(vm); // pop 'this'
    push(vm, OBJ_VAL(bound));
    return true;
}

static ObjUpvalue *captureUpvalue(VM *vm, Value *local) {
    ObjUpvalue *prv = NULL;
    ObjUpvalue *upvalue = vm->openUpvalues;
    while (upvalue != NULL && upvalue->location > local) {
        prv = upvalue;
        upvalue = upvalue->next;
    }

    if (upvalue != NULL && upvalue->location == local) return upvalue;

    ObjUpvalue *createdUpvalue = newUpvalue(vm, local);
    createdUpvalue->next = upvalue;

    if (prv == NULL) {
        vm->openUpvalues = createdUpvalue;
    } else {
        prv->next = createdUpvalue;
    }

    return createdUpvalue;
}

static void closeUpvalues(VM *vm, Value *last) {
    while (vm->openUpvalues != NULL && vm->openUpvalues->location >= last) {
        ObjUpvalue *upvalue = vm->openUpvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm->openUpvalues = upvalue->next;
    }
}

static inline void defineMethod(VM *vm, Value name) {
    Value method = peek(vm, 0);
    ObjClass *klass = AS_CLASS(peek(vm, 1));
    tableSet(vm, &klass->methods, name, method);
    pop(vm);
}

static inline bool isFalsey(Value value) {
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static inline void concatenate(VM *vm) {
    ObjString *b = AS_STRING(peek(vm, 0));
    ObjString *a = AS_STRING(peek(vm, 1));

    int length = a->length + b->length;
    char *chars = ALLOCATE(char, length + 1);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    ObjString *result = takeString(vm, chars, length);
    vm->sp -= 2; // pop the 2 strings ontop of the stack
    push(vm, OBJ_VAL(result));
}

static inline bool doIndexedGet(VM *vm) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (OBJ_TYPE(peek(vm, 1))) {
    case OBJ_STRING: {
        Value index_ = peek(vm, 0);
        ObjString *str = AS_STRING(peek(vm, 1));

        int index = isValidIndex(index_, str->length);
        if (index == -1) {
            runtimeError(vm, "Can only use numbers to index strings");
            return false;
        } else if (index == -2) {
            runtimeError(vm, "can only use integers to index into strings");
            return false;
        } else if (index == -3) {
            runtimeError(vm, "index out of bounds");
            return false;
        } else if (index == -4) {
            ObjRange *range = AS_RANGE(index_);
            int stop =
                (int)range->stop < str->length ? range->stop : str->length;

            int len = (stop - range->start) / range->step;
            char *buf = ALLOCATE(char, len + 1);
            int n = 0;
            for (int i = range->start; i < stop; i += range->step) {
                buf[n++] = str->chars[i];
            }
            buf[len] = '\0';

            ObjString *result = takeString(vm, buf, len);
            vm->sp -= 2; // pop index and string
            push(vm, OBJ_VAL(result));
            return true;
        }

        ObjString *result = copyString(vm, str->chars + index, 1);
        vm->sp -= 2; // pop index and string
        push(vm, OBJ_VAL(result));
        return true;
    }
    case OBJ_ARRAY: {
        Value index_ = peek(vm, 0);
        ObjArray *arr = AS_ARRAY(peek(vm, 1));

        int index = isValidIndex(index_, arr->items.cnt);
        if (index == -1) {
            runtimeError(vm, "Can only use numbers to index arrays");
            return false;
        } else if (index == -2) {
            runtimeError(vm, "can only use integers to index into arrays");
            return false;
        } else if (index == -3) {
            runtimeError(vm, "index out of bounds");
            return false;
        } else if (index == -4) {
            ObjRange *range = AS_RANGE(index_);
            int stop = (int)range->stop < arr->items.cnt ? range->stop
                                                         : arr->items.cnt;
            ObjArray *subarr = newArray(vm);
            pushRoot(vm, OBJ_VAL(subarr));
            for (int i = range->start; i < stop; i += range->step) {
                appendToArray(vm, subarr, indexFromArray(arr, i));
            }
            popRoot(vm);

            vm->sp -= 2; // pop index and array
            push(vm, indexFromArray(arr, index));
            return true;
        }

        vm->sp -= 2; // pop index and array
        push(vm, indexFromArray(arr, index));
        return true;
    }
    case OBJ_MAP: {
        Value key = pop(vm);
        if (!isHashable(key)) {
            runtimeError(vm, "%s is an unhashable type", typeofValue(key));
            return false;
        }

        ObjMap *map = AS_MAP(pop(vm));
        Value result = NIL_VAL;
        tableGet(&map->items, key, &result);
        push(vm, result);
        return true;
    }
    case OBJ_RANGE: {
        Value index_ = pop(vm);
        ObjRange *range = AS_RANGE(pop(vm));

        int index = isValidIndex(
            index_, floor((range->stop - range->start) / range->step) + 1);
        if (index == -1) {
            runtimeError(vm, "Can only use numbers to index ranges");
            return false;
        } else if (index == -2) {
            runtimeError(vm, "can only use integers to index into ranges");
            return false;
        } else if (index == -3) {
            runtimeError(vm, "index out of range");
            return false;
        } else if (index == -4) {
            printf("TODO: allow indexed getting using ranges for ranges\n");
            printf("TODO: taking a range of a range in python returns a new "
                   "range\n");
            abort();
        }

        push(vm, NUMBER_VAL(index * range->step + range->start));
        return true;
    }
    default: UNREACHABLE(); return false;
    }
#pragma GCC diagnostic pop
}

static inline bool doIndexedSet(VM *vm) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (OBJ_TYPE(peek(vm, 2))) {
    case OBJ_STRING:
        runtimeError(vm, "Can not use index setting on strings");
        return false;
    case OBJ_RANGE:
        runtimeError(vm, "Can not use index setting on ranges");
        return false;
    case OBJ_ARRAY: {
        Value value = pop(vm);
        Value index_ = pop(vm);
        ObjArray *arr = AS_ARRAY(pop(vm));

        int index = isValidIndex(index_, arr->items.cnt);
        if (index == -1 || index == -4) {
            runtimeError(vm, "Can only use numbers to index arrays");
            return false;
        } else if (index == -2) {
            runtimeError(vm, "can only use integers to index into arrays");
            return false;
        } else if (index == -3) {
            runtimeError(vm, "index out of bounds");
            return false;
        }

        storeToArray(arr, index, value);
        push(vm, value);
        return true;
    }
    case OBJ_MAP: {
        Value value = peek(vm, 0);
        Value key = peek(vm, 1);
        if (!isHashable(key)) {
            runtimeError(vm, "%s is an unhashable type", typeofValue(key));
            return false;
        }

        ObjMap *map = AS_MAP(peek(vm, 2));
        tableSet(vm, &map->items, key, value);
        vm->sp -= 3;
        push(vm, value);
        return true;
    }
    default: UNREACHABLE(); return false;
    }
#pragma GCC diagnostic pop
}

static InterpretResult run(VM *vm) {
    CallFrame *frame = &vm->frames[vm->frameCount - 1];

#define PUSH(value) (*vm->sp++ = value)
#define POP()       (*(--vm->sp))
#define PEEK(dist)  (*(vm->sp - 1 - dist))
#define READ_BYTE() (*frame->ip++)
#define READ_SHORT()                                                           \
    (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONST()  (frame->closure->fn->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONST())
#define BINARY_OP(valueType, op)                                               \
    do {                                                                       \
        if (!IS_NUMBER(PEEK(0)) || !IS_NUMBER(PEEK(1))) {                      \
            runtimeError(vm, "Operands must be numbers");                      \
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
        for (Value *slot = vm->stack; slot < vm->sp; slot++) {                 \
            printf("[ ");                                                      \
            printValue(*slot);                                                 \
            printf(" ]");                                                      \
        }                                                                      \
        printf("\n");                                                          \
        disassembleInst(&frame->closure->fn->chunk,                            \
                        (int)(frame->ip - frame->closure->fn->chunk.code));    \
    } while (false)
#else
#define TRACE_EXECUTION()
#endif

    OpCode inst = OP_NOP;
    for (;;) {
        TRACE_EXECUTION();
        switch (inst = (OpCode)READ_BYTE()) {
        case OP_CONSTANT:  PUSH(READ_CONST()); break;
        case OP_SMALL_INT: PUSH(NUMBER_VAL(READ_BYTE())); break;
        case OP_NIL:       PUSH(NIL_VAL); break;
        case OP_TRUE:      PUSH(BOOL_VAL(true)); break;
        case OP_FALSE:     PUSH(BOOL_VAL(false)); break;
        case OP_POP:       (void)POP(); break;
        case OP_GET_INDEX: {
            if (!isIndexable(PEEK(1))) {
                runtimeError(vm, "%s is not an indexable type",
                             typeofValue(PEEK(1)));
                return INTERPRET_RUNTIME_ERR;
            }
            if (!doIndexedGet(vm)) return INTERPRET_RUNTIME_ERR;
        } break;
        case OP_SET_INDEX: {
            if (!isIndexable(PEEK(2))) {
                runtimeError(vm, "%s is not an indexable type",
                             typeofValue(PEEK(2)));
                return INTERPRET_RUNTIME_ERR;
            }
            if (!doIndexedSet(vm)) return INTERPRET_RUNTIME_ERR;
        } break;
        case OP_GET_LOCAL:  PUSH(frame->slots[READ_BYTE()]); break;
        case OP_SET_LOCAL:  frame->slots[READ_BYTE()] = PEEK(0); break;
        case OP_GET_GLOBAL: {
            int index = READ_BYTE();
            Value value = vm->globalValues.values[index];
            if (IS_EMPTY(value)) {
                const char *name = findGlobalNameFromIndex(vm, index);
                runtimeError(vm, "Undefined variable '%s'", name);
                return INTERPRET_RUNTIME_ERR;
            }
            PUSH(value);
        } break;
        case OP_DEFINE_GLOBAL: {
            vm->globalValues.values[READ_BYTE()] = POP();
        } break;
        case OP_SET_GLOBAL: {
            int index = READ_BYTE();
            if (IS_EMPTY(vm->globalValues.values[index])) {
                const char *name = findGlobalNameFromIndex(vm, index);
                runtimeError(vm, "Undefined variable '%s'", name);
                return INTERPRET_RUNTIME_ERR;
            }
            vm->globalValues.values[index] = PEEK(0);
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
                runtimeError(vm, "Only instances have properties");
                return INTERPRET_RUNTIME_ERR;
            }

            ObjInstance *instance = AS_INSTANCE(PEEK(0));
            Value name = READ_CONST();
            Value value = EMPTY_VAL;
            if (tableGet(&instance->fields, name, &value)) {
                (void)POP(); // instance
                PUSH(value);
                break;
            }

            if (!bindMethod(vm, instance->klass, name)) {
                return INTERPRET_RUNTIME_ERR;
            }
        } break;
        case OP_SET_PROPERTY: {
            if (!IS_INSTANCE(PEEK(1))) {
                runtimeError(vm, "Only instances have fields");
                return INTERPRET_RUNTIME_ERR;
            }
            ObjInstance *instance = AS_INSTANCE(PEEK(1));
            tableSet(vm, &instance->fields, READ_CONST(), PEEK(0));
            Value value = POP();
            (void)POP(); // instance
            PUSH(value);
        } break;
        case OP_GET_SUPER: {
            Value name = READ_CONST();
            ObjClass *superclass = AS_CLASS(POP());

            if (!bindMethod(vm, superclass, name)) return INTERPRET_RUNTIME_ERR;
        } break;
        case OP_EQUAL: {
            Value b = POP();
            Value a = POP();
            PUSH(BOOL_VAL(valuesEqual(a, b)));
        } break;
        case OP_NOT_EQUAL: {
            Value b = POP();
            Value a = POP();
            PUSH(BOOL_VAL(!valuesEqual(a, b)));
        } break;
        case OP_GREATER:       BINARY_OP(BOOL_VAL, >); break;
        case OP_GREATER_EQUAL: BINARY_OP(BOOL_VAL, >=); break;
        case OP_LESS:          BINARY_OP(BOOL_VAL, <); break;
        case OP_LESS_EQUAL:    BINARY_OP(BOOL_VAL, <=); break;
        case OP_SUBTRACT:      BINARY_OP(NUMBER_VAL, -); break;
        case OP_MULTIPLY:      BINARY_OP(NUMBER_VAL, *); break;
        case OP_DIVIDE:        BINARY_OP(NUMBER_VAL, /); break;
        case OP_ADD:           {
            if (IS_STRING(PEEK(0)) && IS_STRING(PEEK(1))) {
                concatenate(vm);
            } else if (IS_NUMBER(PEEK(0)) && IS_NUMBER(PEEK(1))) {
                double b = AS_NUMBER(POP());
                double a = AS_NUMBER(POP());
                PUSH(NUMBER_VAL(a + b));
            } else {
                runtimeError(vm, "Operands must be two numbers or two strings");
                return INTERPRET_RUNTIME_ERR;
            }
        } break;
        case OP_MOD: {
            if (!IS_NUMBER(PEEK(0)) || !IS_NUMBER(PEEK(1))) {
                runtimeError(vm, "Operands must be numbers");
                return INTERPRET_RUNTIME_ERR;
            }
            double b = AS_NUMBER(POP());
            double a = AS_NUMBER(POP());
            PUSH(NUMBER_VAL(fmod(a, b)));
        } break;
        case OP_NOT:    vm->sp[-1] = BOOL_VAL(isFalsey(vm->sp[-1])); break;
        case OP_NEGATE: {
            if (!IS_NUMBER(PEEK(0))) {
                runtimeError(vm, "Operand must be a number");
                return INTERPRET_RUNTIME_ERR;
            }
            vm->sp[-1] = NUMBER_VAL(-AS_NUMBER(vm->sp[-1]));
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
            if (!callValue(vm, PEEK(argCnt), argCnt))
                return INTERPRET_RUNTIME_ERR;
            frame = &vm->frames[vm->frameCount - 1];
        } break;
        case OP_INVOKE: {
            Value method = READ_CONST();
            int argCnt = READ_BYTE();
            if (!invoke(vm, method, argCnt)) return INTERPRET_RUNTIME_ERR;
            frame = &vm->frames[vm->frameCount - 1];
        } break;
        case OP_SUPER_INVOKE: {
            Value method = READ_CONST();
            int argCnt = READ_BYTE();
            ObjClass *superclass = AS_CLASS(POP());
            if (!invokeFromClass(vm, superclass, method, argCnt)) {
                return INTERPRET_RUNTIME_ERR;
            }
            frame = &vm->frames[vm->frameCount - 1];
        } break;
        case OP_CLOSURE: {
            ObjFn *function = AS_FUNCTION(READ_CONST());
            ObjClosure *closure = newClosure(vm, function);
            PUSH(OBJ_VAL(closure));
            for (int i = 0; i < closure->upvalueCnt; i++) {
                uint8_t isLocal = READ_BYTE();
                uint8_t index = READ_BYTE();
                if (isLocal) {
                    closure->upvalues[i] =
                        captureUpvalue(vm, frame->slots + index);
                } else {
                    closure->upvalues[i] = frame->closure->upvalues[index];
                }
            }
        } break;
        case OP_CLOSE_UPVALUE: {
            closeUpvalues(vm, vm->sp - 1);
            (void)POP();
        } break;
        case OP_RETURN: {
            Value result = POP();
            closeUpvalues(vm, frame->slots);
            vm->frameCount--;
            if (vm->frameCount == 0) {
                (void)POP();
                return INTERPRET_OK;
            }

            vm->sp = frame->slots;
            PUSH(result);
            frame = &vm->frames[vm->frameCount - 1];
        } break;
        case OP_BUILD_ARRAY: {
            int cnt = READ_BYTE();

            ObjArray *arr = newArray(vm);
            pushRoot(vm, OBJ_VAL(arr));
            for (int i = cnt - 1; i >= 0; i--) {
                appendToArray(vm, arr, PEEK(i));
            }
            popRoot(vm);

            vm->sp -= cnt;
            PUSH(OBJ_VAL(arr));
        } break;
        case OP_BUILD_MAP: {
            int cnt = READ_BYTE() * 2;

            ObjMap *map = newMap(vm);
            pushRoot(vm, OBJ_VAL(map));
            for (int i = cnt - 1; i >= 0; i -= 2) {
                Value key = PEEK(i);
                if (!isHashable(key)) {
                    runtimeError(vm, "%s is an unhashable type",
                                 typeofValue(key));
                    return INTERPRET_RUNTIME_ERR;
                }
                Value val = PEEK(i + 1);
                tableSet(vm, &map->items, key, val);
            }
            popRoot(vm);

            vm->sp -= cnt;
            PUSH(OBJ_VAL(map));
        } break;
        case OP_CLASS:   PUSH(OBJ_VAL(newClass(vm, READ_STRING()))); break;
        case OP_INHERIT: {
            Value superclass = PEEK(1);
            if (!IS_CLASS(superclass)) {
                runtimeError(vm, "Superclass must be a class");
                return INTERPRET_RUNTIME_ERR;
            }

            ObjClass *subclass = AS_CLASS(PEEK(0));
            tableAddAll(vm, &AS_CLASS(superclass)->methods, &subclass->methods);
            (void)POP(); // subclass
        } break;
        case OP_METHOD: defineMethod(vm, READ_CONST()); break;
        case OP_NOP:    UNREACHABLE(); break;
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

InterpretResult interpret(VM *vm, const char *source) {
    ObjFn *function = compile(source);
    if (function == NULL) return INTERPRET_COMPILE_ERR;

    pushRoot(vm, OBJ_VAL(function));
    ObjClosure *closure = newClosure(vm, function);
    popRoot(vm);
    push(vm, OBJ_VAL(closure));
    call(vm, closure, 0);

    return run(vm);
}
