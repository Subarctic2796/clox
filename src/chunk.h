#ifndef INCLUDE_CLOX_CHUNK_H_
#define INCLUDE_CLOX_CHUNK_H_

#include "common.h"
#include "value.h"

typedef enum {
    // 0 args
    OP_NOP,
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_POP, // easy optimization OP_POPN, pop n slots at once
    OP_INHERIT,
    OP_EQUAL,
    OP_NOT_EQUAL,
    OP_GREATER,
    OP_GREATER_EQUAL,
    OP_LESS,
    OP_LESS_EQUAL,
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MOD,
    OP_NOT,
    OP_NEGATE,
    OP_PRINT,
    OP_CLOSE_UPVALUE,
    OP_RETURN,
    OP_GET_INDEX,
    OP_SET_INDEX,

    // 1 args
    OP_CONSTANT,
    OP_SMALL_INT,
    OP_BUILD_ARRAY,
    OP_BUILD_MAP,
    OP_METHOD,
    OP_DEFINE_GLOBAL,
    OP_GET_GLOBAL,
    OP_SET_GLOBAL,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_PROPERTY,
    OP_SET_PROPERTY,
    OP_GET_UPVALUE,
    OP_SET_UPVALUE,
    OP_GET_SUPER,

    // 2 args
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_LOOP,
    OP_CLASS,
    OP_CALL,
    OP_INVOKE,
    OP_SUPER_INVOKE,

    // n args
    OP_CLOSURE,
} OpCode;

typedef struct {
    int offset, line;
} LineInfo;

typedef struct {
    int cnt;
    int cap;
    uint8_t *code;
    ValueArray constants;

    LineInfo *lines;
    int lineCnt;
    int lineCap;
} Chunk;

typedef struct VM VM;

void initChunk(Chunk *chunk);
void freeChunk(VM *vm, Chunk *chunk);
void writeChunk(VM *vm, Chunk *chunk, uint8_t byte, int line);
int addConst(VM *vm, Chunk *chunk, Value value);
int getLine(Chunk *chunk, int instruction);

#endif // INCLUDE_CLOX_CHUNK_H_
