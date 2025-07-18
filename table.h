#ifndef INCLUDE_CLOX_TABLE_H_
#define INCLUDE_CLOX_TABLE_H_

#include "common.h"
#include "value.h"

typedef struct {
  Value key;
  Value value;
} Entry;

typedef struct {
  int cnt;
  int cap;
  Entry *entries;
} Table;

void initTable(Table *table);
void freeTable(Table *table);
bool tableGet(Table *table, Value key, Value *value);
bool tableSet(Table *table, Value key, Value value);
bool tableDelete(Table *table, Value key);
void tableAddAll(Table *from, Table *to);
ObjString *tableFindString(Table *table, const char *chars, int len,
                           uint32_t hash);

void tableRemoveWhite(Table *table);
void markTable(Table *table);

#endif // INCLUDE_CLOX_TABLE_H_
