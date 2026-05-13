#ifndef INCLUDE_CLOX_SCANNER_H_
#define INCLUDE_CLOX_SCANNER_H_

#include <stddef.h>

typedef enum {
    // Single-character tokens.
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LSQR,
    TOKEN_RSQR,
    TOKEN_COMMA,
    TOKEN_DOT,
    TOKEN_SEMICOLON,
    TOKEN_COLON,
    TOKEN_MINUS,
    TOKEN_PLUS,
    TOKEN_SLASH,
    TOKEN_STAR,
    TOKEN_PERCENT,
    // One or two character tokens.
    TOKEN_BANG,
    TOKEN_NEQ,
    TOKEN_EQ,
    TOKEN_EQEQ,
    TOKEN_GT,
    TOKEN_GTEQ,
    TOKEN_LT,
    TOKEN_LTEQ,
    TOKEN_ERCENT_EQ,
    // Literals.
    TOKEN_IDENTIFIER,
    TOKEN_STRING,
    TOKEN_NUMBER,
    // Keywords.
    TOKEN_AND,
    TOKEN_CLASS,
    TOKEN_ELSE,
    TOKEN_FALSE,
    TOKEN_FOR,
    TOKEN_FUN,
    TOKEN_IF,
    TOKEN_NIL,
    TOKEN_OR,
    TOKEN_PRINT,
    TOKEN_RETURN,
    TOKEN_SUPER,
    TOKEN_THIS,
    TOKEN_TRUE,
    TOKEN_VAR,
    TOKEN_WHILE,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_IN,

    TOKEN_ERROR,
    TOKEN_EOF,
    __TOKEN_CNT,
} TokenType;

typedef struct {
    TokenType type;
    const char *start;
    int len, line;
} Token;

typedef struct {
    const char *start;
    const char *cur;
    size_t line;
} Lexer;

void initLexer(Lexer *lexer, const char *source);
Token scanToken(Lexer *lexer);

#endif // INCLUDE_CLOX_SCANNER_H_
