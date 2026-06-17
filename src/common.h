#ifndef INCLUDE_CLOX_COMMON_H_
#define INCLUDE_CLOX_COMMON_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define ARRAY_LEN(a) (sizeof((a)) / sizeof((a)[0]))

#define NAN_BOXING

#define LOX_DEBUG

#ifdef LOX_DEBUG // LOX_DEBUG
#define DEBUG_PRINT_CODE
// #define DEBUG_TRACE_EXECUTION
#define DEBUG_STRESS_GC
// #define DEBUG_LOG_GC

#define UNREACHABLE()                                                          \
    do {                                                                       \
        fprintf(stderr, "[%s:%d] in %s() should be unreachable\n", __FILE__,   \
                __LINE__, __func__);                                           \
        abort();                                                               \
    } while (0)
#else
#define UNREACHABLE() __builtin_unreachable()
#endif // LOX_DEBUG

#define UINT8_COUNT (UINT8_MAX + 1)

#endif // INCLUDE_CLOX_COMMON_H_
