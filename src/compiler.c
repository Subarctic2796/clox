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

static inline void initParser(Parser *p, const char *src) {
    initLexer(&p->lexer, src);
    p->hadError = false;
    p->panicMode = false;
}

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

typedef struct Compiler Compiler;

typedef void (*ParseFn)(Compiler *c, bool canAssign);

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
    Parser *parser;
    struct Compiler *enclosing;
    ObjFn *fn;
    FunctionType type;
    ClassCompiler *currentClass;

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

static VM *vm = NULL;
static Compiler *current = NULL;

static inline Chunk *curChunk(const Compiler *compiler) {
    return &compiler->fn->chunk;
}

static void errorAt(Parser *parser, const Token *token, const char *message) {
    if (parser->panicMode) return;

    parser->panicMode = true;
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
    parser->hadError = true;
}

static inline void error(Parser *parser, const char *msg) {
    errorAt(parser, &parser->prv, msg);
}

static inline void errorAtCurrent(Parser *parser, const char *msg) {
    errorAt(parser, &parser->cur, msg);
}

static void advance(Parser *parser) {
    parser->prv = parser->cur;

    for (;;) {
        parser->cur = scanToken(&parser->lexer);
        if (parser->cur.type != TOKEN_ERROR) break;

        errorAtCurrent(parser, parser->cur.start);
    }
}

static inline void consume(Compiler *c, TokenType type, const char *message) {
    if (c->parser->cur.type == type) {
        advance(c->parser);
        return;
    }
    errorAtCurrent(c->parser, message);
}

static inline bool check(Compiler *c, TokenType type) {
    return c->parser->cur.type == type;
}

static inline bool match(Compiler *c, TokenType type) {
    if (!check(c, type)) return false;
    advance(c->parser);
    return true;
}

static inline void emitByte(Compiler *c, uint8_t byte) {
    writeChunk(vm, curChunk(c), byte, c->parser->prv.line);
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
    if (offset > UINT16_MAX) error(c->parser, "Loop body too large");

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
    if (IS_OBJ(value)) pushRoot(vm, value);
    int constIdx = addConst(vm, curChunk(c), value);
    // its safe so can remove it from temp roots
    if (IS_OBJ(value)) popRoot(vm);

    if (constIdx > UINT8_MAX) {
        error(c->parser, "Too many constants in on chunk");
        return 0;
    }

    tableSet(vm, &c->constantsTable, value, NUMBER_VAL(constIdx));
    return (uint8_t)constIdx;
}

static inline void emitConstant(Compiler *c, Value value) {
    emitOpArg(c, OP_CONSTANT, makeConstant(c, value));
}

static void patchJump(Compiler *c, int offset) {
    // -2 to adjust for the bytecode for the jump offset itself
    int jump = curChunk(c)->cnt - offset - 2;

    if (jump > UINT16_MAX) error(c->parser, "Too much code to jump over");

    curChunk(c)->code[offset] = (jump >> 8) & 0xff;
    curChunk(c)->code[offset + 1] = jump & 0xff;
}

static void initCompiler(Compiler *compiler, Compiler *enclosing,
                         Parser *parser, FunctionType type) {
    *compiler = (Compiler){0};
    compiler->parser = parser;
    compiler->enclosing = enclosing;
    compiler->currentClass = enclosing == NULL ? NULL : enclosing->currentClass;
    compiler->type = type;
    compiler->fn = newFunction(vm);
    current = compiler;

    if (type != TYPE_SCRIPT) {
        compiler->fn->name = copyString(vm, parser->prv.start, parser->prv.len);
    }

    Local *local = &compiler->locals[compiler->localCount++];
    *local = (Local){{0}, 0, OP_POP};
    if (type != TYPE_FUNCTION) {
        local->name.start = "this";
        local->name.len = 4;
    } else {
        local->name.start = "";
        local->name.len = 0;
    }
}

static ObjFn *endCompiler(Compiler *compiler) {
    (void)compiler;
    Compiler *compiler_ = current;

    emitReturn(compiler_);
    ObjFn *fn = compiler_->fn;

#ifdef DEBUG_PRINT_CODE
    if (!compiler_->parser->hadError) {
        disassembleChunk(curChunk(compiler_),
                         fn->name != NULL ? fn->name->chars : "<script>");
    }
#endif
    // free constants table
    freeTable(vm, &compiler_->constantsTable);
    compiler_ = compiler_->enclosing;
    current = compiler_;
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

typedef enum {
    IDENT_VAR,
    IDENT_IDENT,
    IDENT_CLASS_LOCAL,
    IDENT_CLASS_GLOBAL,
} IdentType;

static uint8_t identifierConst(Compiler *c, Token *name, IdentType type,
                               uint8_t *classGlobal) {
    ObjString *ident = copyString(vm, name->start, name->len);
    switch (type) {
    case IDENT_IDENT:        return makeConstant(c, OBJ_VAL(ident));
    case IDENT_CLASS_LOCAL:  return makeConstant(c, OBJ_VAL(ident));
    case IDENT_VAR:
    case IDENT_CLASS_GLOBAL: {
        Value index = EMPTY_VAL;
        if (tableGet(&vm->globalNames, OBJ_VAL(ident), &index)) {
            return (uint8_t)AS_NUMBER(index);
        }

        pushRoot(vm, OBJ_VAL(ident));

        uint8_t newIndex = (uint8_t)vm->globalValues.cnt;
        writeValueArray(vm, &vm->globalValues, EMPTY_VAL);
        tableSet(vm, &vm->globalNames, OBJ_VAL(ident),
                 NUMBER_VAL((double)newIndex));

        popRoot(vm);

        if (type == IDENT_CLASS_GLOBAL) {
            *classGlobal = newIndex;
            return makeConstant(c, OBJ_VAL(ident));
        }
        return newIndex;
    }
    default: return 0;
    }
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
                error(compiler->parser,
                      "Can't read local variable in its own initializer");
            }
            return i;
        }
    }
    return -1;
}

static int addUpvalue(Compiler *compiler, uint8_t index, bool isLocal) {
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
        error(compiler->parser, "Too many closure variables in function");
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
        error(c->parser, "Too many local variables in function");
        return -1;
    }

    c->locals[c->localCount++] = (Local){name, -1, OP_POP};
    return c->localCount - 1;
}

static void declareVariable(Compiler *c) {
    // globals need names
    if (c->scopeDepth == 0) return;

    Token *name = &c->parser->prv;
    for (int i = c->localCount - 1; i >= 0; i--) {
        Local *local = &c->locals[i];
        if (local->depth != -1 && local->depth < c->scopeDepth) {
            break;
        }

        if (identifiersEqual(name, &local->name)) {
            error(c->parser, "Already a variable with this name in the scope");
        }
    }

    addLocal(c, *name);
}

static uint8_t parseVariable(Compiler *c, const char *msg) {
    consume(c, TOKEN_IDENTIFIER, msg);

    declareVariable(c);
    // return dummy index instead, as locals aren't looked up by name
    if (c->scopeDepth > 0) return 0;

    return identifierConst(c, &c->parser->prv, IDENT_VAR, NULL);
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
    if (match(c, TOKEN_RPAREN)) return 0;

    uint8_t argCnt = 0;
    do {
        expression(c);
        if (argCnt == 255)
            error(c->parser, "Can't have more than 255 arguments");
        argCnt++;
    } while (match(c, TOKEN_COMMA));
    consume(c, TOKEN_RPAREN, "Expect ')' after arguments");
    return argCnt;
}

static void and_(Compiler *c, bool canAssign) {
    (void)canAssign;
    int endJmpIdx = emitJump(c, OP_JUMP_IF_FALSE);

    emitPop(c);
    parsePrecedence(c, PREC_AND);

    patchJump(c, endJmpIdx);
}

static void binary(Compiler *c, bool canAssign) {
    (void)canAssign;
    TokenType operatorType = c->parser->prv.type;
    ParseRule *rule = getRule(operatorType);
    parsePrecedence(c, (Precedence)(rule->precedence + 1));

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (operatorType) {
    case TOKEN_NEQ:     emitOp(c, OP_NOT_EQUAL); break;
    case TOKEN_EQEQ:    emitOp(c, OP_EQUAL); break;
    case TOKEN_GT:      emitOp(c, OP_GREATER); break;
    case TOKEN_GTEQ:    emitOp(c, OP_GREATER_EQUAL); break;
    case TOKEN_LT:      emitOp(c, OP_LESS); break;
    case TOKEN_LTEQ:    emitOp(c, OP_LESS_EQUAL); break;
    case TOKEN_PLUS:    emitOp(c, OP_ADD); break;
    case TOKEN_MINUS:   emitOp(c, OP_SUBTRACT); break;
    case TOKEN_STAR:    emitOp(c, OP_MULTIPLY); break;
    case TOKEN_SLASH:   emitOp(c, OP_DIVIDE); break;
    case TOKEN_PERCENT: emitOp(c, OP_MOD); break;
    default:            return; // Unreachable.
    }
#pragma GCC diagnostic pop
}

static void call(Compiler *c, bool canAssign) {
    (void)canAssign;
    uint8_t argCnt = argumentList(c);
    emitOpArg(c, OP_CALL, argCnt);
}

static void dot(Compiler *c, bool canAssign) {
    consume(c, TOKEN_IDENTIFIER, "Expect property name after '.'");
    uint8_t name = identifierConst(c, &c->parser->prv, IDENT_IDENT, NULL);

    if (canAssign && match(c, TOKEN_EQ)) {
        expression(c);
        emitOpArg(c, OP_SET_PROPERTY, name);
    } else if (match(c, TOKEN_LPAREN)) {
        uint8_t argCnt = argumentList(c);
        emitOp2Args(c, OP_INVOKE, name, argCnt);
    } else {
        emitOpArg(c, OP_GET_PROPERTY, name);
    }
}

static void literal(Compiler *c, bool canAssign) {
    (void)canAssign;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (c->parser->prv.type) {
    case TOKEN_FALSE: emitOp(c, OP_FALSE); break;
    case TOKEN_TRUE:  emitOp(c, OP_TRUE); break;
    case TOKEN_NIL:   emitOp(c, OP_NIL); break;
    default:          return;
    }
#pragma GCC diagnostic pop
}

static void grouping(Compiler *c, bool canAssign) {
    (void)canAssign;
    expression(c);
    consume(c, TOKEN_RPAREN, "Expect ')' after expression");
}

static void number(Compiler *c, bool canAssign) {
    (void)canAssign;
    double value = strtod(c->parser->prv.start, NULL);
    if (trunc(value) == value) {
        if (value <= UINT8_MAX) {
            emitOpArg(c, OP_SMALL_INT, (uint8_t)(uint64_t)value);
            return;
        }
    }
    emitConstant(c, NUMBER_VAL(value));
}

static void or_(Compiler *c, bool canAssign) {
    (void)canAssign;
    int elseJumpIdx = emitJump(c, OP_JUMP_IF_FALSE);
    int endJumpIdx = emitJump(c, OP_JUMP);

    patchJump(c, elseJumpIdx);
    emitPop(c);

    parsePrecedence(c, PREC_OR);
    patchJump(c, endJumpIdx);
}

static void string(Compiler *c, bool canAssign) {
    (void)canAssign;
    Token prv = c->parser->prv;
    emitConstant(c, OBJ_VAL(copyString(vm, prv.start + 1, prv.len - 2)));
}

static void array(Compiler *c, bool canAssign) {
    (void)canAssign;

    if (match(c, TOKEN_RSQR)) {
        emitOpArg(c, OP_BUILD_ARRAY, 0);
        return;
    }

    int cnt = 0;
    do {
        // trailing comma
        if (check(c, TOKEN_RSQR)) break;

        parsePrecedence(c, PREC_OR);

        if (cnt > UINT8_MAX) {
            error(c->parser,
                  "Can't have more than 255 items in an array literal");
        }
        cnt++;
    } while (match(c, TOKEN_COMMA));

    consume(c, TOKEN_RSQR, "Expect ']' after array literal");

    emitOpArg(c, OP_BUILD_ARRAY, cnt);
}

static void map(Compiler *c, bool canAssign) {
    (void)canAssign;

    int cnt = 0;
    if (match(c, TOKEN_RBRACE)) {
        emitOpArg(c, OP_BUILD_MAP, cnt);
        return;
    }

    do {
        // trailing comma
        if (check(c, TOKEN_RBRACE)) break;

        parsePrecedence(c, PREC_OR); // key
        consume(c, TOKEN_COLON, "Expect ':' after map key");
        parsePrecedence(c, PREC_OR); // value

        if (cnt > UINT8_MAX) {
            error(c->parser, "Can't have more than 255 items in a map literal");
        }
        cnt++;
    } while (match(c, TOKEN_COMMA));

    consume(c, TOKEN_RBRACE, "Expect '}' after map literal");

    emitOpArg(c, OP_BUILD_MAP, cnt);
}

static void subscript(Compiler *c, bool canAssign) {
    parsePrecedence(c, PREC_OR);
    consume(c, TOKEN_RSQR, "Expect ']' after index");

    if (canAssign && match(c, TOKEN_EQ)) {
        expression(c);
        emitOp(c, OP_SET_INDEX);
    } else {
        emitOp(c, OP_GET_INDEX);
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
        argIdx = identifierConst(c, &name, IDENT_VAR, NULL);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }

    if (canAssign && match(c, TOKEN_EQ)) {
        expression(c);
        emitOpArg(c, setOp, argIdx);
    } else {
        emitOpArg(c, getOp, argIdx);
    }
}

static inline void variable(Compiler *c, bool canAssign) {
    namedVariable(c, c->parser->prv, canAssign);
}

static inline Token syntheticToken(const char *txt, const int len) {
    Token token = {0};
    token.start = txt;
    token.len = len;
    return token;
}

static inline uint8_t syntheticIdentifierConst(Compiler *c, const char *txt,
                                               const int len, bool isVar) {
    Token tok = syntheticToken(txt, len);
    return identifierConst(c, &tok, isVar ? IDENT_VAR : IDENT_IDENT, NULL);
}

static void super_(Compiler *c, bool canAssign) {
    (void)canAssign;
    if (c->currentClass == NULL) {
        error(c->parser, "Can't use 'super' outside of a class");
    } else if (!c->currentClass->hasSuperClass) {
        error(c->parser, "Can't use 'super' in a class with no superclass");
    }

    consume(c, TOKEN_DOT, "Expect '.' after 'super'");
    consume(c, TOKEN_IDENTIFIER, "Expect superclass method name");
    uint8_t name = identifierConst(c, &c->parser->prv, IDENT_IDENT, NULL);

    namedVariable(c, syntheticToken("this", 4), false);
    if (match(c, TOKEN_LPAREN)) {
        uint8_t argCnt = argumentList(c);
        namedVariable(c, syntheticToken("super", 5), false);
        emitOp2Args(c, OP_SUPER_INVOKE, name, argCnt);
    } else {
        namedVariable(c, syntheticToken("super", 5), false);
        emitOpArg(c, OP_GET_SUPER, name);
    }
}

static void this_(Compiler *c, bool canAssign) {
    (void)canAssign;
    if (c->currentClass == NULL) {
        error(c->parser, "Can't use 'this' outside of a class");
        return;
    }

    variable(c, false);
}

static void unary(Compiler *c, bool canAssign) {
    (void)canAssign;
    TokenType operatorType = c->parser->prv.type;

    // compile the operand
    parsePrecedence(c, PREC_UNARY);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    // emit the operator instruction
    switch (operatorType) {
    case TOKEN_BANG:  emitOp(c, OP_NOT); break;
    case TOKEN_MINUS: emitOp(c, OP_NEGATE); break;
    default:          return;
    }
#pragma GCC diagnostic pop
}

static void function(Compiler *c, FunctionType type);

static void lambda(Compiler *c, bool canAssign) {
    (void)canAssign;
    function(c, TYPE_FUNCTION);
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
    advance(c->parser);
    ParseFn prefixRule = getRule(c->parser->prv.type)->prefix;
    if (prefixRule == NULL) {
        error(c->parser, "Expect expression");
        return;
    }

    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(c, canAssign);

    while (precedence <= getRule(c->parser->cur.type)->precedence) {
        advance(c->parser);
        ParseFn infixRule = getRule(c->parser->prv.type)->infix;
        infixRule(c, canAssign);

        if (canAssign && match(c, TOKEN_EQ)) {
            error(c->parser, "Invalid assignment target");
        }
    }
}

static inline void expression(Compiler *c) {
    parsePrecedence(c, PREC_ASSIGNMENT);
}

static void block(Compiler *c) {
    while (!check(c, TOKEN_RBRACE) && !check(c, TOKEN_EOF)) {
        declaration(c);
    }

    consume(c, TOKEN_RBRACE, "Expect '}' after block");
}

static void function(Compiler *c, FunctionType type) {
    (void)c;
    Compiler compiler = {0};
    initCompiler(&compiler, current, current->parser, type);
    beginScope(current);

    consume(&compiler, TOKEN_LPAREN, "Expect '(' after function name");
    if (!check(&compiler, TOKEN_RPAREN)) {
        do {
            compiler.fn->arity++;
            if (compiler.fn->arity > 255) {
                errorAtCurrent(c->parser,
                               "Can't have more than 255 parameters");
            }
            uint8_t constIdx =
                parseVariable(&compiler, "Expect parameter name");
            defineVariable(&compiler, constIdx);
        } while (match(&compiler, TOKEN_COMMA));
    }
    consume(&compiler, TOKEN_RPAREN, "Expect ')' after parameters");
    consume(&compiler, TOKEN_LBRACE, "Expect '{' before function body");
    block(&compiler);

    // don't need to call endScope as endCompiler
    // closes the scope implicitly
    ObjFn *function = endCompiler(&compiler);
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
    consume(c, TOKEN_IDENTIFIER, "Expect method name");
    uint8_t constant = identifierConst(c, &c->parser->prv, IDENT_IDENT, NULL);

    FunctionType type = TYPE_METHOD;
    if (c->parser->prv.len == 4 &&
        memcmp(c->parser->prv.start, "init", 4) == 0) {
        type = TYPE_INITIALIZER;
    }
    function(c, type);
    emitOpArg(c, OP_METHOD, constant);
}

static inline void classDecl(Compiler *c) {
    consume(c, TOKEN_IDENTIFIER, "Expect class name");
    Token className = c->parser->prv;
    IdentType type = IDENT_CLASS_LOCAL;
    uint8_t classGlobal = 0;
    if (c->scopeDepth == 0) type = IDENT_CLASS_GLOBAL;
    uint8_t nameConst = identifierConst(c, &c->parser->prv, type, &classGlobal);
    declareVariable(c);

    emitOpArg(c, OP_CLASS, nameConst);
    if (c->scopeDepth == 0) {
        defineVariable(c, classGlobal);
    } else {
        defineVariable(c, nameConst);
    }

    ClassCompiler classCompiler = {c->currentClass, false};
    c->currentClass = &classCompiler;

    if (match(c, TOKEN_LT)) {
        consume(c, TOKEN_IDENTIFIER, "Expect superclass name");
        variable(c, false);

        if (identifiersEqual(&className, &c->parser->prv)) {
            error(c->parser, "A class can't inherit from itself");
        }

        beginScope(c);
        addLocal(c, syntheticToken("super", 5));
        defineVariable(c, 0);

        namedVariable(c, className, false);
        emitOp(c, OP_INHERIT);
        classCompiler.hasSuperClass = true;
    }

    namedVariable(c, className, false);
    consume(c, TOKEN_LBRACE, "Expect '{' before class body");
    while (!check(c, TOKEN_RBRACE) && !check(c, TOKEN_EOF)) {
        method(c);
    }
    consume(c, TOKEN_RBRACE, "Expect '}' after class body");
    emitPop(c);

    if (classCompiler.hasSuperClass) endScope(c);

    c->currentClass = c->currentClass->enclosing;
}

static inline void funDecl(Compiler *c) {
    uint8_t globalIdx = parseVariable(c, "Expect function name");
    markInitialized(c);
    function(c, TYPE_FUNCTION);
    defineVariable(c, globalIdx);
}

static void varDecl(Compiler *c) {
    uint8_t globalIdx = parseVariable(c, "Expect variable name");

    if (match(c, TOKEN_EQ)) {
        expression(c);
    } else {
        emitOp(c, OP_NIL);
    }
    consume(c, TOKEN_SEMICOLON, "Expect ';' after variable declaraion");

    defineVariable(c, globalIdx);
}

static void expressionStmt(Compiler *c) {
    expression(c);
    consume(c, TOKEN_SEMICOLON, "Expect ';' after expression");
    emitPop(c);
}

// this compiles a `for (var ix, i in iterable) { ... }`
// and reports if it successfully managed to compile it
static inline bool forIterStmt(Compiler *c) {
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
    if (!match(c, TOKEN_VAR)) return false;
    if (!match(c, TOKEN_IDENTIFIER)) return false;

    // remember the name incase it is a forIterStmt
    // we call it first name as if we are in a `for (var ix, i in iter) ...`
    // then this one will be the index and not the item
    Token first = c->parser->prv;

    // we still don't know if it is a forIterStmt but the odds are slightly
    // higher, but we still need to see either a `,` or an `in` token to confirm
    bool isIndexAndItem = false;
    if (match(c, TOKEN_COMMA) || match(c, TOKEN_IN)) {
        isIndexAndItem = c->parser->prv.type == TOKEN_COMMA;
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
        consume(c, TOKEN_IDENTIFIER,
                "Expected a second variable name after ',' in the for loop");
        // save the 2nd variable as we need it
        // correct it as we now have the 2nd variable
        Token tmp = first;
        first = c->parser->prv;
        second = tmp;
        // the parser state is now:
        // for (var ix, i in iter) ...
        //                ^
        // we still need to consume the `in` token
        consume(c, TOKEN_IN, "Expect 'in' after variable names in for loop");
    }

    // the 2 possible types are now synced up
    // so now we need to construct the iterator
    // we use the builtin Iter class this will then construct
    // the iterator object once the expression has been evaluated
    emitOpArg(c, OP_GET_GLOBAL, syntheticIdentifierConst(c, "Iter", 4, true));

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
        error(c->parser,
              "Too many local variables in scope. (Not enough space for "
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
    consume(c, TOKEN_RPAREN, "Expect ')' after loop expression");

    Loop loop = {0};
    initLoop(c, &loop);

    // advance the iterator
    emitOpArg(c, OP_GET_LOCAL, itSlot);
    emitOp2Args(c, OP_INVOKE, syntheticIdentifierConst(c, "next", 4, false), 0);

    // test the condition
    loop.end = emitJump(c, OP_JUMP_IF_FALSE);
    emitPop(c);

    // update i
    emitOpArg(c, OP_GET_LOCAL, itSlot);
    emitOp2Args(c, OP_INVOKE, syntheticIdentifierConst(c, "value", 5, false),
                0);
    emitOpArg(c, OP_SET_LOCAL, iSlot);
    emitPop(c);

    // update ix if we need to
    if (isIndexAndItem) {
        emitOpArg(c, OP_GET_LOCAL, itSlot);
        emitOp2Args(c, OP_INVOKE,
                    syntheticIdentifierConst(c, "index", 5, false), 0);
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

static inline void forStmt(Compiler *c) {
    beginScope(c);
    consume(c, TOKEN_LPAREN, "Expect '(' after 'for'");

    // there are 2 types of for loops
    // 1) for (var i = 0; i < n; i = i +1) block
    // 2) for (var ix, i in iterable) block
    // we need to see which one it is and compile it correctly
    // to do this we have to save the state of the lexer and the parser
    // so that we can revert back to the current position if wasn't a
    // for loop over an iterable

    // the parser has the lexer in it so saving the parser also saves the lexer
    Parser savedParser = *c->parser;
    // if it was a for loop over an iterable it will call endScope
    // and we are done compiling the for loop so we just return
    if (forIterStmt(c)) return;

    // otherwise we have to restore the state of the lexer and parser
    // and try to compile a normal for loop
    *c->parser = savedParser;

    if (match(c, TOKEN_SEMICOLON)) {
        // no initializer
    } else if (match(c, TOKEN_VAR)) {
        varDecl(c);
    } else {
        expressionStmt(c);
    }

    Loop loop = {0};
    initLoop(c, &loop);
    if (!match(c, TOKEN_SEMICOLON)) {
        expression(c);
        consume(c, TOKEN_SEMICOLON, "Expect ';' after loop condition");

        // jmp out of the loop if cond is false
        loop.end = emitJump(c, OP_JUMP_IF_FALSE);
        emitPop(c); // cond
    }

    if (!match(c, TOKEN_RPAREN)) {
        int bodyJmpIdx = emitJump(c, OP_JUMP);
        int incrStartIdx = curChunk(c)->cnt;
        expression(c);
        emitPop(c);
        consume(c, TOKEN_RPAREN, "Expect ')' after clauses");

        emitLoop(c, loop.start);
        loop.start = incrStartIdx;
        patchJump(c, bodyJmpIdx);
    }

    loop.body = curChunk(c)->cnt;
    statement(c);
    endLoop(c, loop.start);
    endScope(c);
}

static inline void ifStmt(Compiler *c) {
    consume(c, TOKEN_LPAREN, "Expect '(' after if");
    expression(c);
    consume(c, TOKEN_RPAREN, "Expect ')' after condition");

    int thenJumpIdx = emitJump(c, OP_JUMP_IF_FALSE);
    emitPop(c); // true branch pop
    statement(c);

    int elseJumpIdx = emitJump(c, OP_JUMP);
    patchJump(c, thenJumpIdx);
    emitPop(c); // false branch pop

    if (match(c, TOKEN_ELSE)) statement(c);
    patchJump(c, elseJumpIdx);
}

static inline void printStmt(Compiler *c) {
    expression(c);
    consume(c, TOKEN_SEMICOLON, "Expect ';' after value");
    emitOp(c, OP_PRINT);
}

static inline void returnStmt(Compiler *c) {
    if (c->type == TYPE_SCRIPT) {
        error(c->parser, "Can't return from top-level code");
    }

    if (match(c, TOKEN_SEMICOLON)) {
        emitReturn(c);
    } else {
        if (c->type == TYPE_INITIALIZER) {
            error(c->parser, "Can't return a value from an initializer");
        }

        expression(c);
        consume(c, TOKEN_SEMICOLON, "Expect ';' after return value");
        emitOp(c, OP_RETURN);
    }
}

static inline void whileStmt(Compiler *c) {
    Loop loop = {0};
    initLoop(c, &loop);

    consume(c, TOKEN_LPAREN, "Expect '(' after 'while'");
    expression(c);
    consume(c, TOKEN_RPAREN, "Expect ')' after condition");

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

        advance(parser);
    }
}

static void declaration(Compiler *c) {
    if (match(c, TOKEN_CLASS)) {
        classDecl(c);
    } else if (match(c, TOKEN_FUN)) {
        funDecl(c);
    } else if (match(c, TOKEN_VAR)) {
        varDecl(c);
    } else {
        statement(c);
    }

    if (c->parser->panicMode) synchronize(c->parser);
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
    if (match(c, TOKEN_PRINT)) {
        printStmt(c);
    } else if (match(c, TOKEN_FOR)) {
        forStmt(c);
    } else if (match(c, TOKEN_IF)) {
        ifStmt(c);
    } else if (match(c, TOKEN_RETURN)) {
        returnStmt(c);
    } else if (match(c, TOKEN_WHILE)) {
        whileStmt(c);
    } else if (match(c, TOKEN_LBRACE)) {
        beginScope(c);
        block(c);
        endScope(c);
    } else if (match(c, TOKEN_BREAK)) {
        if (c->loop == NULL) {
            error(c->parser, "Can't use 'break' outside of loops");
            return;
        }

        consume(c, TOKEN_SEMICOLON, "Expected ';' after break");

        // discard any locals made in the loop
        discardLocals(c, c->loop->scopeDepth + 1);

        emitJump(c, OP_NOP);
    } else if (match(c, TOKEN_CONTINUE)) {
        if (c->loop == NULL) {
            error(c->parser, "Can't use 'continue' outside of loops");
            return;
        }

        consume(c, TOKEN_SEMICOLON, "Expected ';' after continue");

        // discard any locals made in the loop
        discardLocals(c, c->loop->scopeDepth + 1);

        // jump to top of the current innermost loop
        emitLoop(c, c->loop->start);
    } else {
        expressionStmt(c);
    }
}

ObjFn *compile(VM *vm_, const char *source) {
    vm = vm_;
    Parser parser = {0};
    initParser(&parser, source);
    Compiler compiler = {0};
    initCompiler(&compiler, current, &parser, TYPE_SCRIPT);

    advance(&parser);

    while (!match(current, TOKEN_EOF)) {
        declaration(current);
    }

    ObjFn *function = endCompiler(current);
    vm = NULL;
    return parser.hadError ? NULL : function;
}

void markCompilerRoots(VM *vm) {
    Compiler *compiler = current;
    while (compiler != NULL) {
        markObject(vm, (Obj *)compiler->fn);
        markTable(vm, &compiler->constantsTable);
        compiler = compiler->enclosing;
    }
}
