/*
 * lexer.h — texto-fonte -> tokens (doc 07 §2).
 *
 * O lexer não conhece o contexto do assembler: erros léxicos viram um
 * token TK_ERROR com mensagem e coluna, e quem consome decide reportar.
 * Strings e caracteres são validados aqui (escapes), mas a decodificação
 * dos bytes fica para ksas_lex_unescape, chamada só quando necessário.
 */
#ifndef KSAS_LEXER_H
#define KSAS_LEXER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum ksas_tok {
    TK_EOF,
    TK_NEWLINE,
    TK_IDENT,      /* label, mnemônico, símbolo (pode conter '.') */
    TK_DIRECTIVE,  /* ".nome": text/len sem o ponto */
    TK_REG,        /* value = índice 0..31 */
    TK_NUM,        /* value = valor em 32 bits */
    TK_CHAR,       /* 'c': value = byte */
    TK_STR,        /* text/len = conteúdo entre as aspas, ainda escapado */
    TK_ERROR,      /* err = mensagem */
    TK_COMMA, TK_COLON, TK_LBRACK, TK_RBRACK, TK_LPAREN, TK_RPAREN,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_SHL, TK_SHR,
    TK_EQUALS,
    TK_DOT         /* "." sozinho: endereço corrente */
} ksas_tok;

typedef struct ksas_token {
    ksas_tok    kind;
    const char *text;
    uint32_t    len;
    uint32_t    col;     /* 1-based */
    uint32_t    value;
    const char *err;
} ksas_token;

typedef struct ksas_lexer {
    const char *p;           /* próximo caractere */
    const char *line_start;  /* início da linha corrente */
    uint32_t    line;        /* 1-based */
} ksas_lexer;

/* Lexer + token corrente (ksas_cur_peek dá o token seguinte sem consumir). */
typedef struct ksas_cursor {
    ksas_lexer lx;
    ksas_token tok;
} ksas_cursor;

void ksas_lex_init(ksas_lexer *lx, const char *src);
void ksas_lex_next(ksas_lexer *lx, ksas_token *t);

void ksas_cur_init(ksas_cursor *cur, const char *src);
void ksas_cur_advance(ksas_cursor *cur);
void ksas_cur_peek(const ksas_cursor *cur, ksas_token *out);

/* Comprimento da linha que começa em p, sem "\r\n". */
uint32_t ksas_lex_line_len(const char *p);

/* Bytes de um TK_STR já validado; out NULL apenas conta. */
uint32_t ksas_lex_unescape(const ksas_token *t, uint8_t *out);

/* Nome de registrador (zr, r0..r31, fp, sp, a0..a3, t0..t10, s0..s13),
 * sem diferenciar maiúsculas. */
bool ksas_reg_parse(const char *s, uint32_t n, uint8_t *idx);

/* s[0..n) == lit, sem diferenciar maiúsculas. */
bool ksas_name_eq(const char *s, uint32_t n, const char *lit);

#endif /* KSAS_LEXER_H */
