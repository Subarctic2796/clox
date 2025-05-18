#include "compiler.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "common.h"
#include "memory.h"
#include "object.h"
#include "scanner.h"
#include "value.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct {
  Token cur, prv;
  bool hadError, panicMode;
} Parser;

typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT, // =
  PREC_OR,         // or
  PREC_AND,        // and
  PREC_EQUALITY,   // == !=
  PREC_COMPARISON, // < > <= >=
  PREC_TERM,       // + -
  PREC_FACTOR,     // * /
  PREC_UNARY,      // ! -
  PREC_CALL,       // . ()
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
  bool isCaptured;
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

typedef struct Compiler {
  struct Compiler *enclosing;
  ObjFn *fn;
  FunctionType type;

  Local locals[UINT8_COUNT];
  int localCount;
  Upvalue upvalues[UINT8_COUNT];
  int scopeDepth;
} Compiler;

typedef struct ClassCompiler {
  struct ClassCompiler *enclosing;
  bool hasSuperClass;
} ClassCompiler;

Parser parser;
Compiler *current = NULL;
ClassCompiler *currentClass = NULL;

static inline Chunk *curChunk(void) { return &current->fn->chunk; }

static void errorAt(Token *token, const char *message) {
  if (parser.panicMode) {
    return;
  }
  parser.panicMode = true;
  fprintf(stderr, "[line %d] Error", token->line);

  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token->type == TOKEN_ERROR) {
    // nothing
  } else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }

  fprintf(stderr, ": %s\n", message);
  parser.hadError = true;
}

static inline void error(const char *msg) { errorAt(&parser.prv, msg); }
static inline void errorAtCurrent(const char *msg) {
  errorAt(&parser.cur, msg);
}

static inline void advance(void) {
  parser.prv = parser.cur;

  for (;;) {
    parser.cur = scanToken();
    if (parser.cur.type != TOKEN_ERROR) {
      break;
    }

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
  if (!check(type)) {
    return false;
  }
  advance();
  return true;
}

static inline void emitByte(uint8_t byte) {
  writeChunk(curChunk(), byte, parser.prv.line);
}

static inline void emitBytes(uint8_t byte1, uint8_t byte2) {
  emitByte(byte1);
  emitByte(byte2);
}

static void emitLoop(int loopStart) {
  emitByte(OP_LOOP);

  int offset = curChunk()->cnt - loopStart + 2;
  if (offset > UINT16_MAX) {
    error("Loop body too large");
  }

  emitByte((offset >> 8) & 0xff);
  emitByte(offset & 0xff);
}

static inline int emitJump(uint8_t inst) {
  emitByte(inst);
  emitByte(0xff);
  emitByte(0xff);
  return curChunk()->cnt - 2;
}

static void emitReturn(void) {
  if (current->type == TYPE_INITIALIZER) {
    emitBytes(OP_GET_LOCAL, 0);
  } else {
    emitByte(OP_NIL);
  }
  emitByte(OP_RETURN);
}

static uint8_t makeConstant(Value value) {
  int constIdx = addConst(curChunk(), value);
  if (constIdx > UINT8_MAX) {
    error("Too many constants in on chunk");
    return 0;
  }

  return (uint8_t)constIdx;
}

static inline void emitConstant(Value value) {
  emitBytes(OP_CONSTANT, makeConstant(value));
}

static void patchJump(int offset) {
  // -2 to adjust for the bytecode for the jump offset itself
  int jump = curChunk()->cnt - offset - 2;

  if (jump > UINT16_MAX) {
    error("Too much code to jump over");
  }

  curChunk()->code[offset] = (jump >> 8) & 0xff;
  curChunk()->code[offset + 1] = jump & 0xff;
}

static void initCompiler(Compiler *compiler, FunctionType type) {
  compiler->enclosing = current;
  compiler->fn = NULL;
  compiler->type = type;
  compiler->localCount = 0;
  compiler->scopeDepth = 0;
  compiler->fn = newFunction();
  current = compiler;

  if (type != TYPE_SCRIPT) {
    current->fn->name = copyString(parser.prv.start, parser.prv.length);
  }

  Local *local = &current->locals[current->localCount++];
  local->depth = 0;
  local->isCaptured = false;
  if (type != TYPE_FUNCTION) {
    local->name.start = "this";
    local->name.length = 4;
  } else {
    local->name.start = "";
    local->name.length = 0;
  }
}

static ObjFn *endCompiler(void) {
  emitReturn();
  ObjFn *fn = current->fn;

#ifdef DEBUG_PRINT_CODE
  if (!parser.hadError) {
    disassembleChunk(currentChunk(),
                     fn->name != NULL ? fn->name->chars : "<script>");
  }
#endif
  current = current->enclosing;
  return fn;
}

static inline void beginScope(void) { current->scopeDepth++; }
static void endScope(void) {
  current->scopeDepth--;
  while (current->localCount > 0 &&
         current->locals[current->localCount - 1].depth > current->scopeDepth) {
    if (current->locals[current->localCount - 1].isCaptured) {
      emitByte(OP_CLOSE_UPVALUE);
    } else {
      emitByte(OP_POP);
    }
    current->localCount--;
  }
}

static inline void expression(void);
static void statement(void);
static void declaration(void);
static inline ParseRule *getRule(TokenType type);
static void parsePrecedence(Precedence precedence);

static inline uint8_t identifierConst(Token *name) {
  return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static inline bool identifiersEqual(Token *a, Token *b) {
  if (a->length != b->length) {
    return false;
  }
  return memcmp(a->start, b->start, a->length) == 0;
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

static int addUpvalue(Compiler *compiler, uint8_t index, bool isLocal) {
  int upvalueCnt = compiler->fn->upvalueCnt;

  for (int i = 0; i < upvalueCnt; i++) {
    Upvalue *upvalue = &compiler->upvalues[i];
    if (upvalue->index == index && upvalue->isLocal == isLocal) {
      return i;
    }
  }

  if (upvalueCnt == UINT8_COUNT) {
    error("Too many closure variables in function");
    return 0;
  }

  compiler->upvalues[upvalueCnt].isLocal = isLocal;
  compiler->upvalues[upvalueCnt].index = index;
  return compiler->fn->upvalueCnt++;
}

static int resolveUpvalue(Compiler *compiler, Token *name) {
  if (compiler->enclosing == NULL) {
    return -1;
  }

  int local = resolveLocal(compiler->enclosing, name);
  if (local != -1) {
    compiler->enclosing->locals[local].isCaptured = true;
    return addUpvalue(compiler, (uint8_t)local, true);
  }

  int upvalue = resolveUpvalue(compiler->enclosing, name);
  if (upvalue != -1) {
    return addUpvalue(compiler, (uint8_t)upvalue, false);
  }

  return -1;
}

static void addLocal(Token name) {
  if (current->localCount == UINT8_COUNT) {
    error("Too many local variables in function");
    return;
  }

  Local *local = &current->locals[current->localCount++];
  local->name = name;
  local->depth = -1;
  local->isCaptured = false;
}

static void declareVariable(void) {
  // globals need names
  if (current->scopeDepth == 0) {
    return;
  }

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
  if (current->scopeDepth > 0) {
    // return dummy index instead, as locals aren't looked up by name
    return 0;
  }

  return identifierConst(&parser.prv);
}

static void markInitialized(void) {
  if (current->scopeDepth == 0) {
    return;
  }
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
  emitBytes(OP_DEFINE_GLOBAL, globalIdx);
}

static uint8_t argumentList(void) {
  uint8_t argCnt = 0;
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      expression();
      if (argCnt == 255) {
        error("Can't have more than 255 arguments");
      }
      argCnt++;
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments");
  return argCnt;
}

static void and_(bool canAssign) {
  (void)canAssign;
  int endJmpIdx = emitJump(OP_JUMP_IF_FALSE);

  emitByte(OP_POP);
  parsePrecedence(PREC_AND);

  patchJump(endJmpIdx);
}

static void binary(bool canAssign) {
  (void)canAssign;
  TokenType operatorType = parser.prv.type;
  ParseRule *rule = getRule(operatorType);
  parsePrecedence((Precedence)(rule->precedence + 1));

  switch (operatorType) {
  case TOKEN_BANG_EQUAL:
    emitBytes(OP_EQUAL, OP_NOT);
    break;
  case TOKEN_EQUAL_EQUAL:
    emitByte(OP_EQUAL);
    break;
  case TOKEN_GREATER:
    emitByte(OP_GREATER);
    break;
  case TOKEN_GREATER_EQUAL:
    emitBytes(OP_LESS, OP_NOT);
    break;
  case TOKEN_LESS:
    emitByte(OP_LESS);
    break;
  case TOKEN_LESS_EQUAL:
    emitBytes(OP_GREATER, OP_NOT);
    break;
  case TOKEN_PLUS:
    emitByte(OP_ADD);
    break;
  case TOKEN_MINUS:
    emitByte(OP_SUBTRACT);
    break;
  case TOKEN_STAR:
    emitByte(OP_MULTIPLY);
    break;
  case TOKEN_SLASH:
    emitByte(OP_DIVIDE);
    break;
  default:
    return; // Unreachable.
  }
}

static void call(bool canAssign) {
  (void)canAssign;
  uint8_t argCnt = argumentList();
  emitBytes(OP_CALL, argCnt);
}

static void dot(bool canAssign) {
  consume(TOKEN_IDENTIFIER, "Expect property name after '.'");
  uint8_t name = identifierConst(&parser.prv);

  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(OP_SET_PROPERTY, name);
  } else if (match(TOKEN_LEFT_PAREN)) {
    uint8_t argCnt = argumentList();
    emitBytes(OP_INVOKE, name);
    emitByte(argCnt);
  } else {
    emitBytes(OP_GET_PROPERTY, name);
  }
}

static void literal(bool canAssign) {
  (void)canAssign;
  switch (parser.prv.type) {
  case TOKEN_FALSE:
    emitByte(OP_FALSE);
    break;
  case TOKEN_TRUE:
    emitByte(OP_TRUE);
    break;
  case TOKEN_NIL:
    emitByte(OP_NIL);
    break;
  default:
    return;
  }
}

static void grouping(bool canAssign) {
  (void)canAssign;
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression");
}

static void number(bool canAssign) {
  (void)canAssign;
  double value = strtod(parser.prv.start, NULL);
  emitConstant(NUMBER_VAL(value));
}

static void or_(bool canAssign) {
  (void)canAssign;
  int elseJumpIdx = emitJump(OP_JUMP_IF_FALSE);
  int endJumpIdx = emitJump(OP_JUMP);

  patchJump(elseJumpIdx);
  emitByte(OP_POP);

  parsePrecedence(PREC_OR);
  patchJump(endJumpIdx);
}

static void string(bool canAssign) {
  (void)canAssign;
  Token prv = parser.prv;
  emitConstant(OBJ_VAL(copyString(prv.start + 1, prv.length - 2)));
}

static void namedVariable(Token name, bool canAssign) {
  uint8_t getOp, setOp;
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

  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(setOp, argIdx);
  } else {
    emitBytes(getOp, argIdx);
  }
}

static inline void variable(bool canAssign) {
  (void)canAssign;
  namedVariable(parser.prv, canAssign);
}

static inline Token syntheticToken(const char *txt) {
  Token token;
  token.start = txt;
  token.length = (int)strlen(txt);
  return token;
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

  namedVariable(syntheticToken("this"), false);
  if (match(TOKEN_LEFT_PAREN)) {
    uint8_t argCnt = argumentList();
    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_SUPER_INVOKE, name);
    emitByte(argCnt);
  } else {
    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_GET_SUPER, name);
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

  // emit the operator instruction
  switch (operatorType) {
  case TOKEN_BANG:
    emitByte(OP_NOT);
    break;
  case TOKEN_MINUS:
    emitByte(OP_NEGATE);
    break;
  default:
    return;
  }
}

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN] = {grouping, call, PREC_CALL},
    [TOKEN_RIGHT_PAREN] = {NULL, NULL, PREC_NONE},
    [TOKEN_LEFT_BRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_RIGHT_BRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},
    [TOKEN_DOT] = {NULL, dot, PREC_CALL},
    [TOKEN_MINUS] = {unary, binary, PREC_TERM},
    [TOKEN_PLUS] = {NULL, binary, PREC_TERM},
    [TOKEN_SEMICOLON] = {NULL, NULL, PREC_NONE},
    [TOKEN_SLASH] = {NULL, binary, PREC_FACTOR},
    [TOKEN_STAR] = {NULL, binary, PREC_FACTOR},
    [TOKEN_BANG] = {unary, NULL, PREC_NONE},
    [TOKEN_BANG_EQUAL] = {NULL, binary, PREC_EQUALITY},
    [TOKEN_EQUAL] = {NULL, NULL, PREC_NONE},
    [TOKEN_EQUAL_EQUAL] = {NULL, binary, PREC_EQUALITY},
    [TOKEN_GREATER] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LESS] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LESS_EQUAL] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_IDENTIFIER] = {variable, NULL, PREC_NONE},
    [TOKEN_STRING] = {string, NULL, PREC_NONE},
    [TOKEN_NUMBER] = {number, NULL, PREC_NONE},
    [TOKEN_AND] = {NULL, and_, PREC_AND},
    [TOKEN_CLASS] = {NULL, NULL, PREC_NONE},
    [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
    [TOKEN_FALSE] = {literal, NULL, PREC_NONE},
    [TOKEN_FOR] = {NULL, NULL, PREC_NONE},
    [TOKEN_FUN] = {NULL, NULL, PREC_NONE},
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
    [TOKEN_ERROR] = {NULL, NULL, PREC_NONE},
    [TOKEN_EOF] = {NULL, NULL, PREC_NONE},
};

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

    if (canAssign && match(TOKEN_EQUAL)) {
      error("Invalid assignmetn target");
    }
  }
}

static inline ParseRule *getRule(TokenType type) { return &rules[type]; }
static inline void expression(void) { parsePrecedence(PREC_ASSIGNMENT); }

static void block(void) {
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    declaration();
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after block");
}

static void function(FunctionType type) {
  Compiler compiler;
  initCompiler(&compiler, type);
  beginScope();

  consume(TOKEN_LEFT_PAREN, "Expect '(' after function name");
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      current->fn->arity++;
      if (current->fn->arity > 255) {
        errorAtCurrent("Can't have more thatn 255 parameters");
      }
      uint8_t constIdx = parseVariable("Expect parameter name");
      defineVariable(constIdx);
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters");
  consume(TOKEN_LEFT_BRACE, "Expect '{' before function body");
  block();

  ObjFn *function = endCompiler();
  emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function)));

  for (int i = 0; i < function->upvalueCnt; i++) {
    emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
    emitByte(compiler.upvalues[i].index);
  }
}

static void method(void) {
  consume(TOKEN_IDENTIFIER, "Expect method name");
  uint8_t constant = identifierConst(&parser.prv);

  FunctionType type = TYPE_METHOD;
  if (parser.prv.length == 4 && memcmp(parser.prv.start, "init", 4) == 0) {
    type = TYPE_INITIALIZER;
  }
  function(type);
  emitBytes(OP_METHOD, constant);
}

static void classDecl(void) {
  consume(TOKEN_IDENTIFIER, "Expect class name");
  Token className = parser.prv;
  uint8_t nameConst = identifierConst(&parser.prv);
  declareVariable();

  emitBytes(OP_CLASS, nameConst);
  defineVariable(nameConst);

  ClassCompiler classCompiler;
  classCompiler.enclosing = currentClass;
  currentClass = &classCompiler;

  if (match(TOKEN_LESS)) {
    consume(TOKEN_IDENTIFIER, "Expect superclass name");
    variable(false);

    if (identifiersEqual(&className, &parser.prv)) {
      error("A class can't inherit from itself");
    }

    beginScope();
    addLocal(syntheticToken("super"));
    defineVariable(0);

    namedVariable(className, false);
    emitByte(OP_INHERIT);
    classCompiler.hasSuperClass = true;
  }

  namedVariable(className, false);
  consume(TOKEN_LEFT_BRACE, "Expect '{' before class body");
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    method();
  }
  consume(TOKEN_RIGHT_BRACE, "Expect '}' after class body");
  emitByte(OP_POP);

  if (classCompiler.hasSuperClass) {
    endScope();
  }

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

  if (match(TOKEN_EQUAL)) {
    expression();
  } else {
    emitByte(OP_NIL);
  }
  consume(TOKEN_SEMICOLON, "Expect ';' after variable declaraion");

  defineVariable(globalIdx);
}

static void expressionStmt(void) {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression");
  emitByte(OP_POP);
}

static void forStmt(void) {
  beginScope();
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'");
  if (match(TOKEN_SEMICOLON)) {
    // no initializer
  } else if (match(TOKEN_VAR)) {
    varDecl();
  } else {
    expressionStmt();
  }

  int loopStartIdx = curChunk()->cnt;
  int exitJmpIdx = -1;
  if (!match(TOKEN_SEMICOLON)) {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after loop condition");

    // jmp out of the loop if cond is false
    exitJmpIdx = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // cond
  }

  if (!match(TOKEN_RIGHT_PAREN)) {
    int bodyJmpIdx = emitJump(OP_JUMP);
    int incrStartIdx = curChunk()->cnt;
    expression();
    emitByte(OP_POP);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after clauses");

    emitLoop(loopStartIdx);
    loopStartIdx = incrStartIdx;
    patchJump(bodyJmpIdx);
  }

  statement();
  emitLoop(loopStartIdx);

  if (exitJmpIdx != -1) {
    patchJump(exitJmpIdx);
    emitByte(OP_POP); // cond
  }

  endScope();
}

static void ifStmt(void) {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after if");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition");

  int thenJumpIdx = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP); // true branch pop
  statement();

  int elseJumpIdx = emitJump(OP_JUMP);
  patchJump(thenJumpIdx);
  emitByte(OP_POP); // false branch pop

  if (match(TOKEN_ELSE)) {
    statement();
  }
  patchJump(elseJumpIdx);
}

static void printStmt(void) {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after value");
  emitByte(OP_PRINT);
}

static void returnStmt(void) {
  if (current->type == TYPE_SCRIPT) {
    error("Can't return from top-level code");
  }

  if (match(TOKEN_SEMICOLON)) {
    emitReturn();
  } else {
    if (current->type == TYPE_INITIALIZER) {
      error("Can't return a value from an initializer");
    }

    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after return value");
    emitByte(OP_RETURN);
  }
}

static void whileStmt(void) {
  int loopStartIdx = curChunk()->cnt;
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect '(' after condition");

  int exitJmpIdx = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  statement();
  emitLoop(loopStartIdx);

  patchJump(exitJmpIdx);
  emitByte(OP_POP);
}

static void synchronize(void) {
  parser.panicMode = false;

  while (parser.cur.type != TOKEN_EOF) {
    if (parser.prv.type == TOKEN_SEMICOLON) {
      return;
    }
    switch (parser.cur.type) {
    case TOKEN_CLASS:
    case TOKEN_FUN:
    case TOKEN_VAR:
    case TOKEN_FOR:
    case TOKEN_IF:
    case TOKEN_WHILE:
    case TOKEN_PRINT:
    case TOKEN_RETURN:
      return;

    default:; // Do nothing.
    }

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

  if (parser.panicMode) {
    synchronize();
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
  } else if (match(TOKEN_LEFT_BRACE)) {
    beginScope();
    block();
    endScope();
  } else {
    expressionStmt();
  }
}

ObjFn *compile(const char *source) {
  initScanner(source);
  Compiler compiler;
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
    compiler = compiler->enclosing;
  }
}
