#ifndef INCLUDE_CLOX_SCANNER_H_
#define INCLUDE_CLOX_SCANNER_H_

typedef enum {
    // Single-character tokens.
    TOKEN_LEFT_PAREN,
    TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACE,
    TOKEN_RIGHT_BRACE,
    TOKEN_LEFT_SQR,
    TOKEN_RIGHT_SQR,
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
    TOKEN_MINUS_EQ,
    TOKEN_PLUS_EQ,
    TOKEN_SLASH_EQ,
    TOKEN_STAR_EQ,
    TOKEN_PERCENT_EQ,
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

void initLexer(const char *source);
Token scanToken(void);

#endif // INCLUDE_CLOX_SCANNER_H_
