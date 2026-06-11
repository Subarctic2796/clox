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

typedef struct VM VM;

void initTable(Table *table);
void freeTable(VM *vm, Table *table);
bool tableGet(Table *table, Value key, Value *value);
bool tableSet(VM *vm, Table *table, Value key, Value value);
bool tableDelete(Table *table, Value key);
void tableClear(Table *table);
bool tableContains(Table *table, Value key);
void tableAddAll(VM *vm, Table *from, Table *to);
ObjString *tableFindString(Table *table, const char *chars, int len,
                           uint32_t hash);

void tableRemoveWhite(Table *table);
void markTable(VM *vm, Table *table);

#endif // INCLUDE_CLOX_TABLE_H_
