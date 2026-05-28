#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "common.h"
#include "compiler.h"
#include "lexer.h"
#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "vm.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct {
    bool hadError, panicMode;
    Token prv, cur;
    Lexer lexer;
} Parser;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT, // =
    PREC_OR,         // or
    PREC_AND,        // and
    PREC_EQUALITY,   // == !=
    PREC_COMPARISON, // < > <= >=
    PREC_TERM,       // + -
    PREC_FACTOR,     // * / %
    PREC_UNARY,      // ! -
    PREC_CALL,       // . ()
    PREC_SUBSCRIPT,  // [expr]
    PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

typedef struct {
    Token name;
    int depth;
    OpCode exitOP;
} Local;

typedef struct {
    uint8_t index;
    bool isLocal;
} Upvalue;

typedef enum {
    TYPE_FUNCTION,
    TYPE_INITIALIZER,
    TYPE_METHOD,
    TYPE_SCRIPT,
} FunctionType;

typedef struct Loop {
    struct Loop *enclosing;
    int start;
    int body;
    int end;
    int scopeDepth;
} Loop;

typedef struct ClassCompiler {
    struct ClassCompiler *enclosing;
    bool hasSuperClass;
} ClassCompiler;

typedef struct Compiler {
    // current compiler info
    struct Compiler *enclosing;
    ObjFn *fn;
    FunctionType type;

    // constants info
    Table constantsTable;

    // loop info
    Loop *loop;

    // locals info
    Local locals[UINT8_COUNT];
    int localCount;

    // upvalue info
    Upvalue upvalues[UINT8_COUNT];

    // scope info
    int scopeDepth;
} Compiler;

Parser parser = {0};
Compiler *current = NULL;
ClassCompiler *currentClass = NULL;

static inline Chunk *curChunk(const Compiler *compiler) {
    return &compiler->fn->chunk;
}

static void errorAt(const Token *token, const char *message) {
    if (parser.panicMode) return;

    parser.panicMode = true;
    fprintf(stderr, "[line %d] Error", token->line);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (token->type) {
    case TOKEN_EOF:   fprintf(stderr, " at end"); break;
    case TOKEN_ERROR: break;
    default:          fprintf(stderr, " at '%.*s'", token->len, token->start); break;
    }
#pragma GCC diagnostic pop

    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

static inline void error(const char *msg) { errorAt(&parser.prv, msg); }
static inline void errorAtCurrent(const char *msg) {
    errorAt(&parser.cur, msg);
}

static void advance(void) {
    parser.prv = parser.cur;

    for (;;) {
        parser.cur = scanToken(&parser.lexer);
        if (parser.cur.type != TOKEN_ERROR) break;

        errorAtCurrent(parser.cur.start);
    }
}

static inline void consume(TokenType type, const char *message) {
    if (parser.cur.type == type) {
        advance();
        return;
    }
    errorAtCurrent(message);
}

static inline bool check(TokenType type) { return parser.cur.type == type; }
static inline bool match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

static inline void emitByte(Compiler *c, uint8_t byte) {
    writeChunk(curChunk(c), byte, parser.prv.line);
}

static inline void emitBytes(Compiler *c, uint8_t byte1, uint8_t byte2) {
    emitByte(c, byte1);
    emitByte(c, byte2);
}

static inline void emitShort(Compiler *c, int arg) {
    emitBytes(c, (arg >> 8) & 0xff, arg & 0xff);
}

static inline void emitOp(Compiler *c, OpCode op) { emitByte(c, (uint8_t)op); }
static inline void emitPop(Compiler *c) { emitOp(c, OP_POP); }

static inline void emitOpArg(Compiler *c, OpCode op, uint8_t arg) {
    emitBytes(c, op, arg);
}

static inline void emitOp2Args(Compiler *c, OpCode op, int arg1, int arg2) {
    emitOp(c, op);
    emitBytes(c, arg1, arg2);
}

static void emitLoop(Compiler *c, int loopStart) {
    emitOp(c, OP_LOOP);

    int offset = curChunk(c)->cnt - loopStart + 2;
    if (offset > UINT16_MAX) error("Loop body too large");

    emitShort(c, offset);
}

static inline int emitJump(Compiler *c, uint8_t inst) {
    emitOp2Args(c, inst, 0xff, 0xff);
    return curChunk(c)->cnt - 2;
}

static void emitReturn(Compiler *c) {
    // TODO: if c->type == method or function
    // and last opcode is OP_RETURN then don't emit
    // OP_NIL, and OP_RETURN
    if (c->type == TYPE_INITIALIZER) {
        emitOpArg(c, OP_GET_LOCAL, 0);
    } else {
        emitOp(c, OP_NIL);
    }
    emitOp(c, OP_RETURN);
}

static uint8_t makeConstant(Compiler *c, Value value) {
    Value existing = EMPTY_VAL;
    if (tableGet(&c->constantsTable, value, &existing)) {
        return (uint8_t)AS_NUMBER(existing); // reuse existing constant
    }

    // add constant
    // make sure not collected
    if (IS_OBJ(value)) pushRoot(value);
    int constIdx = addConst(curChunk(c), value);
    // its safe so can remove it from temp roots
    if (IS_OBJ(value)) popRoot();

    if (constIdx > UINT8_MAX) {
        error("Too many constants in on chunk");
        return 0;
    }

    tableSet(&c->constantsTable, value, NUMBER_VAL(constIdx));
    return (uint8_t)constIdx;
}

static inline void emitConstant(Compiler *c, Value value) {
    emitOpArg(c, OP_CONSTANT, makeConstant(c, value));
}

static void patchJump(Compiler *c, int offset) {
    // -2 to adjust for the bytecode for the jump offset itself
    int jump = curChunk(c)->cnt - offset - 2;

    if (jump > UINT16_MAX) error("Too much code to jump over");

    curChunk(c)->code[offset] = (jump >> 8) & 0xff;
    curChunk(c)->code[offset + 1] = jump & 0xff;
}

static void initCompiler(Compiler *compiler, FunctionType type) {
    *compiler = (Compiler){0};
    compiler->enclosing = current;
    compiler->type = type;
    compiler->fn = newFunction();
    current = compiler;

    if (type != TYPE_SCRIPT) {
        current->fn->name = copyString(parser.prv.start, parser.prv.len);
    }

    Local *local = &current->locals[current->localCount++];
    *local = (Local){{0}, 0, OP_POP};
    if (type != TYPE_FUNCTION) {
        local->name.start = "this";
        local->name.len = 4;
    } else {
        local->name.start = "";
        local->name.len = 0;
    }
}

static ObjFn *endCompiler(void) {
    emitReturn(current);
    ObjFn *fn = current->fn;

#ifdef DEBUG_PRINT_CODE
    if (!parser.hadError) {
        disassembleChunk(curChunk(current),
                         fn->name != NULL ? fn->name->chars : "<script>");
    }
#endif
    // free constants table
    freeTable(&current->constantsTable);
    current = current->enclosing;
    return fn;
}

static inline void beginScope(Compiler *compiler) { compiler->scopeDepth++; }

static void endScope(Compiler *compiler) {
    compiler->scopeDepth--;
    while (compiler->localCount > 0 &&
           compiler->locals[compiler->localCount - 1].depth >
               compiler->scopeDepth) {
        emitOp(compiler, compiler->locals[compiler->localCount - 1].exitOP);
        compiler->localCount--;
    }
}

static inline int getArgCount(const uint8_t *code, const ValueArray constants,
                              const int ip) {
    switch ((OpCode)code[ip]) {
    case OP_NOP:
    case OP_NIL:
    case OP_TRUE:
    case OP_FALSE:
    case OP_POP:
    case OP_EQUAL:
    case OP_NOT_EQUAL:
    case OP_GREATER:
    case OP_GREATER_EQUAL:
    case OP_LESS:
    case OP_LESS_EQUAL:
    case OP_ADD:
    case OP_MOD:
    case OP_SUBTRACT:
    case OP_MULTIPLY:
    case OP_DIVIDE:
    case OP_NOT:
    case OP_NEGATE:
    case OP_CLOSE_UPVALUE:
    case OP_RETURN:
    case OP_PRINT:
    case OP_INHERIT:
    case OP_GET_INDEX:
    case OP_SET_INDEX:     return 0;

    case OP_SMALL_INT:
    case OP_CONSTANT:
    case OP_GET_PROPERTY:
    case OP_SET_PROPERTY:
    case OP_GET_LOCAL:
    case OP_SET_LOCAL:
    case OP_GET_GLOBAL:
    case OP_SET_GLOBAL:
    case OP_DEFINE_GLOBAL:
    case OP_GET_UPVALUE:
    case OP_SET_UPVALUE:
    case OP_GET_SUPER:
    case OP_METHOD:
    case OP_BUILD_ARRAY:
    case OP_BUILD_MAP:     return 1;

    case OP_JUMP:
    case OP_JUMP_IF_FALSE:
    case OP_LOOP:
    case OP_CLASS:
    case OP_CALL:
    case OP_INVOKE:
    case OP_SUPER_INVOKE:  return 2;

    case OP_CLOSURE: {
        int constant = code[ip + 1];
        ObjFn *loadedFn = AS_FUNCTION(constants.values[constant]);

        // There is one byte for the constant, then two for each upvalue.
        return 1 + (loadedFn->upvalueCnt * 2);
    }
    }
    return 0;
}

static inline void initLoop(Compiler *c, Loop *loop) {
    *loop = (Loop){
        c->loop, curChunk(c)->cnt, 0, -1, c->scopeDepth,
    };
    c->loop = loop;
}

static void endLoop(Compiler *c, int loopStart) {
    emitLoop(c, loopStart);
    if (c->loop->end != -1) {
        patchJump(c, c->loop->end);
        emitPop(c);
    }

    int i = c->loop->body;
    Chunk *chunk = curChunk(c);
    while (i < chunk->cnt) {
        if (chunk->code[i] == OP_NOP) {
            chunk->code[i] = OP_JUMP;
            patchJump(c, i + 1);
            i += 3;
        } else {
            i += 1 + getArgCount(chunk->code, chunk->constants, i);
        }
    }

    c->loop = c->loop->enclosing;
}

static inline void expression(Compiler *c);
static void statement(Compiler *c);
static void declaration(Compiler *c);
static inline ParseRule *getRule(TokenType type);
static void parsePrecedence(Compiler *c, Precedence precedence);

static inline uint8_t identifierConst(Compiler *c, Token *name) {
    return makeConstant(c, OBJ_VAL(copyString(name->start, name->len)));
}

static inline bool identifiersEqual(Token *a, Token *b) {
    if (a->len != b->len) return false;
    return memcmp(a->start, b->start, a->len) == 0;
}

static int resolveLocal(Compiler *compiler, Token *name) {
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local *local = &compiler->locals[i];
        if (identifiersEqual(name, &local->name)) {
            if (local->depth == -1) {
                error("Can't read local variable in its own initializer");
            }
            return i;
        }
    }
    return -1;
}

static inline int addUpvalue(Compiler *compiler, uint8_t index, bool isLocal) {
    int upvalueCnt = compiler->fn->upvalueCnt;

    // check if upvalue already exists
    for (int i = 0; i < upvalueCnt; i++) {
        Upvalue *upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->isLocal == isLocal) {
            // reuse existing upvalue
            return i;
        }
    }

    if (upvalueCnt == UINT8_COUNT) {
        error("Too many closure variables in function");
        return 0;
    }

    compiler->upvalues[upvalueCnt] = (Upvalue){index, isLocal};
    return compiler->fn->upvalueCnt++;
}

static int resolveUpvalue(Compiler *compiler, Token *name) {
    if (compiler->enclosing == NULL) return -1;

    int local = resolveLocal(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].exitOP = OP_CLOSE_UPVALUE;
        return addUpvalue(compiler, (uint8_t)local, true);
    }

    int upvalue = resolveUpvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return addUpvalue(compiler, (uint8_t)upvalue, false);
    }

    return -1;
}

static int addLocal(Compiler *c, const Token name) {
    if (c->localCount == UINT8_COUNT) {
        error("Too many local variables in function");
        return -1;
    }

    c->locals[c->localCount++] = (Local){name, -1, OP_POP};
    return c->localCount - 1;
}

static void declareVariable(Compiler *c) {
    // globals need names
    if (c->scopeDepth == 0) return;

    Token *name = &parser.prv;
    for (int i = c->localCount - 1; i >= 0; i--) {
        Local *local = &c->locals[i];
        if (local->depth != -1 && local->depth < c->scopeDepth) {
            break;
        }

        if (identifiersEqual(name, &local->name)) {
            error("Already a variable with this name in the scope");
        }
    }

    addLocal(c, *name);
}

static uint8_t parseVariable(Compiler *c, const char *msg) {
    consume(TOKEN_IDENTIFIER, msg);

    declareVariable(c);
    // return dummy index instead, as locals aren't looked up by name
    if (c->scopeDepth > 0) return 0;

    return identifierConst(c, &parser.prv);
}

static void markInitialized(Compiler *c) {
    if (c->scopeDepth == 0) return;
    c->locals[c->localCount - 1].depth = c->scopeDepth;
}

static void defineVariable(Compiler *c, uint8_t globalIdx) {
    if (c->scopeDepth > 0) {
        markInitialized(c);
        // don't need to do anything as the initializer for
        // the local is a temporary and is therefore already on top
        // of the stack hence there is no need to do anything
        return;
    }
    emitOpArg(c, OP_DEFINE_GLOBAL, globalIdx);
}

static uint8_t argumentList(Compiler *c) {
    (void)c;
    uint8_t argCnt = 0;
    if (!check(TOKEN_RPAREN)) {
        do {
            expression(c);
            if (argCnt == 255) error("Can't have more than 255 arguments");
            argCnt++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RPAREN, "Expect ')' after arguments");
    return argCnt;
}

static void and_(bool canAssign) {
    (void)canAssign;
    int endJmpIdx = emitJump(current, OP_JUMP_IF_FALSE);

    emitPop(current);
    parsePrecedence(current, PREC_AND);

    patchJump(current, endJmpIdx);
}

static void binary(bool canAssign) {
    (void)canAssign;
    TokenType operatorType = parser.prv.type;
    ParseRule *rule = getRule(operatorType);
    parsePrecedence(current, (Precedence)(rule->precedence + 1));

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (operatorType) {
    case TOKEN_NEQ:     emitOp(current, OP_NOT_EQUAL); break;
    case TOKEN_EQEQ:    emitOp(current, OP_EQUAL); break;
    case TOKEN_GT:      emitOp(current, OP_GREATER); break;
    case TOKEN_GTEQ:    emitOp(current, OP_GREATER_EQUAL); break;
    case TOKEN_LT:      emitOp(current, OP_LESS); break;
    case TOKEN_LTEQ:    emitOp(current, OP_LESS_EQUAL); break;
    case TOKEN_PLUS:    emitOp(current, OP_ADD); break;
    case TOKEN_MINUS:   emitOp(current, OP_SUBTRACT); break;
    case TOKEN_STAR:    emitOp(current, OP_MULTIPLY); break;
    case TOKEN_SLASH:   emitOp(current, OP_DIVIDE); break;
    case TOKEN_PERCENT: emitOp(current, OP_MOD); break;
    default:            return; // Unreachable.
    }
#pragma GCC diagnostic pop
}

static void call(bool canAssign) {
    (void)canAssign;
    uint8_t argCnt = argumentList(current);
    emitOpArg(current, OP_CALL, argCnt);
}

static void dot(bool canAssign) {
    consume(TOKEN_IDENTIFIER, "Expect property name after '.'");
    uint8_t name = identifierConst(current, &parser.prv);

    if (canAssign && match(TOKEN_EQ)) {
        expression(current);
        emitOpArg(current, OP_SET_PROPERTY, name);
    } else if (match(TOKEN_LPAREN)) {
        uint8_t argCnt = argumentList(current);
        emitOp2Args(current, OP_INVOKE, name, argCnt);
    } else {
        emitOpArg(current, OP_GET_PROPERTY, name);
    }
}

static void literal(bool canAssign) {
    (void)canAssign;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (parser.prv.type) {
    case TOKEN_FALSE: emitOp(current, OP_FALSE); break;
    case TOKEN_TRUE:  emitOp(current, OP_TRUE); break;
    case TOKEN_NIL:   emitOp(current, OP_NIL); break;
    default:          return;
    }
#pragma GCC diagnostic pop
}

static void grouping(bool canAssign) {
    (void)canAssign;
    expression(current);
    consume(TOKEN_RPAREN, "Expect ')' after expression");
}

static void number(bool canAssign) {
    (void)canAssign;
    double value = strtod(parser.prv.start, NULL);
    if (trunc(value) == value) {
        if (value <= UINT8_MAX) {
            emitOpArg(current, OP_SMALL_INT, (uint8_t)(uint64_t)value);
            return;
        }
    }
    emitConstant(current, NUMBER_VAL(value));
}

static void or_(bool canAssign) {
    (void)canAssign;
    int elseJumpIdx = emitJump(current, OP_JUMP_IF_FALSE);
    int endJumpIdx = emitJump(current, OP_JUMP);

    patchJump(current, elseJumpIdx);
    emitPop(current);

    parsePrecedence(current, PREC_OR);
    patchJump(current, endJumpIdx);
}

static void string(bool canAssign) {
    (void)canAssign;
    Token prv = parser.prv;
    emitConstant(current, OBJ_VAL(copyString(prv.start + 1, prv.len - 2)));
}

static void array(bool canAssign) {
    (void)canAssign;

    int cnt = 0;
    if (!check(TOKEN_RSQR)) {
        do {
            // trailing comma
            if (check(TOKEN_RSQR)) break;

            parsePrecedence(current, PREC_OR);

            if (cnt > UINT8_MAX) {
                error("Can't have more than 255 items in an array literal");
            }
            cnt++;
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RSQR, "Expect ']' after array literal");

    emitOpArg(current, OP_BUILD_ARRAY, cnt);
}

static void map(bool canAssign) {
    (void)canAssign;

    int cnt = 0;
    if (!check(TOKEN_RBRACE)) {
        do {
            // trailing comma
            if (check(TOKEN_RBRACE)) break;

            parsePrecedence(current, PREC_OR); // key
            consume(TOKEN_COLON, "Expect ':' after map key");
            parsePrecedence(current, PREC_OR); // value

            if (cnt > UINT8_MAX) {
                error("Can't have more than 255 items in a map literal");
            }
            cnt++;
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RBRACE, "Expect '}' after map literal");

    emitOpArg(current, OP_BUILD_MAP, cnt);
}

static void subscript(bool canAssign) {
    parsePrecedence(current, PREC_OR);
    consume(TOKEN_RSQR, "Expect ']' after index");

    if (canAssign && match(TOKEN_EQ)) {
        expression(current);
        emitOp(current, OP_SET_INDEX);
    } else {
        emitOp(current, OP_GET_INDEX);
    }
}

static void namedVariable(Compiler *c, Token name, bool canAssign) {
    uint8_t getOp = OP_GET_GLOBAL, setOp = OP_SET_GLOBAL;
    int argIdx = resolveLocal(c, &name);
    if (argIdx != -1) {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
    } else if ((argIdx = resolveUpvalue(c, &name)) != -1) {
        getOp = OP_GET_UPVALUE;
        setOp = OP_SET_UPVALUE;
    } else {
        argIdx = identifierConst(c, &name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }

    if (canAssign && match(TOKEN_EQ)) {
        expression(c);
        emitOpArg(c, setOp, argIdx);
    } else {
        emitOpArg(c, getOp, argIdx);
    }
}

static inline void variable(bool canAssign) {
    namedVariable(current, parser.prv, canAssign);
}

static inline Token syntheticToken(const char *txt, const int len) {
    Token token = {0};
    token.start = txt;
    token.len = len;
    return token;
}

static inline uint8_t syntheticIdentifierConst(Compiler *c, const char *txt,
                                               const int len) {
    return makeConstant(c, OBJ_VAL(copyString(txt, len)));
}

static void super_(bool canAssign) {
    (void)canAssign;
    if (currentClass == NULL) {
        error("Can't use 'super' outside of a class");
    } else if (!currentClass->hasSuperClass) {
        error("Can't use 'super' in a class with no superclass");
    }

    consume(TOKEN_DOT, "Expect '.' after 'super'");
    consume(TOKEN_IDENTIFIER, "Expect superclass method name");
    uint8_t name = identifierConst(current, &parser.prv);

    namedVariable(current, syntheticToken("this", 4), false);
    if (match(TOKEN_LPAREN)) {
        uint8_t argCnt = argumentList(current);
        namedVariable(current, syntheticToken("super", 5), false);
        emitOp2Args(current, OP_SUPER_INVOKE, name, argCnt);
    } else {
        namedVariable(current, syntheticToken("super", 5), false);
        emitOpArg(current, OP_GET_SUPER, name);
    }
}

static void this_(bool canAssign) {
    (void)canAssign;
    if (currentClass == NULL) {
        error("Can't use 'this' outside of a class");
        return;
    }

    variable(false);
}

static void unary(bool canAssign) {
    (void)canAssign;
    TokenType operatorType = parser.prv.type;

    // compile the operand
    parsePrecedence(current, PREC_UNARY);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    // emit the operator instruction
    switch (operatorType) {
    case TOKEN_BANG:  emitOp(current, OP_NOT); break;
    case TOKEN_MINUS: emitOp(current, OP_NEGATE); break;
    default:          return;
    }
#pragma GCC diagnostic pop
}

static void function(FunctionType type);

static void lambda(bool canAssign) {
    (void)canAssign;
    function(TYPE_FUNCTION);
}

ParseRule rules[] = {
    [TOKEN_LPAREN] = {grouping, call, PREC_CALL},
    [TOKEN_RPAREN] = {NULL, NULL, PREC_NONE},
    [TOKEN_LBRACE] = {map, NULL, PREC_NONE},
    [TOKEN_RBRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_LSQR] = {array, subscript, PREC_SUBSCRIPT},
    [TOKEN_RSQR] = {NULL, NULL, PREC_NONE},
    [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},
    [TOKEN_DOT] = {NULL, dot, PREC_CALL},
    [TOKEN_SEMICOLON] = {NULL, NULL, PREC_NONE},
    [TOKEN_COLON] = {NULL, NULL, PREC_NONE},
    [TOKEN_MINUS] = {unary, binary, PREC_TERM},
    [TOKEN_PLUS] = {NULL, binary, PREC_TERM},
    [TOKEN_SLASH] = {NULL, binary, PREC_FACTOR},
    [TOKEN_STAR] = {NULL, binary, PREC_FACTOR},
    [TOKEN_PERCENT] = {NULL, binary, PREC_FACTOR},
    [TOKEN_BANG] = {unary, NULL, PREC_NONE},
    [TOKEN_EQ] = {NULL, NULL, PREC_NONE},
    [TOKEN_NEQ] = {NULL, binary, PREC_EQUALITY},
    [TOKEN_EQEQ] = {NULL, binary, PREC_EQUALITY},
    [TOKEN_GT] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_GTEQ] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LT] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LTEQ] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_IDENTIFIER] = {variable, NULL, PREC_NONE},
    [TOKEN_STRING] = {string, NULL, PREC_NONE},
    [TOKEN_NUMBER] = {number, NULL, PREC_NONE},
    [TOKEN_AND] = {NULL, and_, PREC_AND},
    [TOKEN_CLASS] = {NULL, NULL, PREC_NONE},
    [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
    [TOKEN_FALSE] = {literal, NULL, PREC_NONE},
    [TOKEN_FOR] = {NULL, NULL, PREC_NONE},
    [TOKEN_FUN] = {lambda, NULL, PREC_NONE},
    [TOKEN_IF] = {NULL, NULL, PREC_NONE},
    [TOKEN_NIL] = {literal, NULL, PREC_NONE},
    [TOKEN_OR] = {NULL, or_, PREC_OR},
    [TOKEN_PRINT] = {NULL, NULL, PREC_NONE},
    [TOKEN_RETURN] = {NULL, NULL, PREC_NONE},
    [TOKEN_SUPER] = {super_, NULL, PREC_NONE},
    [TOKEN_THIS] = {this_, NULL, PREC_NONE},
    [TOKEN_TRUE] = {literal, NULL, PREC_NONE},
    [TOKEN_VAR] = {NULL, NULL, PREC_NONE},
    [TOKEN_WHILE] = {NULL, NULL, PREC_NONE},
    [TOKEN_BREAK] = {NULL, NULL, PREC_NONE},
    [TOKEN_CONTINUE] = {NULL, NULL, PREC_NONE},
    [TOKEN_IN] = {NULL, NULL, PREC_NONE},
    [TOKEN_ERROR] = {NULL, NULL, PREC_NONE},
    [TOKEN_EOF] = {NULL, NULL, PREC_NONE},
};

static_assert(ARRAY_LEN(rules) == __TOKEN_CNT,
              "number of tokens is not equal to the number of rules");

static inline ParseRule *getRule(TokenType type) { return &rules[type]; }

static void parsePrecedence(Compiler *c, Precedence precedence) {
    (void)c;
    advance();
    ParseFn prefixRule = getRule(parser.prv.type)->prefix;
    if (prefixRule == NULL) {
        error("Expect expression");
        return;
    }

    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(canAssign);

    while (precedence <= getRule(parser.cur.type)->precedence) {
        advance();
        ParseFn infixRule = getRule(parser.prv.type)->infix;
        infixRule(canAssign);

        if (canAssign && match(TOKEN_EQ)) error("Invalid assignment target");
    }
}

static inline void expression(Compiler *c) {
    parsePrecedence(c, PREC_ASSIGNMENT);
}

static void block(Compiler *c) {
    while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
        declaration(c);
    }

    consume(TOKEN_RBRACE, "Expect '}' after block");
}

static void function(FunctionType type) {
    Compiler compiler = {0};
    initCompiler(&compiler, type);
    beginScope(current);

    consume(TOKEN_LPAREN, "Expect '(' after function name");
    if (!check(TOKEN_RPAREN)) {
        do {
            current->fn->arity++;
            if (current->fn->arity > 255) {
                errorAtCurrent("Can't have more than 255 parameters");
            }
            uint8_t constIdx = parseVariable(current, "Expect parameter name");
            defineVariable(current, constIdx);
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RPAREN, "Expect ')' after parameters");
    consume(TOKEN_LBRACE, "Expect '{' before function body");
    block(current);

    // don't need to call endScope as endCompiler
    // closes the scope implicitly
    ObjFn *function = endCompiler();
    emitOpArg(current, OP_CLOSURE, makeConstant(current, OBJ_VAL(function)));

    for (int i = 0; i < function->upvalueCnt; i++) {
        // true is represented as uint8_t 1
        // false is represented as uint8_t 0
        // so we can just write it straight
        Upvalue upv = compiler.upvalues[i];
        emitBytes(current, upv.isLocal, upv.index);
    }
}

static void method(Compiler *c) {
    consume(TOKEN_IDENTIFIER, "Expect method name");
    uint8_t constant = identifierConst(c, &parser.prv);

    FunctionType type = TYPE_METHOD;
    if (parser.prv.len == 4 && memcmp(parser.prv.start, "init", 4) == 0) {
        type = TYPE_INITIALIZER;
    }
    function(type);
    emitOpArg(c, OP_METHOD, constant);
}

static void classDecl(Compiler *c) {
    consume(TOKEN_IDENTIFIER, "Expect class name");
    Token className = parser.prv;
    uint8_t nameConst = identifierConst(c, &parser.prv);
    declareVariable(c);

    emitOpArg(c, OP_CLASS, nameConst);
    defineVariable(c, nameConst);

    ClassCompiler classCompiler = {currentClass, false};
    currentClass = &classCompiler;

    if (match(TOKEN_LT)) {
        consume(TOKEN_IDENTIFIER, "Expect superclass name");
        variable(false);

        if (identifiersEqual(&className, &parser.prv)) {
            error("A class can't inherit from itself");
        }

        beginScope(c);
        addLocal(c, syntheticToken("super", 5));
        defineVariable(c, 0);

        namedVariable(c, className, false);
        emitOp(c, OP_INHERIT);
        classCompiler.hasSuperClass = true;
    }

    namedVariable(c, className, false);
    consume(TOKEN_LBRACE, "Expect '{' before class body");
    while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
        method(c);
    }
    consume(TOKEN_RBRACE, "Expect '}' after class body");
    emitPop(c);

    if (classCompiler.hasSuperClass) endScope(c);

    currentClass = currentClass->enclosing;
}

static void funDecl(Compiler *c) {
    uint8_t globalIdx = parseVariable(c, "Expect function name");
    markInitialized(c);
    function(TYPE_FUNCTION);
    defineVariable(c, globalIdx);
}

static void varDecl(Compiler *c) {
    uint8_t globalIdx = parseVariable(c, "Expect variable name");

    if (match(TOKEN_EQ)) {
        expression(c);
    } else {
        emitOp(c, OP_NIL);
    }
    consume(TOKEN_SEMICOLON, "Expect ';' after variable declaraion");

    defineVariable(c, globalIdx);
}

static void expressionStmt(Compiler *c) {
    expression(c);
    consume(TOKEN_SEMICOLON, "Expect ';' after expression");
    emitPop(c);
}

// this compiles a `for (var ix, i in iterable) { ... }`
// and reports if it successfully managed to compile it
static bool forIterStmt(Compiler *c) {
    // a for loop of the form:
    // for (var ix, i in iterable) {
    //     print ix;
    //     print i;
    // }
    // will be compiled to something like this:
    // {
    //     var it = Iter(iterable);
    //     var ix;
    //     var i;
    //     while (it.next()) {
    //         ix = it.index();
    //         i = it.value();
    //         print ix;
    //         print i;
    //     }
    // }

    // first determine if it even is a for iterable loop and its type
    // 1) for (var ix, i in iterable) ...
    // 2) for (var i in iterable) ...
    //
    // since we don't know if we are in a forIterStmt we have to
    // parse carefully as we don't want to set panicMode or hadErr
    // in the parser as we may still be able to parse a normal for loop

    // the parser is currently pointing at the '(' after the `for` keyword
    // if the next token is not a `var` then we know it is not a forIterStmt
    if (!match(TOKEN_VAR)) return false;
    if (!match(TOKEN_IDENTIFIER)) return false;

    // remember the name incase it is a forIterStmt
    // we call it first name as if we are in a `for (var ix, i in iter) ...`
    // then this one will be the index and not the item
    Token first = parser.prv;

    // we still don't know if it is a forIterStmt but the odds are slightly
    // higher, but we still need to see either a `,` or an `in` token to confirm
    bool isIndexAndItem = false;
    if (match(TOKEN_COMMA) || match(TOKEN_IN)) {
        isIndexAndItem = parser.prv.type == TOKEN_COMMA;
    } else {
        return false;
    }

    // now we know we are in a forIterLoop so we can actually start
    // reporting errors, as we won't have to backtrack
    //
    // we still have to sync up the 2 possible kinds of forIterStmt's as
    // if its
    // `for (var ix, i in iter) ...`
    //   we are here ^
    // but if its
    // `for (var i in iter) ...`
    //    we are here ^
    // so we need to get them to both be pointing the iterator expression
    // so that we can compile it

    // while it may not be a `for (var ix, i in iter) ...` we still need to
    // have access to the 2nd variable in the rest of the scope
    Token second = {0};
    if (isIndexAndItem) {
        consume(TOKEN_IDENTIFIER,
                "Expected a second variable name after ',' in the for loop");
        // save the 2nd variable as we need it
        // correct it as we now have the 2nd variable
        Token tmp = first;
        first = parser.prv;
        second = tmp;
        // the parser state is now:
        // for (var ix, i in iter) ...
        //                ^
        // we still need to consume the `in` token
        consume(TOKEN_IN, "Expect 'in' after variable names in for loop");
    }

    // the 2 possible types are now synced up
    // so now we need to construct the iterator
    // we use the builtin Iter class this will then construct
    // the iterator object once the expression has been evaluated
    emitOpArg(c, OP_GET_GLOBAL, syntheticIdentifierConst(c, "Iter", 4));

    // so we can now compile the iterator expression and store it in a hidden
    // variable, the space in the name ensures that it won't collide with
    // user-define variables
    expression(c);

    // make sure to actually construct the the iterator object
    emitOpArg(c, OP_CALL, 1);

    // need need to make sure that there is enough space for the `it`, `i`, and
    // possibly the `ix` local variables
    int needed = isIndexAndItem ? 3 : 2;
    if (c->localCount + needed > UINT8_COUNT) {
        error("Too many local variables in scope. (Not enough space for "
              "for-loop internal variables)");
    }

    // add `it `
    int itSlot = addLocal(c, syntheticToken("it ", 3));
    defineVariable(c, 0);
    // add `i` or `ix` and initialize it
    int iSlot = addLocal(c, first);
    defineVariable(c, 1);
    emitOp(c, OP_NIL);

    // add `ix`and initialize it if we need it
    int ixSlot = -1;
    if (isIndexAndItem) {
        ixSlot = addLocal(c, second);
        defineVariable(c, 2);
        emitOp(c, OP_NIL);
    }

    // compile the loop body
    consume(TOKEN_RPAREN, "Expect ')' after loop expression");

    Loop loop = {0};
    initLoop(c, &loop);

    // advance the iterator
    emitOpArg(c, OP_GET_LOCAL, itSlot);
    emitOp2Args(c, OP_INVOKE, syntheticIdentifierConst(c, "next", 4), 0);

    // test the condition
    loop.end = emitJump(c, OP_JUMP_IF_FALSE);
    emitPop(c);

    // update i
    emitOpArg(c, OP_GET_LOCAL, itSlot);
    emitOp2Args(c, OP_INVOKE, syntheticIdentifierConst(c, "value", 5), 0);
    emitOpArg(c, OP_SET_LOCAL, iSlot);
    emitPop(c);

    // update ix if we need to
    if (isIndexAndItem) {
        emitOpArg(c, OP_GET_LOCAL, itSlot);
        emitOp2Args(c, OP_INVOKE, syntheticIdentifierConst(c, "index", 5), 0);
        emitOpArg(c, OP_SET_LOCAL, ixSlot);
        emitPop(c);
    }

    // compile the actual body
    loop.body = curChunk(c)->cnt;
    statement(c);
    endLoop(c, loop.start);
    endScope(c);
    return true;
}

static void forStmt(Compiler *c) {
    beginScope(c);
    consume(TOKEN_LPAREN, "Expect '(' after 'for'");

    // there are 2 types of for loops
    // 1) for (var i = 0; i < n; i = i +1) block
    // 2) for (var ix, i in iterable) block
    // we need to see which one it is and compile it correctly
    // to do this we have to save the state of the lexer and the parser
    // so that we can revert back to the current position if wasn't a
    // for loop over an iterable

    // the parser has the lexer in it so saving the parser also saves the lexer
    Parser savedParser = parser;
    // if it was a for loop over an iterable it will call endScope
    // and we are done compiling the for loop so we just return
    if (forIterStmt(c)) return;

    // otherwise we have to restore the state of the lexer and parser
    // and try to compile a normal for loop
    parser = savedParser;

    if (match(TOKEN_SEMICOLON)) {
        // no initializer
    } else if (match(TOKEN_VAR)) {
        varDecl(c);
    } else {
        expressionStmt(c);
    }

    Loop loop = {0};
    initLoop(c, &loop);
    if (!match(TOKEN_SEMICOLON)) {
        expression(c);
        consume(TOKEN_SEMICOLON, "Expect ';' after loop condition");

        // jmp out of the loop if cond is false
        loop.end = emitJump(c, OP_JUMP_IF_FALSE);
        emitPop(c); // cond
    }

    if (!match(TOKEN_RPAREN)) {
        int bodyJmpIdx = emitJump(c, OP_JUMP);
        int incrStartIdx = curChunk(c)->cnt;
        expression(c);
        emitPop(c);
        consume(TOKEN_RPAREN, "Expect ')' after clauses");

        emitLoop(c, loop.start);
        loop.start = incrStartIdx;
        patchJump(c, bodyJmpIdx);
    }

    loop.body = curChunk(c)->cnt;
    statement(c);
    endLoop(c, loop.start);
    endScope(c);
}

static void ifStmt(Compiler *c) {
    consume(TOKEN_LPAREN, "Expect '(' after if");
    expression(c);
    consume(TOKEN_RPAREN, "Expect ')' after condition");

    int thenJumpIdx = emitJump(c, OP_JUMP_IF_FALSE);
    emitPop(c); // true branch pop
    statement(c);

    int elseJumpIdx = emitJump(c, OP_JUMP);
    patchJump(c, thenJumpIdx);
    emitPop(c); // false branch pop

    if (match(TOKEN_ELSE)) statement(c);
    patchJump(c, elseJumpIdx);
}

static void printStmt(Compiler *c) {
    expression(c);
    consume(TOKEN_SEMICOLON, "Expect ';' after value");
    emitOp(c, OP_PRINT);
}

static void returnStmt(Compiler *c) {
    if (c->type == TYPE_SCRIPT) error("Can't return from top-level code");

    if (match(TOKEN_SEMICOLON)) {
        emitReturn(c);
    } else {
        if (c->type == TYPE_INITIALIZER) {
            error("Can't return a value from an initializer");
        }

        expression(c);
        consume(TOKEN_SEMICOLON, "Expect ';' after return value");
        emitOp(c, OP_RETURN);
    }
}

static void whileStmt(Compiler *c) {
    Loop loop = {0};
    initLoop(c, &loop);

    consume(TOKEN_LPAREN, "Expect '(' after 'while'");
    expression(c);
    consume(TOKEN_RPAREN, "Expect ')' after condition");

    loop.end = emitJump(c, OP_JUMP_IF_FALSE);
    emitPop(c);
    loop.body = curChunk(c)->cnt;
    statement(c);
    endLoop(c, loop.start);
}

static void synchronize(Parser *parser) {
    parser->panicMode = false;

    while (parser->cur.type != TOKEN_EOF) {
        if (parser->prv.type == TOKEN_SEMICOLON) return;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
        switch (parser->cur.type) {
        case TOKEN_BREAK:
        case TOKEN_CONTINUE:
        case TOKEN_CLASS:
        case TOKEN_FUN:
        case TOKEN_VAR:
        case TOKEN_FOR:
        case TOKEN_IF:
        case TOKEN_WHILE:
        case TOKEN_PRINT:
        case TOKEN_RETURN:   return;

        default:; // Do nothing.
        }
#pragma GCC diagnostic pop

        advance();
    }
}

static void declaration(Compiler *c) {
    if (match(TOKEN_CLASS)) {
        classDecl(c);
    } else if (match(TOKEN_FUN)) {
        funDecl(c);
    } else if (match(TOKEN_VAR)) {
        varDecl(c);
    } else {
        statement(c);
    }

    if (parser.panicMode) synchronize(&parser);
}

// discards any locals created
static inline void discardLocals(Compiler *c, int depth) {
    int i = c->localCount - 1;
    while (i >= 0 && c->locals[i].depth > depth) {
        emitOp(c, c->locals[i].exitOP);
        i--;
    }
}

static void statement(Compiler *c) {
    if (match(TOKEN_PRINT)) {
        printStmt(c);
    } else if (match(TOKEN_FOR)) {
        forStmt(c);
    } else if (match(TOKEN_IF)) {
        ifStmt(c);
    } else if (match(TOKEN_RETURN)) {
        returnStmt(c);
    } else if (match(TOKEN_WHILE)) {
        whileStmt(c);
    } else if (match(TOKEN_LBRACE)) {
        beginScope(c);
        block(c);
        endScope(c);
    } else if (match(TOKEN_BREAK)) {
        if (c->loop == NULL) {
            error("Can't use 'break' outside of loops");
            return;
        }

        consume(TOKEN_SEMICOLON, "Expected ';' after break");

        // discard any locals made in the loop
        discardLocals(c, c->loop->scopeDepth + 1);

        emitJump(c, OP_NOP);
    } else if (match(TOKEN_CONTINUE)) {
        if (c->loop == NULL) {
            error("Can't use 'continue' outside of loops");
            return;
        }

        consume(TOKEN_SEMICOLON, "Expected ';' after continue");

        // discard any locals made in the loop
        discardLocals(c, c->loop->scopeDepth + 1);

        // jump to top of the current innermost loop
        emitLoop(c, c->loop->start);
    } else {
        expressionStmt(c);
    }
}

ObjFn *compile(const char *source) {
    initLexer(&parser.lexer, source);
    Compiler compiler = {0};
    initCompiler(&compiler, TYPE_SCRIPT);

    parser.hadError = false;
    parser.panicMode = false;

    advance();

    while (!match(TOKEN_EOF)) {
        declaration(current);
    }

    ObjFn *function = endCompiler();
    return parser.hadError ? NULL : function;
}

void markCompilerRoots(void) {
    Compiler *compiler = current;
    while (compiler != NULL) {
        markObject((Obj *)compiler->fn);
        markTable(&compiler->constantsTable);
        compiler = compiler->enclosing;
    }
}
