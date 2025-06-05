#include <stdlib.h>

#include "compiler.h"
#include "memory.h"
#include "vm.h"

#ifdef DEBUG_LOG_GC
#include "debug.h"
#include <stdio.h>
#endif

#define GC_HEAP_GROW_FACTOR 2

void *reallocate(void *ptr, size_t oldSize, size_t newSize) {
  vm.bytesAllocated += newSize - oldSize;
  if (newSize > oldSize) {
#ifdef DEBUG_STRESS_GC
    collectGarbage();
#endif

    if (vm.bytesAllocated > vm.nextGC) {
      collectGarbage();
    }
  }

  if (newSize == 0) {
    free(ptr);
    return NULL;
  }

  void *result = realloc(ptr, newSize);
  if (result == NULL) {
    exit(1);
  }
  return result;
}

void markObject(Obj *object) {
  if (object == NULL) {
    return;
  }
  if (object->isMarked) {
    return;
  }

#ifdef DEBUG_LOG_GC
  printf("%p mark ", (void *)object);
  printValue(OBJ_VAL(object));
  printf("\n");
#endif // ifdef DEBUG_LOG_GC

  object->isMarked = true;

  if (vm.grayCap < vm.grayCnt + 1) {
    vm.grayCap = GROW_CAP(vm.grayCap);
    vm.grayStack = (Obj **)realloc(vm.grayStack, sizeof(Obj *) * vm.grayCap);

    if (vm.grayStack == NULL) {
      exit(1);
    }
  }

  vm.grayStack[vm.grayCnt++] = object;
}

void markValue(Value value) {
  if (IS_OBJ(value)) {
    markObject(AS_OBJ(value));
  }
}

static inline void markArray(ValueArray *array) {
  for (int i = 0; i < array->cnt; i++) {
    markValue(array->values[i]);
  }
}

static void blackenObject(Obj *object) {
#ifdef DEBUG_LOG_GC
  printf("%p blacken ", (void *)object);
  printValue(OBJ_VAL(object));
  printf("\n");
#endif // ifdef DEBUG_LOG_GC

  switch (object->type) {
  case OBJ_BOUND_METHOD: {
    ObjBoundMethod *bound = (ObjBoundMethod *)object;
    markValue(bound->receiver);
    markObject((Obj *)bound->method);
    break;
  }
  case OBJ_CLASS: {
    ObjClass *klass = (ObjClass *)object;
    markObject((Obj *)klass->name);
    markTable(&klass->methods);
    break;
  }
  case OBJ_CLOSURE: {
    ObjClosure *closure = (ObjClosure *)object;
    markObject((Obj *)closure->fn);
    for (int i = 0; i < closure->upvalueCnt; i++) {
      markObject((Obj *)closure->upvalues[i]);
    }
    break;
  }
  case OBJ_FUNCTION: {
    ObjFn *function = (ObjFn *)object;
    markObject((Obj *)function->name);
    markArray(&function->chunk.constants);
    break;
  }
  case OBJ_INSTANCE: {
    ObjInstance *instance = (ObjInstance *)object;
    markObject((Obj *)instance->klass);
    markTable(&instance->fields);
    break;
  }
  case OBJ_UPVALUE: {
    markValue(((ObjUpvalue *)object)->closed);
    break;
  }
  case OBJ_NATIVE:
  case OBJ_STRING:
    break;
  }
}

static void freeObject(Obj *object) {
#ifdef DEBUG_LOG_GC
#define NEED_TO_STRING
  printf("%p free type %s\n", (void *)object, ObjToStrings[object->type]);
#undef NEED_TO_STRING
#endif // ifdef DEBUG_LOG_GC

  switch (object->type) {
  case OBJ_BOUND_METHOD:
    FREE(ObjBoundMethod, object);
    break;
  case OBJ_CLASS: {
    ObjClass *klass = (ObjClass *)object;
    freeTable(&klass->methods);
    FREE(ObjClass, object);
    break;
  }
  case OBJ_CLOSURE: {
    ObjClosure *closure = (ObjClosure *)object;
    FREE_ARRAY(ObjUpvalue *, closure->upvalues, closure->upvalueCnt);
    FREE(ObjClosure, object);
    break;
  }
  case OBJ_FUNCTION: {
    ObjFn *function = (ObjFn *)object;
    freeChunk(&function->chunk);
    FREE(ObjFn, object);
    break;
  }
  case OBJ_INSTANCE: {
    ObjInstance *instance = (ObjInstance *)object;
    freeTable(&instance->fields);
    FREE(ObjInstance, object);
    break;
  }
  case OBJ_NATIVE:
    FREE(ObjNative, object);
    break;
  case OBJ_STRING: {
    ObjString *string = (ObjString *)object;
    FREE_ARRAY(char, string->chars, string->length + 1);
    FREE(ObjString, object);
    break;
  }
  case OBJ_UPVALUE:
    FREE(ObjUpvalue, object);
    break;
  }
}

static void markRoots(void) {
#ifdef DEBUG_LOG_GC
  printf("-- begin mark roots\nmarking stack\n");
#endif // ifdef DEBUG_LOG_GC

  for (Value *slot = vm.stack; slot < vm.sp; slot++) {
    markValue(*slot);
  }

#ifdef DEBUG_LOG_GC
  printf("marking callframes\n");
#endif // ifdef DEBUG_LOG_GC

  for (int i = 0; i < vm.frameCount; i++) {
    markObject((Obj *)vm.frames[i].closure);
  }

#ifdef DEBUG_LOG_GC
  printf("marking upvalues\n");
#endif // ifdef DEBUG_LOG_GC

  for (ObjUpvalue *upvalue = vm.openUpvalues; upvalue != NULL;
       upvalue = upvalue->next) {
    markObject((Obj *)upvalue);
  }

#ifdef DEBUG_LOG_GC
  printf("marking globals\n");
#endif // ifdef DEBUG_LOG_GC
  markTable(&vm.globals);

#ifdef DEBUG_LOG_GC
  printf("marking compiler roots\n");
#endif // ifdef DEBUG_LOG_GC
  markCompilerRoots();

#ifdef DEBUG_LOG_GC
  printf("marking init string\n");
#endif // ifdef DEBUG_LOG_GC
  markObject((Obj *)vm.initString);

#ifdef DEBUG_LOG_GC
  printf("-- end mark roots\n");
#endif // ifdef DEBUG_LOG_GC
}

static inline void traceReferences(void) {
  while (vm.grayCnt > 0) {
    Obj *object = vm.grayStack[--vm.grayCnt];
    blackenObject(object);
  }
}

static void sweep(void) {
  Obj *prv = NULL;
  Obj *object = vm.objects;
  while (object != NULL) {
    if (object->isMarked) {
      object->isMarked = false;
      prv = object;
      object = object->next;
    } else {
      Obj *unreached = object;
      object = object->next;
      if (prv != NULL) {
        prv->next = object;
      } else {
        vm.objects = object;
      }
      freeObject(unreached);
    }
  }
}

void collectGarbage(void) {
#ifdef DEBUG_LOG_GC
  printf("-- gc begin\n");
  size_t before = vm.bytesAllocated;
#endif // ifdef DEBUG_LOG_GC

  markRoots();
  traceReferences();
  tableRemoveWhite(&vm.strings);
  sweep();

  vm.nextGC = vm.bytesAllocated * GC_HEAP_GROW_FACTOR;

#ifdef DEBUG_LOG_GC
  printf("-- gc end\n");
  printf("   collected %zu bytes (from %zu to %zu) next at %zu\n",
         before - vm.bytesAllocated, before, vm.bytesAllocated, vm.nextGC);
#endif // ifdef DEBUG_LOG_GC
}

void freeObjects(void) {
  Obj *object = vm.objects;
  while (object != NULL) {
    Obj *next = object->next;
    freeObject(object);
    object = next;
  }

  free(vm.grayStack);
}
