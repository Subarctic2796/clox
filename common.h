#ifndef INCLUDE_CLOX_COMMON_H_
#define INCLUDE_CLOX_COMMON_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define LOX_DEBUG

#ifdef LOX_DEBUG // LOX_DEBUG
#define DEBUG_PRINT_CODE
#define DEBUG_TRACE_EXECUTION
#endif // LOX_DEBUG

#define UINT8_COUNT (UINT8_MAX + 1)

#endif // INCLUDE_CLOX_COMMON_H_
