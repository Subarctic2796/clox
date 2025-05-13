#ifndef INCLUDE_CLOX_COMPILER_H_
#define INCLUDE_CLOX_COMPILER_H_

#include "object.h"
#include "vm.h"

ObjFunction *compile(const char *source);
void markCompilerRoots(void);

#endif // INCLUDE_CLOX_COMPILER_H_
