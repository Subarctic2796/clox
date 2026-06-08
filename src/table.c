#include "table.h"
#include "memory.h"
#include "object.h"
#include "value.h"

#define TABLE_MAX_LOAD 0.75

void initTable(Table *table) { *table = (Table){0}; }

void freeTable(VM *vm, Table *table) {
    FREE_ARRAY(Entry, table->entries, table->cap);
    initTable(table);
}

static Entry *findEntry(Entry *entries, int cap, Value key) {
    uint32_t idx = hashValue(key) & (cap - 1);
    Entry *tombstone = NULL;

    for (;;) {
        Entry *entry = &entries[idx];
        if (IS_EMPTY(entry->key)) {
            if (IS_NIL(entry->value)) {
                // Empty entry.
                return tombstone != NULL ? tombstone : entry;
            } else {
                // We found a tombstone.
                if (tombstone == NULL) tombstone = entry;
            }
        } else if (valuesEqual(key, entry->key)) {
            // We found the key.
            return entry;
        }

        idx = (idx + 1) & (cap - 1);
    }
}

bool tableGet(Table *table, Value key, Value *value) {
    if (table->cnt == 0) return false;

    Entry *entry = findEntry(table->entries, table->cap, key);
    if (IS_EMPTY(entry->key)) return false;

    *value = entry->value;
    return true;
}

static void adjustCap(VM *vm, Table *table, int cap) {
    Entry *entries = ALLOCATE(Entry, cap);
    for (int i = 0; i < cap; i++) {
        entries[i] = (Entry){EMPTY_VAL, NIL_VAL};
    }

    table->cnt = 0;
    for (int i = 0; i < table->cap; i++) {
        Entry *entry = &table->entries[i];
        if (IS_EMPTY(entry->key)) continue;

        Entry *dest = findEntry(entries, cap, entry->key);
        *dest = (Entry){entry->key, entry->value};
        table->cnt++;
    }

    FREE_ARRAY(Entry, table->entries, table->cap);
    table->entries = entries;
    table->cap = cap;
}

bool tableSet(VM *vm, Table *table, Value key, Value value) {
    if (table->cnt + 1 > table->cap * TABLE_MAX_LOAD) {
        int cap = GROW_CAP(table->cap);
        adjustCap(vm, table, cap);
    }

    Entry *entry = findEntry(table->entries, table->cap, key);
    bool isNewKey = IS_EMPTY(entry->key);
    if (isNewKey && IS_NIL(entry->value)) table->cnt++;
    *entry = (Entry){key, value};
    return isNewKey;
}

bool tableDelete(Table *table, Value key) {
    if (table->cnt == 0) return false;

    // find entry
    Entry *entry = findEntry(table->entries, table->cap, key);
    if (IS_EMPTY(entry->key)) return false;

    // place tombstone in the entry
    *entry = (Entry){EMPTY_VAL, BOOL_VAL(true)};
    return true;
}

void tableClear(Table *table) {
    for (int i = 0; i < table->cnt; i++) {
        table->entries[i] = (Entry){EMPTY_VAL, NIL_VAL};
    }
    table->cnt = 0;
}

void tableAddAll(VM *vm, Table *from, Table *to) {
    for (int i = 0; i < from->cap; i++) {
        Entry *entry = &from->entries[i];
        if (!IS_EMPTY(entry->key)) tableSet(vm, to, entry->key, entry->value);
    }
}

ObjString *tableFindString(Table *table, const char *chars, int len,
                           uint32_t hash) {
    if (table->cnt == 0) return NULL;

    uint32_t idx = hash & (table->cap - 1);
    for (;;) {
        Entry *entry = &table->entries[idx];
        if (IS_EMPTY(entry->key)) {
            // stop if we find an empty non-tombstone entry
            if (IS_NIL(entry->value)) return NULL;
        } else if (IS_STRING(entry->key)) {
            ObjString *string = AS_STRING(entry->key);
            if (string->length == len && string->hash == hash &&
                memcmp(string->chars, chars, len) == 0) {
                // we found it
                return string;
            }
        }
        idx = (idx + 1) & (table->cap - 1);
    }
}

void tableRemoveWhite(Table *table) {
    for (int i = 0; i < table->cap; i++) {
        Entry *entry = &table->entries[i];
        if (IS_STRING(entry->key)) {
            ObjString *string = AS_STRING(entry->key);
            if (!string->obj.isMarked) tableDelete(table, entry->key);
        }
    }
}

void markTable(VM *vm, Table *table) {
    for (int i = 0; i < table->cap; i++) {
        Entry *entry = &table->entries[i];
        markValue(vm, entry->key);
        markValue(vm, entry->value);
    }
}
