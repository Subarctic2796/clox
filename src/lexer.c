#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "lexer.h"

void initLexer(Lexer *lexer, const char *source) {
    lexer->start = source;
    lexer->cur = source;
    lexer->line = 1;
}

static inline bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

static inline bool isAtEnd(Lexer *l) { return *l->cur == '\0'; }
static inline char advance(Lexer *l) { return *(l->cur++); }
static inline char peek(Lexer *l) { return *l->cur; }
static inline char peekNext(Lexer *l) { return isAtEnd(l) ? '\0' : l->cur[1]; }

static inline bool match(Lexer *l, char expected) {
    if (isAtEnd(l)) return false;
    if (*l->cur != expected) return false;
    l->cur++;
    return true;
}

static inline Token makeToken(Lexer *l, TokenType type) {
    return (Token){
        .type = type,
        .start = l->start,
        .len = (int)(l->cur - l->start),
        .line = l->line,
    };
}

static inline Token matchMakeToken(Lexer *l, char expected, TokenType t1,
                                   TokenType t2) {
    return makeToken(l, match(l, expected) ? t1 : t2);
}

static inline Token errorToken(Lexer *l, const char *msg) {
    return (Token){
        .type = TOKEN_ERROR,
        .start = msg,
        .len = (int)strnlen(msg, 1024),
        .line = l->line,
    };
}

static inline Token skipWhiteSpace(Lexer *l) {
    for (;;) {
        char c = peek(l);
        switch (c) {
        case ' ':
        case '\r':
        case '\t': advance(l); break;
        case '\n':
            l->line++;
            advance(l);
            break;
        case '/':
            if (peekNext(l) == '/') {
                while (peek(l) != '\n' && !isAtEnd(l)) {
                    advance(l);
                }
            } else if (peekNext(l) == '*') {
                advance(l);
                int nesting = 1;
                while (nesting > 0) {
                    if (isAtEnd(l)) {
                        return errorToken(l, "Unterminated block comment");
                    }

                    if (peek(l) == '/' && peekNext(l) == '*') {
                        advance(l);
                        advance(l);
                        nesting++;
                        continue;
                    }

                    if (peek(l) == '*' && peekNext(l) == '/') {
                        advance(l);
                        advance(l);
                        nesting--;
                        continue;
                    }

                    advance(l);
                }
            } else {
                return (Token){0};
            }
            break;
        default: return (Token){0};
        }
    }
}

static inline TokenType identifierType(Lexer *l) {
    int len = l->cur - l->start;
    if ((len < 1) || (len > 8)) return TOKEN_IDENTIFIER;

    typedef struct {
        TokenType type;
        const char *chars;
        int len;
    } Keyword;

    static const Keyword KEYWORDS[] = {
        {TOKEN_IF, "if", 2},
        {TOKEN_IN, "in", 2},
        {TOKEN_OR, "or", 2},
        {TOKEN_AND, "and", 3},
        {TOKEN_FOR, "for", 3},
        {TOKEN_FUN, "fun", 3},
        {TOKEN_NIL, "nil", 3},
        {TOKEN_VAR, "var", 3},
        {TOKEN_ELSE, "else", 4},
        {TOKEN_THIS, "this", 4},
        {TOKEN_TRUE, "true", 4},
        {TOKEN_BREAK, "break", 5},
        {TOKEN_CLASS, "class", 5},
        {TOKEN_FALSE, "false", 5},
        {TOKEN_PRINT, "print", 5},
        {TOKEN_SUPER, "super", 5},
        {TOKEN_WHILE, "while", 5},
        {TOKEN_RETURN, "return", 6},
        {TOKEN_CONTINUE, "continue", 8},
    };
#define NUM_KEYWORDS 19
    static_assert((sizeof KEYWORDS / sizeof KEYWORDS[0]) == NUM_KEYWORDS,
                  "number of keywords changed");

    for (size_t i = 0; i < NUM_KEYWORDS; i++) {
        Keyword kw = KEYWORDS[i];
        if (len == kw.len && memcmp(l->start, kw.chars, len) == 0) {
            return kw.type;
        }
    }
    return TOKEN_IDENTIFIER;
}

static inline Token makeIdent(Lexer *l) {
    while (isAlpha(peek(l)) || isDigit(peek(l))) {
        advance(l);
    }
    return makeToken(l, identifierType(l));
}

static inline Token makeNumber(Lexer *l) {
    while (isDigit(peek(l))) {
        advance(l);
    }

    // look for fractional part
    if (peek(l) == '.' && isDigit(peekNext(l))) {
        // consume '.'
        advance(l);

        while (isDigit(peek(l))) {
            advance(l);
        }
    }
    return makeToken(l, TOKEN_NUMBER);
}

static inline Token makeString(Lexer *l) {
    while (peek(l) != '"' && !isAtEnd(l)) {
        if (peek(l) == '\n') l->line++;
        advance(l);
    }
    if (isAtEnd(l)) return errorToken(l, "Unterminated string");
    // the closing quote
    advance(l);
    return makeToken(l, TOKEN_STRING);
}

Token scanToken(Lexer *lexer) {
    Token err = skipWhiteSpace(lexer);
    if (err.type == TOKEN_ERROR) return err;

    lexer->start = lexer->cur;

    if (isAtEnd(lexer)) return makeToken(lexer, TOKEN_EOF);

    char c = advance(lexer);
    if (isAlpha(c)) return makeIdent(lexer);
    if (isDigit(c)) return makeNumber(lexer);

    switch (c) {
    case '(': return makeToken(lexer, TOKEN_LPAREN);
    case ')': return makeToken(lexer, TOKEN_RPAREN);
    case '{': return makeToken(lexer, TOKEN_LBRACE);
    case '}': return makeToken(lexer, TOKEN_RBRACE);
    case '[': return makeToken(lexer, TOKEN_LSQR);
    case ']': return makeToken(lexer, TOKEN_RSQR);
    case ';': return makeToken(lexer, TOKEN_SEMICOLON);
    case ':': return makeToken(lexer, TOKEN_COLON);
    case ',': return makeToken(lexer, TOKEN_COMMA);
    case '.': return makeToken(lexer, TOKEN_DOT);
    case '-': return makeToken(lexer, TOKEN_MINUS);
    case '/': return makeToken(lexer, TOKEN_SLASH);
    case '*': return makeToken(lexer, TOKEN_STAR);
    case '+': return makeToken(lexer, TOKEN_PLUS);
    case '%': return makeToken(lexer, TOKEN_PERCENT);
    case '!': return matchMakeToken(lexer, '=', TOKEN_NEQ, TOKEN_BANG);
    case '=': return matchMakeToken(lexer, '=', TOKEN_EQEQ, TOKEN_EQ);
    case '<': return matchMakeToken(lexer, '=', TOKEN_LTEQ, TOKEN_LT);
    case '>': return matchMakeToken(lexer, '=', TOKEN_GTEQ, TOKEN_GT);
    case '"': return makeString(lexer);
    }

    return errorToken(lexer, "Unexpected character");
}
