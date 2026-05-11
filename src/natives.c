#include <time.h>

#include "common.h"
#include "memory.h"
#include "natives.h"
#include "object.h"

typedef struct {
    const char *name;
    const NativeFn fn;
} NativeDecl;

typedef struct {
    const char *name;
    const int numFns;
    const NativeDecl *fns;
} NativeClassDecl;

#define CHECK_ARITY_NATIVE(arity)                                              \
    if (argc != arity) {                                                       \
        return ERROR_VAL(false, "Expected " #arity " arguments but got %d",    \
                         argc);                                                \
    }

static Value clockNative(int argc, Value *args) {
    (void)argc;
    (void)args;
    return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

// append a value to the array
static Value appendNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(2);
    if (!IS_ARRAY(args[0])) {
        return ERROR_VAL(false, "Can only append to arrays");
    }

    appendToArray(AS_ARRAY(args[0]), args[1]);
    return NIL_VAL;
}

// delete an item from the array or map at index
static Value deleteNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(2);
    if (!(IS_ARRAY(args[0]) || IS_MAP(args[0]))) {
        return ERROR_VAL(false,
                         "Can only use 'delete' on maps and arrays, got %s",
                         typeofValue(args[0]));
    }

    if (IS_ARRAY(args[0])) {
        ObjArray *arr = AS_ARRAY(args[0]);
        int index = isValidIndex(args[1], arr->items.cnt);

        if (index == -1) {
            return ERROR_VAL(false, "Can only use numbers to index arrays");
        } else if (index == -2) {
            return ERROR_VAL(false,
                             "can only use integers to index into arrays");
        } else if (index == -3) {
            return ERROR_VAL(false, "index out of bounds");
        }

        deleteFromArray(arr, index);
        return NIL_VAL;
    } else if (IS_MAP(args[0])) {
        ObjMap *map = AS_MAP(args[0]);
        Value key = args[1];
        if (!isHashable(key)) {
            return ERROR_VAL(false, "%s is an unhashable type",
                             typeofValue(key));
        }

        tableDelete(&map->items, key);
        return NIL_VAL;
    }
    return NIL_VAL;
}

// returns length of a string, array or map
static Value lenNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(1);

    if (IS_STRING(args[0])) {
        return NUMBER_VAL(AS_STRING(args[0])->length);
    } else if (IS_ARRAY(args[0])) {
        return NUMBER_VAL(AS_ARRAY(args[0])->items.cnt);
    } else if (IS_MAP(args[0])) {
        return NUMBER_VAL(AS_MAP(args[0])->items.cnt);
    }
    return ERROR_VAL(false,
                     "Can only take the length of strings, arrays, and maps");
}

static Value errorNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(1);
    return ERROR_VAL(AS_BOOL(args[0]), "this is a recoverable error");
}

static Value typeofNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(1);
    Value v = args[0];
    if (IS_CLASS(v)) {
        return OBJ_VAL(AS_CLASS(v)->name);
    } else if (IS_INSTANCE(v)) {
        ObjString *name = AS_INSTANCE(v)->klass->name;

        int len = name->length + 9;
        char *buf = ALLOCATE(char, len + 1);
        snprintf(buf, len + 1, "%s instance", name->chars);

        return OBJ_VAL(takeString(buf, len));
    }
    const char *str = typeofValue(v);
    return OBJ_VAL(copyString(str, (int)strnlen(str, 17)));
}

static Value keysNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(1);
    Value v = args[0];
    if (!IS_MAP(v)) {
        return ERROR_VAL(false, "'keys' can only be used on maps, got %s",
                         typeofValue(v));
    }

    Table map = AS_MAP(v)->items;

    ObjArray *arr = newArray();
    pushRoot(OBJ_VAL(arr));
    for (int i = 0; i < map.cap; i++) {
        Entry entry = map.entries[i];
        if (IS_EMPTY(entry.key)) continue;
        appendToArray(arr, entry.key);
    }
    popRoot();
    return OBJ_VAL(arr);
}

static Value iterInitNative(int argc, Value *args) {
    CHECK_ARITY_NATIVE(1);
    if (!isIndexable(args[0])) {
        return ERROR_VAL(
            false, "Can only create iterators from strings, arrays, and maps");
    }

    ObjInstance *inst = AS_INSTANCE(args[-1]);

    ObjString *obj = copyString("obj", 3);
    pushRoot(OBJ_VAL(obj));
    ObjString *idx = copyString("_index", 6);
    pushRoot(OBJ_VAL(idx));

    // add obj and _index to the instance's fields
    tableSet(&inst->fields, OBJ_VAL(obj), args[0]);
    tableSet(&inst->fields, OBJ_VAL(idx), NUMBER_VAL(0));

    popRoot(); // obj
    popRoot(); // idx

    return OBJ_VAL(inst);
}

static Value iterNextNative(int argc, Value *args) {
#define OBJ_HASH 3343205242
#define IDX_HASH 1364385362

    CHECK_ARITY_NATIVE(0);

    ObjInstance *iter = AS_INSTANCE(args[-1]);
    ObjString *_objStr = tableFindString(&iter->fields, "obj", 3, OBJ_HASH);
    ObjString *_idxStr = tableFindString(&iter->fields, "_index", 6, IDX_HASH);

    Value obj = EMPTY_VAL, idx = EMPTY_VAL;
    tableGet(&iter->fields, OBJ_VAL(_objStr), &obj);
    tableGet(&iter->fields, OBJ_VAL(_idxStr), &idx);

    int index = AS_NUMBER(idx);
    Value result = FALSE_VAL;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (OBJ_TYPE(obj)) {
    case OBJ_STRING: result = BOOL_VAL(index < AS_STRING(obj)->length); break;
    case OBJ_ARRAY:  result = BOOL_VAL(index < AS_ARRAY(obj)->items.cnt); break;
    case OBJ_MAP:    {
        Table map = AS_MAP(obj)->items;
        for (; index < map.cap; index++) {
            if (!IS_EMPTY(map.entries[index].key)) break;
        }
        result = BOOL_VAL(index < map.cap);
    } break;
    default: return FALSE_VAL;
    }
#pragma GCC diagnostic pop

    // update index
    tableSet(&iter->fields, OBJ_VAL(_idxStr), NUMBER_VAL(index + 1));

    return result;

#undef OBJ_HASH
#undef IDX_HASH
}

static Value iterValueNative(int argc, Value *args) {
#define OBJ_HASH 3343205242
#define IDX_HASH 1364385362

    CHECK_ARITY_NATIVE(0);

    ObjInstance *iter = AS_INSTANCE(args[-1]);
    ObjString *_objStr = tableFindString(&iter->fields, "obj", 3, OBJ_HASH);
    ObjString *_idxStr = tableFindString(&iter->fields, "_index", 6, IDX_HASH);

    Value obj = EMPTY_VAL, idx = EMPTY_VAL;
    tableGet(&iter->fields, OBJ_VAL(_objStr), &obj);
    tableGet(&iter->fields, OBJ_VAL(_idxStr), &idx);

    int index = AS_NUMBER(idx) - 1;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (OBJ_TYPE(obj)) {
    case OBJ_STRING: return OBJ_VAL(copyString(AS_CSTRING(obj) + index, 1));
    case OBJ_ARRAY:  return AS_ARRAY(obj)->items.values[index];
    case OBJ_MAP:    return AS_MAP(obj)->items.entries[index].value;
    default:         return NIL_VAL;
    }
#pragma GCC diagnostic pop

    return NIL_VAL;

#undef OBJ_HASH
#undef IDX_HASH
}

static Value iterIndexNative(int argc, Value *args) {
#define OBJ_HASH 3343205242
#define IDX_HASH 1364385362

    CHECK_ARITY_NATIVE(0);

    ObjInstance *iter = AS_INSTANCE(args[-1]);
    ObjString *_idxStr = tableFindString(&iter->fields, "_index", 6, IDX_HASH);
    ObjString *_objStr = tableFindString(&iter->fields, "obj", 3, OBJ_HASH);

    Value obj = EMPTY_VAL, idx = EMPTY_VAL;
    tableGet(&iter->fields, OBJ_VAL(_idxStr), &idx);
    tableGet(&iter->fields, OBJ_VAL(_objStr), &obj);

    double index = AS_NUMBER(idx) - 1;
    if (IS_MAP(obj)) return AS_MAP(obj)->items.entries[(int)index].key;
    return NUMBER_VAL(index);

#undef OBJ_HASH
#undef IDX_HASH
}

static void defineNativeClass(VM *vm, const NativeClassDecl decl) {
    // add class to globals
    ObjString *kname = copyString(decl.name, (int)strnlen(decl.name, 1024));
    pushRoot(OBJ_VAL(kname));
    ObjClass *klass = newClass(kname);
    pushRoot(OBJ_VAL(klass));
    tableSet(&vm->globals, OBJ_VAL(kname), OBJ_VAL(klass));

    // add native functions to the class
    for (int i = 0; i < decl.numFns; i++) {
        NativeDecl fn = decl.fns[i];
        ObjString *fname = copyString(fn.name, (int)strnlen(fn.name, 1024));
        pushRoot(OBJ_VAL(fname));
        ObjNative *native = newNative(fn.fn);
        pushRoot(OBJ_VAL(native));
        tableSet(&klass->methods, OBJ_VAL(fname), OBJ_VAL(native));
        popRoot(); // fn name
        popRoot(); // fn
    }

    popRoot(); // class name
    popRoot(); // class
}

static void defineNative(VM *vm, const NativeDecl decl) {
    ObjString *nativeName =
        copyString(decl.name, (int)strnlen(decl.name, 1024));
    pushRoot(OBJ_VAL(nativeName));
    ObjNative *fn = newNative(decl.fn);
    pushRoot(OBJ_VAL(fn));
    tableSet(&vm->globals, OBJ_VAL(nativeName), OBJ_VAL(fn));
    popRoot(); // pop native ptr
    popRoot(); // pop native name
}

void defineAllNatives(VM *vm) {
    static const NativeDecl NATIVE_FNS[] = {
        {"clock", clockNative},   {"append", appendNative},
        {"delete", deleteNative}, {"len", lenNative},
        {"error", errorNative},   {"typeof", typeofNative},
        {"keys", keysNative},
    };

    for (size_t i = 0; i < ARRAY_LEN(NATIVE_FNS); i++) {
        defineNative(vm, NATIVE_FNS[i]);
    }

    static const NativeDecl ITER_FNS[] = {
        {"init", iterInitNative},
        {"next", iterNextNative},
        {"value", iterValueNative},
        {"index", iterIndexNative},
    };

    static const NativeClassDecl NATIVE_CLASSES[] = {
        {"Iter", 4, ITER_FNS},
    };

    for (size_t i = 0; i < ARRAY_LEN(NATIVE_CLASSES); i++) {
        defineNativeClass(vm, NATIVE_CLASSES[i]);
    }
}
