/*
 * parser.c — linhas, labels, constantes e diretivas (ver parser.h).
 */
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "expr.h"
#include "insn.h"
#include "parser.h"
#include "symtab.h"

/* ---- Auxiliares ------------------------------------------------------- */

/*
 * Literal float do token corrente. O lexer só conhece inteiros ("1.5" vira
 * TK_ERROR, "-2e3" é TK_MINUS + erro, "inf" é TK_IDENT), então o texto cru
 * da linha a partir da coluna do token vai para o strtof do host, que
 * arredonda RNE e aceita decimal, expoente, hexadecimal ("0x1.8p1"), "inf"
 * e "nan". Depois o lexer recomeça logo após o literal. Quem chama chegou
 * aqui avançando o cursor sem reportar TK_ERROR (ksas_cur_advance).
 */
bool ksas_parse_float(ksas_ctx *c, ksas_cursor *cur, uint32_t *bits)
{
    const char *start;
    char *end;
    float f;
    uint32_t u;

    if (ksas_at_eol(cur)) {
        return ksas_perr(c, cur->tok.col, "esperado valor float");
    }
    start = cur->lx.line_start + (cur->tok.col - 1u);
    errno = 0;
    f = strtof(start, &end);
    if (end == start || isalnum((unsigned char)*end) || *end == '_' || *end == '.') {
        return ksas_perr(c, cur->tok.col, "valor float invalido");
    }
    memcpy(&u, &f, sizeof u);
    if (errno == ERANGE && (u & UINT32_C(0x7FFFFFFF)) == UINT32_C(0x7F800000)) {
        return ksas_perr(c, cur->tok.col, "valor fora do alcance de float (use inf)");
    }
    if ((u & UINT32_C(0x7FFFFFFF)) > UINT32_C(0x7F800000)) {
        u = (u & UINT32_C(0x80000000)) | UINT32_C(0x7FC00000);   /* NaN canônico */
    }
    *bits = u;
    cur->lx.p = end;
    ksas_cur_advance(cur);
    return true;
}

bool ksas_at_eol(const ksas_cursor *cur)
{
    return cur->tok.kind == TK_NEWLINE || cur->tok.kind == TK_EOF;
}

bool ksas_expect(ksas_ctx *c, ksas_cursor *cur, ksas_tok kind, const char *what)
{
    if (cur->tok.kind != kind) {
        return ksas_perr(c, cur->tok.col, "esperado %s", what);
    }
    ksas_advance(c, cur);
    return true;
}

bool ksas_expect_reg(ksas_ctx *c, ksas_cursor *cur, uint8_t *reg)
{
    if (cur->tok.kind != TK_REG) {
        return ksas_perr(c, cur->tok.col, "esperado registrador");
    }
    *reg = (uint8_t)cur->tok.value;
    ksas_advance(c, cur);
    return true;
}

bool ksas_parse_expr(ksas_ctx *c, ksas_cursor *cur, uint32_t *node)
{
    return ksas_expr_parse(c, cur, node);
}

uint32_t ksas_const_node(ksas_ctx *c, uint32_t value, uint32_t col)
{
    return ksas_node_new(c, KSAS_EN_NUM, col, value, 0);
}

uint32_t ksas_file_add(ksas_ctx *c, const char *name, char *owned_name, char *owned_text,
                       bool listed)
{
    ksas_file *f = ksas_grow(c, c->files, &c->files_cap, c->nfiles + 1u, sizeof *f,
                             KSAS_MAX_LINES, "arquivos");

    if (f == NULL) {
        free(owned_name);
        free(owned_text);
        return KSAS_NONE;
    }
    c->files = f;
    f = &c->files[c->nfiles];
    f->name = name;
    f->owned_name = owned_name;
    f->owned_text = owned_text;
    f->listed = listed;
    return c->nfiles++;
}

/* ---- Símbolos --------------------------------------------------------- */

static bool define_label(ksas_ctx *c, const ksas_token *name)
{
    uint32_t sym = ksas_sym_intern(c, name->text, name->len, c->cur_line, name->col);
    uint32_t item;
    ksas_symbol *s;

    if (sym == KSAS_NONE || !ksas_sym_define(c, sym, KSAS_SYM_LABEL, name->col)) {
        return false;
    }
    item = ksas_item_new(c, KSAS_IT_LABEL, name->col);
    if (item == KSAS_NONE) {
        return false;
    }
    c->items[item].u.sym = sym;
    s = &c->sym.syms[sym];
    s->section = c->section;
    s->item = item;
    return true;
}

/* Constante com valor "node". O item EQU dá endereço a "." na expressão. */
static bool define_equ_node(ksas_ctx *c, const ksas_token *name, uint32_t node)
{
    uint32_t sym, item;
    ksas_symbol *s;

    if (node == KSAS_NONE) {
        return false;
    }
    sym = ksas_sym_intern(c, name->text, name->len, c->cur_line, name->col);
    if (sym == KSAS_NONE || !ksas_sym_define(c, sym, KSAS_SYM_EQU, name->col)) {
        return false;
    }
    item = ksas_item_new(c, KSAS_IT_EQU, name->col);
    if (item == KSAS_NONE) {
        return false;
    }
    c->items[item].u.sym = sym;
    s = &c->sym.syms[sym];
    s->expr = node;
    s->item = item;
    return true;
}

/* NOME = expr / .equ NOME, expr. */
static bool define_equ(ksas_ctx *c, ksas_cursor *cur, const ksas_token *name)
{
    uint32_t node;

    return ksas_parse_expr(c, cur, &node) && define_equ_node(c, name, node);
}

/* ---- Diretivas -------------------------------------------------------- */

typedef bool (*dir_fn)(ksas_ctx *c, ksas_cursor *cur, uint32_t col);

static bool dir_text(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    (void)cur; (void)col;
    c->section = KSAS_SEC_TEXT;
    return true;
}

static bool dir_rodata(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    (void)cur; (void)col;
    c->section = KSAS_SEC_RODATA;
    return true;
}

static bool dir_data(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    (void)cur; (void)col;
    c->section = KSAS_SEC_DATA;
    return true;
}

static bool dir_bss(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    (void)cur; (void)col;
    c->section = KSAS_SEC_BSS;
    return true;
}

static bool dir_pic(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    (void)cur; (void)col;
    c->pic = true;
    return true;
}

static bool dir_global(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    (void)col;
    for (;;) {
        uint32_t sym;
        if (cur->tok.kind != TK_IDENT) {
            return ksas_perr(c, cur->tok.col, "esperado nome de simbolo");
        }
        sym = ksas_sym_intern(c, cur->tok.text, cur->tok.len, c->cur_line, cur->tok.col);
        if (sym == KSAS_NONE) {
            return false;
        }
        c->sym.syms[sym].global = true;
        ksas_advance(c, cur);
        if (cur->tok.kind != TK_COMMA) {
            return true;
        }
        ksas_advance(c, cur);
    }
}

static bool dir_equ(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    ksas_token name = cur->tok;

    (void)col;
    if (name.kind != TK_IDENT) {
        return ksas_perr(c, name.col, "esperado nome da constante");
    }
    ksas_advance(c, cur);
    return ksas_expect(c, cur, TK_COMMA, "','") && define_equ(c, cur, &name);
}

static bool require_initialized(ksas_ctx *c, uint32_t col)
{
    if (c->section == KSAS_SEC_BSS) {
        return ksas_perr(c, col, "dados inicializados nao sao permitidos em .bss (use .space)");
    }
    return true;
}

/* Item com os bytes de uma string (+ '\0' se zero_term). */
static bool string_item(ksas_ctx *c, const ksas_token *t, bool zero_term)
{
    uint32_t len = ksas_lex_unescape(t, NULL);
    uint32_t total = len + (zero_term ? 1u : 0u);
    uint32_t off = ksas_pool_add(c, NULL, total);
    uint32_t item;

    if (off == KSAS_NONE) {
        return false;
    }
    (void)ksas_lex_unescape(t, c->pool + off);
    item = ksas_item_new(c, KSAS_IT_BYTES, t->col);
    if (item == KSAS_NONE) {
        return false;
    }
    c->items[item].u.bytes.off = off;
    c->items[item].u.bytes.len = total;
    c->items[item].size = total;
    return true;
}

static bool append_elem(ksas_ctx *c, uint32_t node)
{
    uint32_t *e = ksas_grow(c, c->elems, &c->elems_cap, c->nelems + 1u, sizeof *e,
                            KSAS_MAX_NODES, "elementos de dados");
    if (e == NULL) {
        return false;
    }
    c->elems = e;
    c->elems[c->nelems++] = node;
    return true;
}

/* Fecha uma sequência de expressões consecutivas num item DATA. */
static bool flush_data(ksas_ctx *c, uint32_t first, uint32_t col, uint8_t width)
{
    uint32_t item;

    if (c->nelems == first) {
        return true;
    }
    item = ksas_item_new(c, KSAS_IT_DATA, col);
    if (item == KSAS_NONE) {
        return false;
    }
    c->items[item].u.data.first = first;
    c->items[item].u.data.count = c->nelems - first;
    c->items[item].u.data.width = width;
    return true;
}

/* .byte/.half/.word: lista de expressões; .byte aceita também strings. */
static bool data_list(ksas_ctx *c, ksas_cursor *cur, uint32_t col, uint8_t width)
{
    uint32_t first = c->nelems, run_col = cur->tok.col, node;

    if (!require_initialized(c, col)) {
        return false;
    }
    for (;;) {
        if (cur->tok.kind == TK_STR) {
            if (width != 1u) {
                return ksas_perr(c, cur->tok.col, "strings so sao aceitas em .byte");
            }
            if (!flush_data(c, first, run_col, width) || !string_item(c, &cur->tok, false)) {
                return false;
            }
            ksas_advance(c, cur);
            first = c->nelems;
            run_col = cur->tok.col;
        } else if (!ksas_parse_expr(c, cur, &node) || !append_elem(c, node)) {
            return false;
        }
        if (cur->tok.kind != TK_COMMA) {
            return flush_data(c, first, run_col, width);
        }
        ksas_advance(c, cur);
        if (c->nelems == first) {
            run_col = cur->tok.col;
        }
    }
}

static bool dir_byte(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    return data_list(c, cur, col, 1u);
}

static bool dir_half(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    return data_list(c, cur, col, 2u);
}

static bool dir_word(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    return data_list(c, cur, col, 4u);
}

/* .float v[, v...]: binary32 little-endian, como .word dos bits. Só
 * literais (ksas_parse_float), não expressões. O cursor chega aqui sem que
 * o primeiro valor tenha sido reportado como TK_ERROR (parse_directive). */
static bool dir_float(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    uint32_t first = c->nelems, run_col = cur->tok.col, bits, vcol, node;

    if (!require_initialized(c, col)) {
        return false;
    }
    for (;;) {
        vcol = cur->tok.col;
        if (!ksas_parse_float(c, cur, &bits)) {
            return false;
        }
        node = ksas_const_node(c, bits, vcol);
        if (node == KSAS_NONE || !append_elem(c, node)) {
            return false;
        }
        if (cur->tok.kind != TK_COMMA) {
            return flush_data(c, first, run_col, 4u);
        }
        ksas_cur_advance(cur);                 /* idem: o próximo pode ser "2.5" */
    }
}

static bool string_list(ksas_ctx *c, ksas_cursor *cur, uint32_t col, bool zero_term)
{
    if (!require_initialized(c, col)) {
        return false;
    }
    for (;;) {
        if (cur->tok.kind != TK_STR) {
            return ksas_perr(c, cur->tok.col, "esperado string entre aspas");
        }
        if (!string_item(c, &cur->tok, zero_term)) {
            return false;
        }
        ksas_advance(c, cur);
        if (cur->tok.kind != TK_COMMA) {
            return true;
        }
        ksas_advance(c, cur);
    }
}

static bool dir_ascii(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    return string_list(c, cur, col, false);
}

static bool dir_asciz(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    return string_list(c, cur, col, true);
}

/* Item cujo tamanho vem de uma expressão avaliada no layout. */
static bool expr_item(ksas_ctx *c, ksas_cursor *cur, uint32_t col, ksas_item_kind kind)
{
    uint32_t node, item;

    if (!ksas_parse_expr(c, cur, &node)) {
        return false;
    }
    item = ksas_item_new(c, kind, col);
    if (item == KSAS_NONE) {
        return false;
    }
    c->items[item].u.expr = node;
    return true;
}

static bool dir_space(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    uint32_t count, fill = KSAS_NONE, item;

    if (!ksas_parse_expr(c, cur, &count)) {
        return false;
    }
    if (cur->tok.kind == TK_COMMA) {
        if (c->section == KSAS_SEC_BSS) {
            return ksas_perr(c, cur->tok.col, ".bss so reserva espaco: valor de preenchimento nao permitido");
        }
        ksas_advance(c, cur);
        if (!ksas_parse_expr(c, cur, &fill)) {
            return false;
        }
    }
    item = ksas_item_new(c, KSAS_IT_SPACE, col);
    if (item == KSAS_NONE) {
        return false;
    }
    c->items[item].u.space.count = count;
    c->items[item].u.space.fill = fill;
    return true;
}

static bool dir_align(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    return expr_item(c, cur, col, KSAS_IT_ALIGN);
}

static bool dir_org(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    return expr_item(c, cur, col, KSAS_IT_ORG);
}

/* .entry e .stack: uma vez só; avaliados no fim (passo 2). */
static bool once_expr(ksas_ctx *c, ksas_cursor *cur, uint32_t col, uint32_t *expr,
                      uint32_t *line, const char *name)
{
    if (*expr != KSAS_NONE) {
        return ksas_perr(c, col, "%s repetido", name);
    }
    if (!ksas_parse_expr(c, cur, expr)) {
        return false;
    }
    *line = c->cur_line;
    return true;
}

static bool dir_entry(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    return once_expr(c, cur, col, &c->entry_expr, &c->entry_line, ".entry");
}

static bool dir_stack(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    return once_expr(c, cur, col, &c->stack_expr, &c->stack_line, ".stack");
}

static char *token_string(const ksas_token *t)
{
    uint32_t len = ksas_lex_unescape(t, NULL);
    char *s = malloc((size_t)len + 1u);

    if (s != NULL) {
        (void)ksas_lex_unescape(t, (uint8_t *)s);
        s[len] = '\0';
    }
    return s;
}

/* Nome com que o arquivo incluído é registrado: "path" relativo ao
 * diretório de "from" (como o callback resolve), para que um .include
 * dentro dele parta do diretório dele e as mensagens citem o caminho
 * resolvido. Absoluto ("/x", "\x", "C:...") fica como está. NULL sem
 * memória. */
static char *include_name(const char *from, const char *path)
{
    const char *p, *slash = NULL;
    size_t dir_len, len = strlen(path);
    char *name;

    for (p = from; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            slash = p;
        }
    }
    dir_len = (path[0] == '/' || path[0] == '\\' || (path[0] != '\0' && path[1] == ':') ||
               slash == NULL) ? 0u : (size_t)(slash - from) + 1u;
    name = malloc(dir_len + len + 1u);
    if (name != NULL) {
        memcpy(name, from, dir_len);
        memcpy(name + dir_len, path, len + 1u);
    }
    return name;
}

static bool include_file(ksas_ctx *c, uint32_t col, char *path)
{
    const ksas_line *line = &c->lines[c->cur_line];
    const char *err = "falha de leitura";
    uint32_t saved_line = c->cur_line, file;
    char *text, *name;

    if (c->opt->include == NULL) {
        free(path);
        return ksas_perr(c, col, ".include nao suportado neste contexto");
    }
    if (c->include_depth >= KSAS_MAX_INCLUDE_DEPTH) {
        free(path);
        return ksas_perr(c, col, ".include aninhado demais (ciclo de inclusao?)");
    }
    text = c->opt->include(c->opt->include_ctx, c->files[line->file].name, path, &err);
    if (text == NULL) {
        (void)ksas_perr(c, col, "nao foi possivel incluir '%s': %s", path, err);
        free(path);
        return false;
    }
    name = include_name(c->files[line->file].name, path);
    free(path);
    if (name == NULL) {
        free(text);
        return ksas_perr(c, col, "memoria insuficiente");
    }
    file = ksas_file_add(c, name, name, text, true);
    if (file == KSAS_NONE) {
        return false;
    }
    c->include_depth++;
    ksas_parse_source(c, file, text);
    c->include_depth--;
    c->cur_line = saved_line;
    c->line_failed = false;
    return true;
}

static bool dir_include(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    ksas_token t = cur->tok;
    char *path;

    (void)col;
    if (t.kind != TK_STR) {
        return ksas_perr(c, t.col, "esperado nome de arquivo entre aspas");
    }
    ksas_advance(c, cur);
    if (!ksas_at_eol(cur)) {
        return ksas_perr(c, cur->tok.col, "texto inesperado depois de .include");
    }
    path = token_string(&t);
    if (path == NULL) {
        return ksas_perr(c, t.col, "memoria insuficiente");
    }
    return include_file(c, t.col, path);
}

typedef struct directive {
    const char *name;
    dir_fn      fn;
} directive;

static const directive directives[] = {
    { "text", dir_text },     { "rodata", dir_rodata }, { "data", dir_data },
    { "bss", dir_bss },       { "global", dir_global }, { "globl", dir_global },
    { "equ", dir_equ },       { "byte", dir_byte },     { "half", dir_half },
    { "word", dir_word },     { "float", dir_float },   { "ascii", dir_ascii },
    { "asciz", dir_asciz },   { "space", dir_space },   { "align", dir_align },
    { "org", dir_org },       { "entry", dir_entry },   { "stack", dir_stack },
    { "include", dir_include }, { "pic", dir_pic }
};

static bool parse_directive(ksas_ctx *c, ksas_cursor *cur)
{
    ksas_token t = cur->tok;
    size_t i;

    for (i = 0; i < sizeof directives / sizeof directives[0]; i++) {
        if (ksas_name_eq(t.text, t.len, directives[i].name)) {
            if (directives[i].fn == dir_float) {
                ksas_cur_advance(cur);       /* "1.5" não é token: ver ksas_parse_float */
            } else {
                ksas_advance(c, cur);
            }
            return directives[i].fn(c, cur, t.col);
        }
    }
    return ksas_perr(c, t.col, "diretiva desconhecida '.%.*s'", (int)t.len, t.text);
}

/* ---- Linhas ----------------------------------------------------------- */

static bool parse_statement(ksas_ctx *c, ksas_cursor *cur)
{
    ksas_token next;

    ksas_cur_peek(cur, &next);
    switch (cur->tok.kind) {
    case TK_IDENT:
        if (next.kind == TK_EQUALS) {
            ksas_token name = cur->tok;
            ksas_advance(c, cur);
            ksas_advance(c, cur);
            return define_equ(c, cur, &name);
        }
        return ksas_parse_insn(c, cur);
    case TK_DIRECTIVE:
        return parse_directive(c, cur);
    case TK_REG:
        if (next.kind == TK_COLON) {
            return ksas_perr(c, cur->tok.col, "'%.*s' e um registrador e nao pode ser label",
                             (int)cur->tok.len, cur->tok.text);
        }
        return ksas_perr(c, cur->tok.col, "esperado label, instrucao ou diretiva");
    case TK_ERROR:
        return false;                       /* já reportado pelo cursor */
    default:
        return ksas_perr(c, cur->tok.col, "esperado label, instrucao ou diretiva");
    }
}

static void parse_line(ksas_ctx *c, ksas_cursor *cur)
{
    ksas_token next;

    for (;;) {
        if (cur->tok.kind != TK_IDENT) {
            break;
        }
        ksas_cur_peek(cur, &next);
        if (next.kind != TK_COLON) {
            break;
        }
        if (!define_label(c, &cur->tok)) {
            return;
        }
        ksas_advance(c, cur);
        ksas_advance(c, cur);
    }
    if (ksas_at_eol(cur) || !parse_statement(c, cur)) {
        return;
    }
    if (!ksas_at_eol(cur)) {
        (void)ksas_perr(c, cur->tok.col, "texto inesperado no fim da linha");
    }
}

static bool new_line(ksas_ctx *c, uint32_t file, const ksas_cursor *cur)
{
    ksas_line *l = ksas_grow(c, c->lines, &c->lines_cap, c->nlines + 1u, sizeof *l,
                             KSAS_MAX_LINES, "linhas");

    if (l == NULL) {
        return false;
    }
    c->lines = l;
    l = &c->lines[c->nlines];
    l->file = file;
    l->lineno = cur->lx.line;
    l->text = cur->lx.line_start;
    l->len = ksas_lex_line_len(cur->lx.line_start);
    l->first_item = c->nitems;
    c->cur_line = c->nlines++;
    c->line_failed = false;
    return true;
}

/* Descarta o resto da linha (após erro) sem reportar mais nada. */
static void skip_line(ksas_cursor *cur)
{
    while (!ksas_at_eol(cur)) {
        ksas_cur_advance(cur);
    }
}

void ksas_parse_source(ksas_ctx *c, uint32_t file, const char *text)
{
    ksas_cursor cur;

    ksas_cur_init(&cur, text);
    while (!c->aborted && *cur.lx.p != '\0') {
        if (!new_line(c, file, &cur)) {
            return;
        }
        ksas_advance(c, &cur);
        parse_line(c, &cur);
        skip_line(&cur);
        if (cur.tok.kind == TK_EOF) {
            return;
        }
    }
}

void ksas_parse_define(ksas_ctx *c, uint32_t file, const char *text)
{
    ksas_cursor cur;
    ksas_token name;

    ksas_cur_init(&cur, text);
    if (!new_line(c, file, &cur)) {
        return;
    }
    ksas_advance(c, &cur);
    name = cur.tok;
    if (name.kind != TK_IDENT) {
        (void)ksas_perr(c, name.col, "-D espera NOME ou NOME=valor");
        return;
    }
    ksas_advance(c, &cur);
    if (cur.tok.kind == TK_EQUALS) {
        ksas_advance(c, &cur);
        if (define_equ(c, &cur, &name) && cur.tok.kind != TK_EOF) {
            (void)ksas_perr(c, cur.tok.col, "texto inesperado na definicao -D");
        }
        return;
    }
    if (cur.tok.kind != TK_EOF) {
        (void)ksas_perr(c, cur.tok.col, "-D espera NOME ou NOME=valor");
        return;
    }
    /* "-DNOME" sozinho vale 1, como no cc. */
    (void)define_equ_node(c, &name, ksas_const_node(c, 1u, name.col));
}
