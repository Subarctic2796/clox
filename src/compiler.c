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

static inline void emitByte(uint8_t byte) {
    writeChunk(curChunk(current), byte, parser.prv.line);
}

static inline void emitBytes(uint8_t byte1, uint8_t byte2) {
    emitByte(byte1);
    emitByte(byte2);
}

static inline void emitShort(int arg) {
    emitBytes((arg >> 8) & 0xff, arg & 0xff);
}

static inline void emitOp(OpCode op) { emitByte((uint8_t)op); }
static inline void emitPop() { emitOp(OP_POP); }
static inline void emitOpArg(OpCode op, uint8_t arg) { emitBytes(op, arg); }
static inline void emitOp2Args(OpCode op, int arg1, int arg2) {
    emitOp(op);
    emitBytes(arg1, arg2);
}

static void emitLoop(int loopStart) {
    emitOp(OP_LOOP);

    int offset = curChunk(current)->cnt - loopStart + 2;
    if (offset > UINT16_MAX) error("Loop body too large");

    emitShort(offset);
}

static inline int emitJump(uint8_t inst) {
    emitOp2Args(inst, 0xff, 0xff);
    return curChunk(current)->cnt - 2;
}

static void emitReturn(void) {
    // TODO: if current->type == method or function
    // and last opcode is OP_RETURN then don't emit
    // OP_NIL, and OP_RETURN
    if (current->type == TYPE_INITIALIZER) {
        emitOpArg(OP_GET_LOCAL, 0);
    } else {
        emitOp(OP_NIL);
    }
    emitOp(OP_RETURN);
}

static uint8_t makeConstant(Value value) {
    Value existing = EMPTY_VAL;
    if (tableGet(&current->constantsTable, value, &existing)) {
        return (uint8_t)AS_NUMBER(existing); // reuse existing constant
    }

    // add constant
    // make sure not collected
    if (IS_OBJ(value)) pushRoot(value);
    int constIdx = addConst(curChunk(current), value);
    // its safe so can remove it from temp roots
    if (IS_OBJ(value)) popRoot();

    if (constIdx > UINT8_MAX) {
        error("Too many constants in on chunk");
        return 0;
    }

    tableSet(&current->constantsTable, value, NUMBER_VAL(constIdx));
    return (uint8_t)constIdx;
}

static inline void emitConstant(Value value) {
    emitOpArg(OP_CONSTANT, makeConstant(value));
}

static void patchJump(int offset) {
    // -2 to adjust for the bytecode for the jump offset itself
    int jump = curChunk(current)->cnt - offset - 2;

    if (jump > UINT16_MAX) error("Too much code to jump over");

    curChunk(current)->code[offset] = (jump >> 8) & 0xff;
    curChunk(current)->code[offset + 1] = jump & 0xff;
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
    emitReturn();
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

static inline void beginScope(void) { current->scopeDepth++; }
static void endScope(void) {
    current->scopeDepth--;
    while (current->localCount > 0 &&
           current->locals[current->localCount - 1].depth >
               current->scopeDepth) {
        emitOp(current->locals[current->localCount - 1].exitOP);
        current->localCount--;
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

static inline void initLoop(Loop *loop) {
    *loop = (Loop){
        current->loop, curChunk(current)->cnt, 0, -1, current->scopeDepth,
    };
    current->loop = loop;
}

static void endLoop(int loopStart) {
    emitLoop(loopStart);
    if (current->loop->end != -1) {
        patchJump(current->loop->end);
        emitPop();
    }

    int i = current->loop->body;
    Chunk *chunk = curChunk(current);
    while (i < chunk->cnt) {
        if (chunk->code[i] == OP_NOP) {
            chunk->code[i] = OP_JUMP;
            patchJump(i + 1);
            i += 3;
        } else {
            i += 1 + getArgCount(chunk->code, chunk->constants, i);
        }
    }

    current->loop = current->loop->enclosing;
}

static inline void expression(void);
static void statement(void);
static void declaration(void);
static inline ParseRule *getRule(TokenType type);
static void parsePrecedence(Precedence precedence);

static inline uint8_t identifierConst(Token *name) {
    return makeConstant(OBJ_VAL(copyString(name->start, name->len)));
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

static int addLocal(const Token name) {
    if (current->localCount == UINT8_COUNT) {
        error("Too many local variables in function");
        return -1;
    }

    current->locals[current->localCount++] = (Local){name, -1, OP_POP};
    return current->localCount - 1;
}

static void declareVariable(void) {
    // globals need names
    if (current->scopeDepth == 0) return;

    Token *name = &parser.prv;
    for (int i = current->localCount - 1; i >= 0; i--) {
        Local *local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scopeDepth) {
            break;
        }

        if (identifiersEqual(name, &local->name)) {
            error("Already a variable with this name in the scope");
        }
    }

    addLocal(*name);
}

static uint8_t parseVariable(const char *errorMsg) {
    consume(TOKEN_IDENTIFIER, errorMsg);

    declareVariable();
    // return dummy index instead, as locals aren't looked up by name
    if (current->scopeDepth > 0) return 0;

    return identifierConst(&parser.prv);
}

static void markInitialized() {
    if (current->scopeDepth == 0) return;
    current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static void defineVariable(uint8_t globalIdx) {
    if (current->scopeDepth > 0) {
        markInitialized();
        // don't need to do anything as the initializer for
        // the local is a temporary and is therefore already on top
        // of the stack hence there is no need to do anything
        return;
    }
    emitOpArg(OP_DEFINE_GLOBAL, globalIdx);
}

static uint8_t argumentList(void) {
    uint8_t argCnt = 0;
    if (!check(TOKEN_RPAREN)) {
        do {
            expression();
            if (argCnt == 255) error("Can't have more than 255 arguments");
            argCnt++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RPAREN, "Expect ')' after arguments");
    return argCnt;
}

static void and_(bool canAssign) {
    (void)canAssign;
    int endJmpIdx = emitJump(OP_JUMP_IF_FALSE);

    emitPop();
    parsePrecedence(PREC_AND);

    patchJump(endJmpIdx);
}

static void binary(bool canAssign) {
    (void)canAssign;
    TokenType operatorType = parser.prv.type;
    ParseRule *rule = getRule(operatorType);
    parsePrecedence((Precedence)(rule->precedence + 1));

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (operatorType) {
    case TOKEN_NEQ:     emitOp(OP_NOT_EQUAL); break;
    case TOKEN_EQEQ:    emitOp(OP_EQUAL); break;
    case TOKEN_GT:      emitOp(OP_GREATER); break;
    case TOKEN_GTEQ:    emitOp(OP_GREATER_EQUAL); break;
    case TOKEN_LT:      emitOp(OP_LESS); break;
    case TOKEN_LTEQ:    emitOp(OP_LESS_EQUAL); break;
    case TOKEN_PLUS:    emitOp(OP_ADD); break;
    case TOKEN_MINUS:   emitOp(OP_SUBTRACT); break;
    case TOKEN_STAR:    emitOp(OP_MULTIPLY); break;
    case TOKEN_SLASH:   emitOp(OP_DIVIDE); break;
    case TOKEN_PERCENT: emitOp(OP_MOD); break;
    default:            return; // Unreachable.
    }
#pragma GCC diagnostic pop
}

static void call(bool canAssign) {
    (void)canAssign;
    uint8_t argCnt = argumentList();
    emitOpArg(OP_CALL, argCnt);
}

static void dot(bool canAssign) {
    consume(TOKEN_IDENTIFIER, "Expect property name after '.'");
    uint8_t name = identifierConst(&parser.prv);

    if (canAssign && match(TOKEN_EQ)) {
        expression();
        emitOpArg(OP_SET_PROPERTY, name);
    } else if (match(TOKEN_LPAREN)) {
        uint8_t argCnt = argumentList();
        emitOp2Args(OP_INVOKE, name, argCnt);
    } else {
        emitOpArg(OP_GET_PROPERTY, name);
    }
}

static void literal(bool canAssign) {
    (void)canAssign;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (parser.prv.type) {
    case TOKEN_FALSE: emitOp(OP_FALSE); break;
    case TOKEN_TRUE:  emitOp(OP_TRUE); break;
    case TOKEN_NIL:   emitOp(OP_NIL); break;
    default:          return;
    }
#pragma GCC diagnostic pop
}

static void grouping(bool canAssign) {
    (void)canAssign;
    expression();
    consume(TOKEN_RPAREN, "Expect ')' after expression");
}

static void number(bool canAssign) {
    (void)canAssign;
    double value = strtod(parser.prv.start, NULL);
    if (trunc(value) == value) {
        if (value <= UINT8_MAX) {
            emitOpArg(OP_SMALL_INT, (uint8_t)(uint64_t)value);
            return;
        }
    }
    emitConstant(NUMBER_VAL(value));
}

static void or_(bool canAssign) {
    (void)canAssign;
    int elseJumpIdx = emitJump(OP_JUMP_IF_FALSE);
    int endJumpIdx = emitJump(OP_JUMP);

    patchJump(elseJumpIdx);
    emitPop();

    parsePrecedence(PREC_OR);
    patchJump(endJumpIdx);
}

static void string(bool canAssign) {
    (void)canAssign;
    Token prv = parser.prv;
    emitConstant(OBJ_VAL(copyString(prv.start + 1, prv.len - 2)));
}

static void array(bool canAssign) {
    (void)canAssign;

    int cnt = 0;
    if (!check(TOKEN_RSQR)) {
        do {
            // trailing comma
            if (check(TOKEN_RSQR)) break;

            parsePrecedence(PREC_OR);

            if (cnt > UINT8_MAX) {
                error("Can't have more than 255 items in an array literal");
            }
            cnt++;
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RSQR, "Expect ']' after array literal");

    emitOpArg(OP_BUILD_ARRAY, cnt);
}

static void map(bool canAssign) {
    (void)canAssign;

    int cnt = 0;
    if (!check(TOKEN_RBRACE)) {
        do {
            // trailing comma
            if (check(TOKEN_RBRACE)) break;

            parsePrecedence(PREC_OR); // key
            consume(TOKEN_COLON, "Expect ':' after map key");
            parsePrecedence(PREC_OR); // value

            if (cnt > UINT8_MAX) {
                error("Can't have more than 255 items in a map literal");
            }
            cnt++;
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RBRACE, "Expect '}' after map literal");

    emitOpArg(OP_BUILD_MAP, cnt);
}

static void subscript(bool canAssign) {
    parsePrecedence(PREC_OR);
    consume(TOKEN_RSQR, "Expect ']' after index");

    if (canAssign && match(TOKEN_EQ)) {
        expression();
        emitOp(OP_SET_INDEX);
    } else {
        emitOp(OP_GET_INDEX);
    }
}

static void namedVariable(Token name, bool canAssign) {
    uint8_t getOp = OP_GET_GLOBAL, setOp = OP_SET_GLOBAL;
    int argIdx = resolveLocal(current, &name);
    if (argIdx != -1) {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
    } else if ((argIdx = resolveUpvalue(current, &name)) != -1) {
        getOp = OP_GET_UPVALUE;
        setOp = OP_SET_UPVALUE;
    } else {
        argIdx = identifierConst(&name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }

    if (canAssign && match(TOKEN_EQ)) {
        expression();
        emitOpArg(setOp, argIdx);
    } else {
        emitOpArg(getOp, argIdx);
    }
}

static inline void variable(bool canAssign) {
    namedVariable(parser.prv, canAssign);
}

static inline Token syntheticToken(const char *txt, const int len) {
    Token token = {0};
    token.start = txt;
    token.len = len;
    return token;
}

static inline uint8_t syntheticIdentifierConst(const char *txt, const int len) {
    return makeConstant(OBJ_VAL(copyString(txt, len)));
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
    uint8_t name = identifierConst(&parser.prv);

    namedVariable(syntheticToken("this", 4), false);
    if (match(TOKEN_LPAREN)) {
        uint8_t argCnt = argumentList();
        namedVariable(syntheticToken("super", 5), false);
        emitOp2Args(OP_SUPER_INVOKE, name, argCnt);
    } else {
        namedVariable(syntheticToken("super", 5), false);
        emitOpArg(OP_GET_SUPER, name);
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
    parsePrecedence(PREC_UNARY);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    // emit the operator instruction
    switch (operatorType) {
    case TOKEN_BANG:  emitOp(OP_NOT); break;
    case TOKEN_MINUS: emitOp(OP_NEGATE); break;
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

static void parsePrecedence(Precedence precedence) {
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

static inline void expression(void) { parsePrecedence(PREC_ASSIGNMENT); }

static void block(void) {
    while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
        declaration();
    }

    consume(TOKEN_RBRACE, "Expect '}' after block");
}

static void function(FunctionType type) {
    Compiler compiler = {0};
    initCompiler(&compiler, type);
    beginScope();

    consume(TOKEN_LPAREN, "Expect '(' after function name");
    if (!check(TOKEN_RPAREN)) {
        do {
            current->fn->arity++;
            if (current->fn->arity > 255) {
                errorAtCurrent("Can't have more than 255 parameters");
            }
            uint8_t constIdx = parseVariable("Expect parameter name");
            defineVariable(constIdx);
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RPAREN, "Expect ')' after parameters");
    consume(TOKEN_LBRACE, "Expect '{' before function body");
    block();

    // don't need to call endScope as endCompiler
    // closes the scope implicitly
    ObjFn *function = endCompiler();
    emitOpArg(OP_CLOSURE, makeConstant(OBJ_VAL(function)));

    for (int i = 0; i < function->upvalueCnt; i++) {
        // true is represented as uint8_t 1
        // false is represented as uint8_t 0
        // so we can just write it straight
        emitBytes(compiler.upvalues[i].isLocal, compiler.upvalues[i].index);
    }
}

static void method(void) {
    consume(TOKEN_IDENTIFIER, "Expect method name");
    uint8_t constant = identifierConst(&parser.prv);

    FunctionType type = TYPE_METHOD;
    if (parser.prv.len == 4 && memcmp(parser.prv.start, "init", 4) == 0) {
        type = TYPE_INITIALIZER;
    }
    function(type);
    emitOpArg(OP_METHOD, constant);
}

static void classDecl(void) {
    consume(TOKEN_IDENTIFIER, "Expect class name");
    Token className = parser.prv;
    uint8_t nameConst = identifierConst(&parser.prv);
    declareVariable();

    emitOpArg(OP_CLASS, nameConst);
    defineVariable(nameConst);

    ClassCompiler classCompiler = {currentClass, false};
    currentClass = &classCompiler;

    if (match(TOKEN_LT)) {
        consume(TOKEN_IDENTIFIER, "Expect superclass name");
        variable(false);

        if (identifiersEqual(&className, &parser.prv)) {
            error("A class can't inherit from itself");
        }

        beginScope();
        addLocal(syntheticToken("super", 5));
        defineVariable(0);

        namedVariable(className, false);
        emitOp(OP_INHERIT);
        classCompiler.hasSuperClass = true;
    }

    namedVariable(className, false);
    consume(TOKEN_LBRACE, "Expect '{' before class body");
    while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
        method();
    }
    consume(TOKEN_RBRACE, "Expect '}' after class body");
    emitPop();

    if (classCompiler.hasSuperClass) endScope();

    currentClass = currentClass->enclosing;
}

static void funDecl(void) {
    uint8_t globalIdx = parseVariable("Expect function name");
    markInitialized();
    function(TYPE_FUNCTION);
    defineVariable(globalIdx);
}

static void varDecl(void) {
    uint8_t globalIdx = parseVariable("Expect variable name");

    if (match(TOKEN_EQ)) {
        expression();
    } else {
        emitOp(OP_NIL);
    }
    consume(TOKEN_SEMICOLON, "Expect ';' after variable declaraion");

    defineVariable(globalIdx);
}

static void expressionStmt(void) {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after expression");
    emitPop();
}

// this compiles a `for (var ix, i in iterable) { ... }`
// and reports if it successfully managed to compile it
static bool forIterStmt() {
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
    emitOpArg(OP_GET_GLOBAL, syntheticIdentifierConst("Iter", 4));

    // so we can now compile the iterator expression and store it in a hidden
    // variable, the space in the name ensures that it won't collide with
    // user-define variables
    expression();

    // make sure to actually construct the the iterator object
    emitOpArg(OP_CALL, 1);

    // need need to make sure that there is enough space for the `it`, `i`, and
    // possibly the `ix` local variables
    int needed = isIndexAndItem ? 3 : 2;
    if (current->localCount + needed > UINT8_COUNT) {
        error("Too many local variables in scope. (Not enough space for "
              "for-loop internal variables)");
    }

    // add `it `
    int itSlot = addLocal(syntheticToken("it ", 3));
    defineVariable(0);
    // add `i` or `ix` and initialize it
    int iSlot = addLocal(first);
    defineVariable(1);
    emitOp(OP_NIL);

    // add `ix`and initialize it if we need it
    int ixSlot = -1;
    if (isIndexAndItem) {
        ixSlot = addLocal(second);
        defineVariable(1);
        emitOp(OP_NIL);
    }

    // compile the loop body
    consume(TOKEN_RPAREN, "Expect ')' after loop expression");

    Loop loop = {0};
    initLoop(&loop);

    // advance the iterator
    emitOpArg(OP_GET_LOCAL, itSlot);
    emitOp2Args(OP_INVOKE, syntheticIdentifierConst("next", 4), 0);

    // test the condition
    loop.end = emitJump(OP_JUMP_IF_FALSE);
    emitPop();

    // update i
    emitOpArg(OP_GET_LOCAL, itSlot);
    emitOp2Args(OP_INVOKE, syntheticIdentifierConst("value", 5), 0);
    emitOpArg(OP_SET_LOCAL, iSlot);
    emitPop();

    // update ix if we need to
    if (isIndexAndItem) {
        emitOpArg(OP_GET_LOCAL, itSlot);
        emitOp2Args(OP_INVOKE, syntheticIdentifierConst("index", 5), 0);
        emitOpArg(OP_SET_LOCAL, ixSlot);
        emitPop();
    }

    // compile the actual body
    loop.body = curChunk(current)->cnt;
    statement();
    endLoop(loop.start);
    endScope();
    return true;
}

static void forStmt(void) {
    beginScope();
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
    if (forIterStmt()) return;

    // otherwise we have to restore the state of the lexer and parser
    // and try to compile a normal for loop
    parser = savedParser;

    if (match(TOKEN_SEMICOLON)) {
        // no initializer
    } else if (match(TOKEN_VAR)) {
        varDecl();
    } else {
        expressionStmt();
    }

    Loop loop = {0};
    initLoop(&loop);
    if (!match(TOKEN_SEMICOLON)) {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after loop condition");

        // jmp out of the loop if cond is false
        loop.end = emitJump(OP_JUMP_IF_FALSE);
        emitPop(); // cond
    }

    if (!match(TOKEN_RPAREN)) {
        int bodyJmpIdx = emitJump(OP_JUMP);
        int incrStartIdx = curChunk(current)->cnt;
        expression();
        emitPop();
        consume(TOKEN_RPAREN, "Expect ')' after clauses");

        emitLoop(loop.start);
        loop.start = incrStartIdx;
        patchJump(bodyJmpIdx);
    }

    loop.body = curChunk(current)->cnt;
    statement();
    endLoop(loop.start);
    endScope();
}

static void ifStmt(void) {
    consume(TOKEN_LPAREN, "Expect '(' after if");
    expression();
    consume(TOKEN_RPAREN, "Expect ')' after condition");

    int thenJumpIdx = emitJump(OP_JUMP_IF_FALSE);
    emitPop(); // true branch pop
    statement();

    int elseJumpIdx = emitJump(OP_JUMP);
    patchJump(thenJumpIdx);
    emitPop(); // false branch pop

    if (match(TOKEN_ELSE)) statement();
    patchJump(elseJumpIdx);
}

static void printStmt(void) {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after value");
    emitOp(OP_PRINT);
}

static void returnStmt(void) {
    if (current->type == TYPE_SCRIPT) error("Can't return from top-level code");

    if (match(TOKEN_SEMICOLON)) {
        emitReturn();
    } else {
        if (current->type == TYPE_INITIALIZER) {
            error("Can't return a value from an initializer");
        }

        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after return value");
        emitOp(OP_RETURN);
    }
}

static void whileStmt(void) {
    Loop loop = {0};
    initLoop(&loop);

    consume(TOKEN_LPAREN, "Expect '(' after 'while'");
    expression();
    consume(TOKEN_RPAREN, "Expect ')' after condition");

    loop.end = emitJump(OP_JUMP_IF_FALSE);
    emitPop();
    loop.body = curChunk(current)->cnt;
    statement();
    endLoop(loop.start);
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

static void declaration(void) {
    if (match(TOKEN_CLASS)) {
        classDecl();
    } else if (match(TOKEN_FUN)) {
        funDecl();
    } else if (match(TOKEN_VAR)) {
        varDecl();
    } else {
        statement();
    }

    if (parser.panicMode) synchronize(&parser);
}

// discards any locals created
static inline void discardLocals(int depth) {
    int i = current->localCount - 1;
    while (i >= 0 && current->locals[i].depth > depth) {
        emitOp(current->locals[i].exitOP);
        i--;
    }
}

static void statement(void) {
    if (match(TOKEN_PRINT)) {
        printStmt();
    } else if (match(TOKEN_FOR)) {
        forStmt();
    } else if (match(TOKEN_IF)) {
        ifStmt();
    } else if (match(TOKEN_RETURN)) {
        returnStmt();
    } else if (match(TOKEN_WHILE)) {
        whileStmt();
    } else if (match(TOKEN_LBRACE)) {
        beginScope();
        block();
        endScope();
    } else if (match(TOKEN_BREAK)) {
        if (current->loop == NULL) {
            error("Can only use 'break' inside of loops");
            return;
        }

        consume(TOKEN_SEMICOLON, "Expected ';' after break");

        // discard any locals made in the loop
        discardLocals(current->loop->scopeDepth);

        emitJump(OP_NOP);
    } else if (match(TOKEN_CONTINUE)) {
        if (current->loop == NULL) {
            error("Can only use 'continue' inside of loops");
            return;
        }

        consume(TOKEN_SEMICOLON, "Expected ';' after continue");

        // discard any locals made in the loop
        discardLocals(current->loop->scopeDepth);

        // jump to top of the current innermost loop
        emitLoop(current->loop->start);
    } else {
        expressionStmt();
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
        declaration();
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
