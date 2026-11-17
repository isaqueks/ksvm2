/*
 * lexer.c — tokenização (ver lexer.h).
 */
#include <ctype.h>
#include <string.h>

#include "lexer.h"

static bool is_ident_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

/* '.' é aceito dentro de nomes para permitir "b.eq" e labels "a.b". */
static bool is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '.';
}

static int lower(char c)
{
    return tolower((unsigned char)c);
}

bool ksas_name_eq(const char *s, uint32_t n, const char *lit)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (lit[i] == '\0' || lower(s[i]) != lower(lit[i])) {
            return false;
        }
    }
    return lit[n] == '\0';
}

void ksas_lex_init(ksas_lexer *lx, const char *src)
{
    lx->p = src;
    lx->line_start = src;
    lx->line = 1;
}

uint32_t ksas_lex_line_len(const char *p)
{
    uint32_t n = 0;

    while (p[n] != '\0' && p[n] != '\n') {
        n++;
    }
    if (n > 0u && p[n - 1u] == '\r') {
        n--;
    }
    return n;
}

static uint32_t col_of(const ksas_lexer *lx, const char *at)
{
    return (uint32_t)(at - lx->line_start) + 1u;
}

static void set_tok(ksas_token *t, ksas_tok kind, const char *text, uint32_t len)
{
    t->kind = kind;
    t->text = text;
    t->len = len;
    t->value = 0;
    t->err = NULL;
}

static void set_error(const ksas_lexer *lx, ksas_token *t, const char *at, const char *msg)
{
    set_tok(t, TK_ERROR, at, 1);
    t->col = col_of(lx, at);
    t->err = msg;
}

/* ---- Registradores ---------------------------------------------------- */

/* Decimal de 1 ou 2 dígitos sem zero à esquerda ("r01" não é registrador). */
static bool small_number(const char *s, uint32_t n, unsigned *v)
{
    if (n == 0u || n > 2u || (n == 2u && s[0] == '0')) {
        return false;
    }
    *v = 0;
    while (n-- > 0u) {
        if (!isdigit((unsigned char)*s)) {
            return false;
        }
        *v = *v * 10u + (unsigned)(*s - '0');
        s++;
    }
    return true;
}

bool ksas_reg_parse(const char *s, uint32_t n, uint8_t *idx)
{
    /* Prefixo -> (primeiro índice, quantidade): aliases da ABI (doc 07). */
    static const struct { char prefix; unsigned base, count; } banks[] = {
        { 'r', 0u, 32u }, { 'a', 1u, 4u }, { 't', 5u, 11u }, { 's', 16u, 14u }
    };
    unsigned v, i;

    if (ksas_name_eq(s, n, "zr")) { *idx = 0;  return true; }
    if (ksas_name_eq(s, n, "fp")) { *idx = 30; return true; }
    if (ksas_name_eq(s, n, "sp")) { *idx = 31; return true; }
    if (n < 2u || !small_number(s + 1, n - 1u, &v)) {
        return false;
    }
    for (i = 0; i < sizeof banks / sizeof banks[0]; i++) {
        if (lower(s[0]) == banks[i].prefix && v < banks[i].count) {
            *idx = (uint8_t)(banks[i].base + v);
            return true;
        }
    }
    return false;
}

/* ---- Escapes ---------------------------------------------------------- */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* *pp aponta para o caractere após '\'. Avança e devolve o byte, ou -1.
 * Fim do texto ou da linha não é consumido: o lexer não pode passar do
 * NUL nem engolir o '\n' (a contagem de linhas é feita no TK_NEWLINE). */
static int escape_value(const char **pp)
{
    const char *p = *pp;
    int hi, lo;

    if (*p == '\0' || *p == '\n' || *p == '\r') {
        return -1;
    }
    *pp = p + 1;
    switch (*p) {
    case 'n':  return '\n';
    case 't':  return '\t';
    case 'r':  return '\r';
    case '0':  return 0;
    case '\\': return '\\';
    case '"':  return '"';
    case '\'': return '\'';
    case 'x':
        hi = hex_digit(p[1]);
        lo = (hi >= 0) ? hex_digit(p[2]) : -1;
        if (lo < 0) {
            return -1;
        }
        *pp = p + 3;
        return hi * 16 + lo;
    default:
        return -1;
    }
}

uint32_t ksas_lex_unescape(const ksas_token *t, uint8_t *out)
{
    const char *p = t->text, *end = t->text + t->len;
    uint32_t n = 0;

    while (p < end) {
        int c;
        if (*p == '\\') {
            p++;
            c = escape_value(&p);   /* já validado pelo lexer: c >= 0 */
        } else {
            c = (unsigned char)*p++;
        }
        if (out != NULL) {
            out[n] = (uint8_t)c;
        }
        n++;
    }
    return n;
}

/* ---- Literais --------------------------------------------------------- */

static unsigned digit_value(char c)
{
    int h = hex_digit(c);
    return (h < 0) ? 99u : (unsigned)h;
}

/* Pula o resto de um "número" malformado para não gerar erros em cascata. */
static void bad_number(ksas_lexer *lx, ksas_token *t, const char *start, const char *msg)
{
    set_error(lx, t, start, msg);
    while (is_ident_char(*lx->p)) {
        lx->p++;
    }
}

/* Decimal, 0x hex, 0b binário; '_' separa dígitos. Erro se passar de
 * 32 bits ou se vierem letras/dígitos inválidos colados ao número. */
static void lex_number(ksas_lexer *lx, ksas_token *t, const char *start)
{
    const char *p = start;
    unsigned base = 10u, d;
    uint32_t v = 0;
    bool any = false;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16u;
        p += 2;
    } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        base = 2u;
        p += 2;
    }
    for (;; p++) {
        if (*p == '_') {
            continue;
        }
        d = digit_value(*p);
        if (d >= base) {
            break;
        }
        if (v > (UINT32_C(0xFFFFFFFF) - d) / base) {
            lx->p = p;
            bad_number(lx, t, start, "numero nao cabe em 32 bits");
            return;
        }
        v = v * base + d;
        any = true;
    }
    lx->p = p;
    if (!any || is_ident_char(*p)) {
        bad_number(lx, t, start, "numero malformado");
        return;
    }
    set_tok(t, TK_NUM, start, (uint32_t)(p - start));
    t->value = v;
}

static void lex_char(ksas_lexer *lx, ksas_token *t, const char *start)
{
    const char *p = start + 1;
    int c;

    if (*p == '\\') {
        p++;
        c = escape_value(&p);
        if (c < 0) {
            set_error(lx, t, start + 1, "escape invalido");
            lx->p = p;
            return;
        }
    } else if (*p == '\0' || *p == '\n' || *p == '\'') {
        set_error(lx, t, start, "caractere literal vazio ou sem fechamento");
        lx->p = (*p == '\'') ? p + 1 : p;
        return;
    } else {
        c = (unsigned char)*p++;
    }
    if (*p != '\'') {
        set_error(lx, t, start, "caractere literal sem ' de fechamento");
        lx->p = p;
        return;
    }
    lx->p = p + 1;
    set_tok(t, TK_CHAR, start, (uint32_t)(lx->p - start));
    t->value = (uint32_t)c;
}

static void lex_string(ksas_lexer *lx, ksas_token *t, const char *start)
{
    const char *p = start + 1;

    while (*p != '"') {
        if (*p == '\0' || *p == '\n' || *p == '\r') {
            set_error(lx, t, start, "string sem aspas de fechamento");
            lx->p = p;
            return;
        }
        if (*p == '\\') {
            const char *esc = p;
            p++;
            if (escape_value(&p) < 0) {
                set_error(lx, t, esc,
                          "escape invalido (use \\n \\t \\r \\0 \\\\ \\\" \\' \\xHH)");
                /* Pula até o fim da string para não reinterpretar o resto. */
                while (*p != '\0' && *p != '\n' && *p != '"') {
                    p++;
                }
                lx->p = (*p == '"') ? p + 1 : p;
                return;
            }
        } else {
            p++;
        }
    }
    set_tok(t, TK_STR, start + 1, (uint32_t)(p - start - 1));
    lx->p = p + 1;
}

static void lex_word(ksas_lexer *lx, ksas_token *t, const char *start, ksas_tok kind)
{
    const char *p = start + 1;
    uint8_t reg;

    while (is_ident_char(*p)) {
        p++;
    }
    lx->p = p;
    if (kind == TK_DIRECTIVE) {
        set_tok(t, TK_DIRECTIVE, start + 1, (uint32_t)(p - start - 1));
        return;
    }
    set_tok(t, TK_IDENT, start, (uint32_t)(p - start));
    if (ksas_reg_parse(start, t->len, &reg)) {
        t->kind = TK_REG;
        t->value = reg;
    }
}

/* ---- Pontuação -------------------------------------------------------- */

static bool lex_punct(ksas_lexer *lx, ksas_token *t, const char *p)
{
    static const struct { char c; ksas_tok kind; } single[] = {
        { ',', TK_COMMA },  { ':', TK_COLON },  { '[', TK_LBRACK },  { ']', TK_RBRACK },
        { '(', TK_LPAREN }, { ')', TK_RPAREN }, { '+', TK_PLUS },    { '-', TK_MINUS },
        { '*', TK_STAR },   { '/', TK_SLASH },  { '%', TK_PERCENT }, { '&', TK_AMP },
        { '|', TK_PIPE },   { '^', TK_CARET },  { '~', TK_TILDE },   { '=', TK_EQUALS }
    };
    size_t i;

    if ((p[0] == '<' && p[1] == '<') || (p[0] == '>' && p[1] == '>')) {
        set_tok(t, p[0] == '<' ? TK_SHL : TK_SHR, p, 2);
        lx->p = p + 2;
        return true;
    }
    for (i = 0; i < sizeof single / sizeof single[0]; i++) {
        if (*p == single[i].c) {
            set_tok(t, single[i].kind, p, 1);
            lx->p = p + 1;
            return true;
        }
    }
    return false;
}

/* Pula espaços e um comentário (';', '#' ou "//") até o fim da linha. */
static const char *skip_blank(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\f' || *p == '\v') {
        p++;
    }
    if (*p == ';' || *p == '#' || (p[0] == '/' && p[1] == '/')) {
        while (*p != '\0' && *p != '\n') {
            p++;
        }
    }
    return p;
}

void ksas_lex_next(ksas_lexer *lx, ksas_token *t)
{
    const char *p = skip_blank(lx->p);

    t->col = col_of(lx, p);
    if (*p == '\0') {
        set_tok(t, TK_EOF, p, 0);
        lx->p = p;
    } else if (*p == '\n') {
        set_tok(t, TK_NEWLINE, p, 1);
        lx->p = p + 1;
        lx->line_start = lx->p;
        lx->line++;
    } else if (isdigit((unsigned char)*p)) {
        lex_number(lx, t, p);
    } else if (is_ident_start(*p)) {
        lex_word(lx, t, p, TK_IDENT);
    } else if (*p == '.' && is_ident_start(p[1])) {
        lex_word(lx, t, p, TK_DIRECTIVE);
    } else if (*p == '.') {
        set_tok(t, TK_DOT, p, 1);
        lx->p = p + 1;
    } else if (*p == '\'') {
        lex_char(lx, t, p);
    } else if (*p == '"') {
        lex_string(lx, t, p);
    } else if (!lex_punct(lx, t, p)) {
        set_error(lx, t, p, "caractere inesperado");
        lx->p = p + 1;
    }
}

void ksas_cur_init(ksas_cursor *cur, const char *src)
{
    ksas_lex_init(&cur->lx, src);
    memset(&cur->tok, 0, sizeof cur->tok);
}

void ksas_cur_advance(ksas_cursor *cur)
{
    ksas_lex_next(&cur->lx, &cur->tok);
}

void ksas_cur_peek(const ksas_cursor *cur, ksas_token *out)
{
    ksas_lexer copy = cur->lx;
    ksas_lex_next(&copy, out);
}
