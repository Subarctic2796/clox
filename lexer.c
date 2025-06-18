#include "lexer.h"
#include <stdbool.h>
#include <string.h>

typedef struct {
  const char *start;
  const char *cur;
  size_t line;
} Lexer;

Lexer lexer;

void initLexer(const char *source) {
  lexer.start = source;
  lexer.cur = source;
  lexer.line = 1;
}

static inline bool isAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
static inline bool isAtEnd(void) { return *lexer.cur == '\0'; }

static inline char advance(void) {
  lexer.cur++;
  return lexer.cur[-1];
}

static inline char peek(void) { return *lexer.cur; }
static inline char peekNext(void) {
  if (isAtEnd()) {
    return '\0';
  }
  return lexer.cur[1];
}

static inline bool match(char expected) {
  if (isAtEnd()) {
    return false;
  }
  if (*lexer.cur != expected) {
    return false;
  }
  lexer.cur++;
  return true;
}

static inline Token makeToken(TokenType type) {
  Token token = {
      .type = type,
      .start = lexer.start,
      .len = (int)(lexer.cur - lexer.start),
      .line = lexer.line,
  };
  return token;
}

static inline Token errorToken(const char *msg) {
  Token token = {
      .type = TOKEN_ERROR,
      .start = msg,
      .len = (int)strlen(msg),
      .line = lexer.line,
  };
  return token;
}

static inline void skipWhiteSpace(void) {
  for (;;) {
    char c = peek();
    switch (c) {
    case ' ':
    case '\r':
    case '\t':
      advance();
      break;
    case '\n':
      lexer.line++;
      advance();
      break;
    case '/':
      if (peekNext() == '/') {
        while (peek() != '\n' && !isAtEnd()) {
          advance();
        }
      } else {
        return;
      }
      break;
    default:
      return;
    }
  }
}

static inline TokenType checkKeyword(int start, int length, const char *rest,
                                     TokenType type) {
  if (lexer.cur - lexer.start == start + length &&
      memcmp(lexer.start + start, rest, length) == 0) {
    return type;
  }
  return TOKEN_IDENTIFIER;
}

static inline TokenType identifierType(void) {
  switch (lexer.start[0]) {
  case 'a':
    return checkKeyword(1, 2, "nd", TOKEN_AND);
  case 'c':
    return checkKeyword(1, 4, "lass", TOKEN_CLASS);
  case 'e':
    return checkKeyword(1, 3, "lse", TOKEN_ELSE);
  case 'f':
    if (lexer.cur - lexer.start > 1) {
      switch (lexer.start[1]) {
      case 'a':
        return checkKeyword(2, 3, "lse", TOKEN_FALSE);
      case 'o':
        return checkKeyword(2, 1, "r", TOKEN_FOR);
      case 'u':
        return checkKeyword(2, 1, "n", TOKEN_FUN);
      }
    }
    break;
  case 'i':
    return checkKeyword(1, 1, "f", TOKEN_IF);
  case 'n':
    return checkKeyword(1, 2, "il", TOKEN_NIL);
  case 'o':
    return checkKeyword(1, 1, "r", TOKEN_OR);
  case 'p':
    return checkKeyword(1, 4, "rint", TOKEN_PRINT);
  case 'r':
    return checkKeyword(1, 5, "eturn", TOKEN_RETURN);
  case 's':
    return checkKeyword(1, 4, "uper", TOKEN_SUPER);
  case 't':
    if (lexer.cur - lexer.start > 1) {
      switch (lexer.start[1]) {
      case 'h':
        return checkKeyword(2, 2, "is", TOKEN_THIS);
      case 'r':
        return checkKeyword(2, 2, "ue", TOKEN_TRUE);
      }
    }
    break;
  case 'v':
    return checkKeyword(1, 2, "ar", TOKEN_VAR);
  case 'w':
    return checkKeyword(1, 4, "hile", TOKEN_WHILE);
  }
  return TOKEN_IDENTIFIER;
}

static inline Token makeIdent(void) {
  while (isAlpha(peek()) || isDigit(peek())) {
    advance();
  }
  return makeToken(identifierType());
}

static inline Token makeNumber(void) {
  while (isDigit(peek())) {
    advance();
  }

  // look for fractional part
  if (peek() == '.' && isDigit(peekNext())) {
    // consume '.'
    advance();

    while (isDigit(peek())) {
      advance();
    }
  }
  return makeToken(TOKEN_NUMBER);
}

static inline Token makeString(void) {
  while (peek() != '"' && !isAtEnd()) {
    if (peek() == '\n') {
      lexer.line++;
    }
    advance();
  }
  if (isAtEnd()) {
    return errorToken("Unterminated string");
  }
  // the closing quote
  advance();
  return makeToken(TOKEN_STRING);
}

Token scanToken(void) {
  skipWhiteSpace();
  lexer.start = lexer.cur;

  if (isAtEnd()) {
    return makeToken(TOKEN_EOF);
  }

  char c = advance();
  if (isAlpha(c)) {
    return makeIdent();
  }
  if (isDigit(c)) {
    return makeNumber();
  }

  switch (c) {
  case '(':
    return makeToken(TOKEN_LEFT_PAREN);
  case ')':
    return makeToken(TOKEN_RIGHT_PAREN);
  case '{':
    return makeToken(TOKEN_LEFT_BRACE);
  case '}':
    return makeToken(TOKEN_RIGHT_BRACE);
  case ';':
    return makeToken(TOKEN_SEMICOLON);
  case ',':
    return makeToken(TOKEN_COMMA);
  case '.':
    return makeToken(TOKEN_DOT);
  case '-':
    return makeToken(TOKEN_MINUS);
  case '+':
    return makeToken(TOKEN_PLUS);
  case '/':
    return makeToken(TOKEN_SLASH);
  case '*':
    return makeToken(TOKEN_STAR);
  case '!':
    return makeToken(match('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
  case '=':
    return makeToken(match('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
  case '<':
    return makeToken(match('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
  case '>':
    return makeToken(match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
  case '"':
    return makeString();
  }

  return errorToken("Unexpected character");
}
