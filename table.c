#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"

#define TABLE_MAX_LOAD 0.75

void initTable(Table *table) {
  table->cnt = 0;
  table->cap = 0;
  table->entries = NULL;
}

void freeTable(Table *table) {
  FREE_ARRAY(Entry, table->entries, table->cap);
  initTable(table);
}

static Entry *findEntry(Entry *entries, int cap, ObjString *key) {
  uint32_t idx = key->hash % cap;
  Entry *tombstone = NULL;

  for (;;) {
    Entry *entry = &entries[idx];
    if (entry->key == NULL) {
      if (IS_NIL(entry->value)) {
        // empty entry
        return tombstone != NULL ? tombstone : entry;
      } else {
        if (tombstone == NULL) {
          tombstone = entry;
        }
      }
    } else if (entry->key == key) {
      // found the key
      return entry;
    }

    idx = (idx + 1) % cap;
  }
}

bool tableGet(Table *table, ObjString *key, Value *value) {
  if (table->cnt == 0) {
    return false;
  }

  Entry *entry = findEntry(table->entries, table->cap, key);
  if (entry->key == NULL) {
    return false;
  }

  *value = entry->value;
  return true;
}

static void adjustCap(Table *table, int cap) {
  Entry *entries = ALLOCATE(Entry, cap);
  for (int i = 0; i < cap; i++) {
    entries[i].key = NULL;
    entries[i].value = NIL_VAL;
  }

  table->cnt = 0;
  for (int i = 0; i < table->cap; i++) {
    Entry *entry = &table->entries[i];
    if (entry->key == NULL) {
      continue;
    }

    Entry *dest = findEntry(entries, cap, entry->key);
    dest->key = entry->key;
    dest->value = entry->value;
    table->cnt++;
  }

  FREE_ARRAY(Entry, table->entries, table->cap);
  table->entries = entries;
  table->cap = cap;
}

bool tableSet(Table *table, ObjString *key, Value value) {
  if (table->cnt + 1 > table->cap * TABLE_MAX_LOAD) {
    int cap = GROW_CAP(table->cap);
    adjustCap(table, cap);
  }

  Entry *entry = findEntry(table->entries, table->cap, key);
  bool isNewKey = entry->key == NULL;
  if (isNewKey && IS_NIL(entry->value)) {
    table->cnt++;
  }
  entry->key = key;
  entry->value = value;
  return isNewKey;
}

bool tableDelete(Table *table, ObjString *key) {
  if (table->cnt == 0) {
    return false;
  }

  // find entry
  Entry *entry = findEntry(table->entries, table->cap, key);
  if (entry->key == NULL) {
    return false;
  }

  // place tombstone in the entry
  entry->key = NULL;
  entry->value = BOOL_VAL(true);
  return true;
}

void tableAddAll(Table *from, Table *to) {
  for (int i = 0; i < from->cap; i++) {
    Entry *entry = &from->entries[i];
    if (entry->key != NULL) {
      tableSet(to, entry->key, entry->value);
    }
  }
}

ObjString *tableFindString(Table *table, const char *chars, int length,
                           uint32_t hash) {
  if (table->cnt == 0) {
    return NULL;
  }

  uint32_t idx = hash % table->cap;
  for (;;) {
    Entry *entry = &table->entries[idx];
    if (entry->key == NULL) {
      // stop i fwe find an empty non-tombstone entry
      if (IS_NIL(entry->value)) {
        return NULL;
      }
    } else if (entry->key->length == length && entry->key->hash == hash &&
               memcmp(entry->key->chars, chars, length) == 0) {
      // we found it
      return entry->key;
    }
  }
}
