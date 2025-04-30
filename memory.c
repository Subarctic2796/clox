#include "memory.h"
#include <stddef.h>
#include <stdlib.h>

void *reallocate(void *ptr, size_t oldSize, size_t newSize) {
  (void)oldSize; // tmp while oldSize not used

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
