#ifndef INCLUDE_CLOX_COMPILER_H_
#define INCLUDE_CLOX_COMPILER_H_

#include "object.h"
#include "vm.h"

ObjFn *compile(VM *vm, const char *source);
void markCompilerRoots(VM *vm);

#endif // INCLUDE_CLOX_COMPILER_H_
