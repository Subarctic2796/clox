#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "compiler.h"

#ifdef DEBUG_TRACE_EXECUTION
#include "debug.h"
#endif

#include "memory.h"
#include "object.h"
#include "vm.h"

VM vm;

static Value clockNative(int argc, Value *args) {
  (void)argc;
  (void)args;
  return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static void resetStack(void) {
  vm.sp = vm.stack;
  vm.frameCount = 0;
  vm.openUpvalues = NULL;
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
  int len = (int)strlen(name);
  push(OBJ_VAL(copyString(name, len)));
  push(OBJ_VAL(newNative(function)));
  tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
  pop(); // pop native name, on stack bc of GC
  pop(); // pop native ptr, on stack bc of GC
}

void initVM(void) {
  resetStack();
  vm.objects = NULL;
  vm.bytesAllocated = 0;
  vm.nextGC = 1024 * 1024; // 1mib

  vm.grayCnt = 0;
  vm.grayCap = 0;
  vm.grayStack = NULL;

  initTable(&vm.globals);

  initTable(&vm.strings);

  vm.initString = NULL;
  vm.initString = copyString("init", 4);

  defineNative("clock", clockNative);
}

void freeVM(void) {
  freeTable(&vm.globals);
  freeTable(&vm.strings);
  vm.initString = NULL;
  freeObjects();
}

void push(Value value) { *vm.sp++ = value; }
Value pop(void) { return *(--vm.sp); }
static inline Value peek(int dist) { return vm.sp[-1 - dist]; }

static bool call(ObjClosure *closure, int argc) {
  if (argc != closure->fn->arity) {
    runtimeError("Expected %d arguments but got %d", closure->fn->arity, argc);
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
      if (tableGet(&klass->methods, vm.initString, &init)) {
        return call(AS_CLOSURE(init), argCnt);
      } else if (argCnt != 0) {
        runtimeError("Expected 0 arguments but got %d", argCnt);
        return false;
      }
      return true;
    }
    case OBJ_CLOSURE:
      return call(AS_CLOSURE(callee), argCnt);
    case OBJ_NATIVE: {
      NativeFn native = AS_NATIVE(callee);
      Value result = native(argCnt, vm.sp - argCnt);
      vm.sp -= argCnt + 1;
      push(result);
      return true;
    }
    default:
      break; // non-callable object type
    }
  }
  runtimeError("Can only call functions and classes");
  return false;
}

static inline bool invokeFromClass(ObjClass *klass, ObjString *name, int argc) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'", name->chars);
    return false;
  }
  return call(AS_CLOSURE(method), argc);
}

static bool invoke(ObjString *name, int argCnt) {
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

static bool bindMethod(ObjClass *klass, ObjString *name) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'", name->chars);
    return false;
  }

  ObjBoundMethod *bound = newBoundMethod(peek(0), AS_CLOSURE(method));
  pop();
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

  if (upvalue != NULL && upvalue->location == local) {
    return upvalue;
  }

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

static void defineMethod(ObjString *name) {
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
  pop();
  pop();
  push(OBJ_VAL(result));
}

static InterpretResult run(void) {
  CallFrame *frame = &vm.frames[vm.frameCount - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT()                                                           \
  (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONST() (frame->closure->fn->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONST())
#define BINARY_OP(valueType, op)                                               \
  do {                                                                         \
    if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {                          \
      runtimeError("Operands must be numbers");                                \
      return INTERPRET_RUNTIME_ERR;                                            \
    }                                                                          \
    double b = AS_NUMBER(pop());                                               \
    double a = AS_NUMBER(pop());                                               \
    push(valueType(a op b));                                                   \
  } while (false)

  for (;;) {
    uint8_t inst;
// loop:
#ifdef DEBUG_TRACE_EXECUTION
    printf("          ");
    for (Value *slot = vm.stack; slot < vm.sp; slot++) {
      printf("[ ");
      printValue(*slot);
      printf(" ]");
    }
    printf("\n");
    disassembleInst(&frame->closure->fn->chunk,
                    (int)(frame->ip - frame->closure->fn->chunk.code));
#endif
    switch (inst = READ_BYTE()) {
    case OP_CONSTANT: {
      Value constant = READ_CONST();
      push(constant);
      break;
      // goto loop;
    }
    case OP_NIL:
      push(NIL_VAL);
      break;
      // goto loop;
    case OP_TRUE:
      push(BOOL_VAL(true));
      break;
      // goto loop;
    case OP_FALSE:
      push(BOOL_VAL(false));
      break;
      // goto loop;
    case OP_POP:
      pop();
      break;
      // goto loop;
    case OP_SET_LOCAL: {
      uint8_t slot = READ_BYTE();
      frame->slots[slot] = peek(0);
      break;
      // goto loop;
    }
    case OP_GET_LOCAL: {
      uint8_t slot = READ_BYTE();
      push(frame->slots[slot]);
      break;
      // goto loop;
    }
    case OP_GET_GLOBAL: {
      ObjString *name = READ_STRING();
      Value value;
      if (!tableGet(&vm.globals, name, &value)) {
        runtimeError("Undefined variable '%s'", name->chars);
        return INTERPRET_RUNTIME_ERR;
      }
      push(value);
      break;
      // goto loop;
    }
    case OP_DEFINE_GLOBAL: {
      ObjString *name = READ_STRING();
      tableSet(&vm.globals, name, peek(0));
      pop();
      break;
      // goto loop;
    }
    case OP_SET_GLOBAL: {
      ObjString *name = READ_STRING();
      if (tableSet(&vm.globals, name, peek(0))) {
        tableDelete(&vm.globals, name);
        runtimeError("Undefined variable '%s'", name->chars);
        return INTERPRET_RUNTIME_ERR;
      }
      break;
      // goto loop;
    }
    case OP_GET_UPVALUE: {
      uint8_t slot = READ_BYTE();
      push(*frame->closure->upvalues[slot]->location);
      break;
      // goto loop;
    }
    case OP_SET_UPVALUE: {
      uint8_t slot = READ_BYTE();
      *frame->closure->upvalues[slot]->location = peek(0);
      break;
      // goto loop;
    }
    case OP_GET_PROPERTY: {
      if (!IS_INSTANCE(peek(0))) {
        runtimeError("Only instances have properties");
        return INTERPRET_RUNTIME_ERR;
      }

      ObjInstance *instance = AS_INSTANCE(peek(0));
      ObjString *name = READ_STRING();
      Value value;
      if (tableGet(&instance->fields, name, &value)) {
        pop(); // instance
        push(value);
        break;
        // goto loop;
      }

      if (!bindMethod(instance->klass, name)) {
        return INTERPRET_RUNTIME_ERR;
      }
      break;
      // goto loop;
    }
    case OP_SET_PROPERTY: {
      if (!IS_INSTANCE(peek(1))) {
        runtimeError("Only instances have fields");
        return INTERPRET_RUNTIME_ERR;
      }
      ObjInstance *instance = AS_INSTANCE(peek(1));
      tableSet(&instance->fields, READ_STRING(), peek(0));
      Value value = pop();
      pop();
      push(value);
      break;
      // goto loop;
    }
    case OP_GET_SUPER: {
      ObjString *name = READ_STRING();
      ObjClass *superclass = AS_CLASS(pop());

      if (!bindMethod(superclass, name)) {
        return INTERPRET_RUNTIME_ERR;
      }
      break;
      // goto loop;
    }
    case OP_EQUAL: {
      Value b = pop();
      Value a = pop();
      push(BOOL_VAL(valuesEqual(a, b)));
      break;
      // goto loop;
    }
    case OP_GREATER:
      BINARY_OP(BOOL_VAL, >);
      break;
      // goto loop;
    case OP_LESS:
      BINARY_OP(BOOL_VAL, <);
      break;
      // goto loop;
    case OP_ADD: {
      if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
        concatenate();
      } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
        double b = AS_NUMBER(pop());
        double a = AS_NUMBER(pop());
        push(NUMBER_VAL(a + b));
      } else {
        runtimeError("Operands must be two numbers or two strings");
        return INTERPRET_RUNTIME_ERR;
      }
      break;
      // goto loop;
    }
    case OP_SUBTRACT:
      BINARY_OP(NUMBER_VAL, -);
      break;
      // goto loop;
    case OP_MULTIPLY:
      BINARY_OP(NUMBER_VAL, *);
      break;
      // goto loop;
    case OP_DIVIDE:
      BINARY_OP(NUMBER_VAL, /);
      break;
      // goto loop;
    case OP_NOT:
      push(BOOL_VAL(isFalsey(pop())));
      break;
      // goto loop;
    case OP_NEGATE: {
      if (!IS_NUMBER(peek(0))) {
        runtimeError("Operand must be a number");
        return INTERPRET_RUNTIME_ERR;
      }
      push(NUMBER_VAL(-AS_NUMBER(pop())));
      break;
      // goto loop;
    }
    case OP_PRINT: {
      printValue(pop());
      printf("\n");
      break;
      // goto loop;
    }
    case OP_JUMP: {
      uint16_t offset = READ_SHORT();
      frame->ip += offset;
      break;
      // goto loop;
    }
    case OP_JUMP_IF_FALSE: {
      uint16_t offset = READ_SHORT();
      if (isFalsey(peek(0))) {
        frame->ip += offset;
      }
      break;
      // goto loop;
    }
    case OP_LOOP: {
      uint16_t offset = READ_SHORT();
      frame->ip -= offset;
      break;
      // goto loop;
    }
    case OP_CALL: {
      int argCnt = READ_BYTE();
      if (!callValue(peek(argCnt), argCnt)) {
        return INTERPRET_RUNTIME_ERR;
      }
      frame = &vm.frames[vm.frameCount - 1];
      break;
      // goto loop;
    }
    case OP_INVOKE: {
      ObjString *method = READ_STRING();
      int argCnt = READ_BYTE();
      if (!invoke(method, argCnt)) {
        return INTERPRET_RUNTIME_ERR;
      }
      frame = &vm.frames[vm.frameCount - 1];
      break;
      // goto loop;
    }
    case OP_SUPER_INVOKE: {
      ObjString *method = READ_STRING();
      int argCnt = READ_BYTE();
      ObjClass *superclass = AS_CLASS(pop());
      if (!invokeFromClass(superclass, method, argCnt)) {
        return INTERPRET_RUNTIME_ERR;
      }
      frame = &vm.frames[vm.frameCount - 1];
      break;
      // goto loop;
    }
    case OP_CLOSURE: {
      ObjFn *function = AS_FUNCTION(READ_CONST());
      ObjClosure *closure = newClosure(function);
      push(OBJ_VAL(closure));
      for (int i = 0; i < closure->upvalueCnt; i++) {
        uint8_t isLocal = READ_BYTE();
        uint8_t index = READ_BYTE();
        if (isLocal) {
          closure->upvalues[i] = captureUpvalue(frame->slots + index);
        } else {
          closure->upvalues[i] = frame->closure->upvalues[index];
        }
      }
      break;
      // goto loop;
    }
    case OP_CLOSE_UPVALUE: {
      closeUpvalues(vm.sp - 1);
      pop();
      break;
      // goto loop;
    }
    case OP_RETURN: {
      Value result = pop();
      closeUpvalues(frame->slots);
      vm.frameCount--;
      if (vm.frameCount == 0) {
        pop();
        return INTERPRET_OK;
      }

      vm.sp = frame->slots;
      push(result);
      frame = &vm.frames[vm.frameCount - 1];
      break;
      // goto loop;
    }
    case OP_CLASS:
      push(OBJ_VAL(newClass(READ_STRING())));
      break;
      // goto loop;
    case OP_INHERIT: {
      Value superclass = peek(1);
      if (!IS_CLASS(superclass)) {
        runtimeError("Superclass must be a class");
        return INTERPRET_RUNTIME_ERR;
      }

      ObjClass *subclass = AS_CLASS(peek(0));
      tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
      pop(); // subclass
      break;
      // goto loop;
    }
    case OP_METHOD:
      defineMethod(READ_STRING());
      break;
      // goto loop;
    }
  } // loop end

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONST
#undef READ_STRING
#undef BINARY_OP

  return INTERPRET_OK;
}

InterpretResult interpret(const char *source) {
  ObjFn *function = compile(source);
  if (function == NULL) {
    return INTERPRET_COMPILE_ERR;
  }

  push(OBJ_VAL(function));
  ObjClosure *closure = newClosure(function);
  pop();
  push(OBJ_VAL(closure));
  call(closure, 0);

  return run();
}
