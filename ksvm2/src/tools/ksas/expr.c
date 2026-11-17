/*
 * expr.c — parser e avaliador de expressões (ver expr.h).
 */
#include <string.h>

#include "expr.h"
#include "symtab.h"

/* Profundidade máxima da avaliação recursiva (cadeias de .equ e somas
 * longas viram árvores profundas); evita estourar a pilha do host. */
#define KSAS_MAX_EVAL_DEPTH 4096u

uint32_t ksas_node_new(ksas_ctx *c, ksas_node_op op, uint32_t col, uint32_t a, uint32_t b)
{
    ksas_node *n = ksas_grow(c, c->nodes, &c->nodes_cap, c->nnodes + 1u, sizeof *n,
                             KSAS_MAX_NODES, "nos de expressao");

    if (n == NULL) {
        return KSAS_NONE;
    }
    c->nodes = n;
    n = &c->nodes[c->nnodes];
    n->op = (uint8_t)op;
    n->col = col;
    n->a = a;
    n->b = b;
    return c->nnodes++;
}

bool ksas_expr_is_symbol(const ksas_ctx *c, uint32_t node)
{
    return node != KSAS_NONE && c->nodes[node].op == KSAS_EN_SYM;
}

/* ---- Parser ----------------------------------------------------------- */

typedef struct binop {
    ksas_tok     tok;
    ksas_node_op op;
    unsigned     prec;
} binop;

static const binop binops[] = {
    { TK_PIPE,  KSAS_EN_OR,  1 }, { TK_CARET, KSAS_EN_XOR, 2 }, { TK_AMP,   KSAS_EN_AND, 3 },
    { TK_SHL,   KSAS_EN_SHL, 4 }, { TK_SHR,   KSAS_EN_SHR, 4 },
    { TK_PLUS,  KSAS_EN_ADD, 5 }, { TK_MINUS, KSAS_EN_SUB, 5 },
    { TK_STAR,  KSAS_EN_MUL, 6 }, { TK_SLASH, KSAS_EN_DIV, 6 }, { TK_PERCENT, KSAS_EN_MOD, 6 }
};

static const binop *find_binop(ksas_tok tok)
{
    size_t i;

    for (i = 0; i < sizeof binops / sizeof binops[0]; i++) {
        if (binops[i].tok == tok) {
            return &binops[i];
        }
    }
    return NULL;
}

static bool parse_binary(ksas_ctx *c, ksas_cursor *cur, unsigned min_prec,
                         unsigned depth, uint32_t *out);

static bool make_leaf(ksas_ctx *c, ksas_cursor *cur, ksas_node_op op, uint32_t a, uint32_t *out)
{
    *out = ksas_node_new(c, op, cur->tok.col, a, 0);
    ksas_advance(c, cur);
    return *out != KSAS_NONE;
}

static bool parse_primary(ksas_ctx *c, ksas_cursor *cur, unsigned depth, uint32_t *out)
{
    const ksas_token *t = &cur->tok;
    uint32_t sym;

    switch (t->kind) {
    case TK_NUM:
    case TK_CHAR:
        return make_leaf(c, cur, KSAS_EN_NUM, t->value, out);
    case TK_IDENT:
        sym = ksas_sym_intern(c, t->text, t->len, c->cur_line, t->col);
        return sym != KSAS_NONE && make_leaf(c, cur, KSAS_EN_SYM, sym, out);
    case TK_DOT:
        return make_leaf(c, cur, KSAS_EN_DOT, 0, out);
    case TK_LPAREN:
        ksas_advance(c, cur);
        if (!parse_binary(c, cur, 1, depth + 1u, out)) {
            return false;
        }
        if (cur->tok.kind != TK_RPAREN) {
            return ksas_perr(c, cur->tok.col, "esperado ')'");
        }
        ksas_advance(c, cur);
        return true;
    case TK_REG:
        return ksas_perr(c, t->col, "registrador '%.*s' nao pode aparecer em expressao",
                         (int)t->len, t->text);
    default:
        return ksas_perr(c, t->col, "esperado expressao");
    }
}

static bool parse_unary(ksas_ctx *c, ksas_cursor *cur, unsigned depth, uint32_t *out)
{
    ksas_tok k = cur->tok.kind;
    uint32_t col = cur->tok.col, inner;

    if (depth > KSAS_MAX_EXPR_DEPTH) {
        return ksas_perr(c, col, "expressao aninhada demais");
    }
    if (k != TK_MINUS && k != TK_TILDE && k != TK_PLUS) {
        return parse_primary(c, cur, depth, out);
    }
    ksas_advance(c, cur);
    if (!parse_unary(c, cur, depth + 1u, &inner)) {
        return false;
    }
    if (k == TK_PLUS) {
        *out = inner;
        return true;
    }
    *out = ksas_node_new(c, (k == TK_MINUS) ? KSAS_EN_NEG : KSAS_EN_CPL, col, inner, 0);
    return *out != KSAS_NONE;
}

/* Precedence climbing: operadores de mesma precedência associam à esquerda. */
static bool parse_binary(ksas_ctx *c, ksas_cursor *cur, unsigned min_prec,
                         unsigned depth, uint32_t *out)
{
    if (!parse_unary(c, cur, depth, out)) {
        return false;
    }
    for (;;) {
        const binop *b = find_binop(cur->tok.kind);
        uint32_t col = cur->tok.col, rhs;

        if (b == NULL || b->prec < min_prec) {
            return true;
        }
        ksas_advance(c, cur);
        if (!parse_binary(c, cur, b->prec + 1u, depth + 1u, &rhs)) {
            return false;
        }
        *out = ksas_node_new(c, b->op, col, *out, rhs);
        if (*out == KSAS_NONE) {
            return false;
        }
    }
}

bool ksas_expr_parse(ksas_ctx *c, ksas_cursor *cur, uint32_t *out)
{
    return parse_binary(c, cur, 1, 0, out);
}

/* ---- Avaliação -------------------------------------------------------- */

static ksas_ev eval(ksas_ctx *c, const ksas_evalctx *e, uint32_t node, unsigned depth,
                    ksas_value *out);

static bool reporting(const ksas_ctx *c, const ksas_evalctx *e)
{
    return e->report || c->final;
}

static ksas_ev fail(ksas_ctx *c, const ksas_evalctx *e, uint32_t col, const char *fmt,
                    const char *name, int name_len)
{
    if (reporting(c, e)) {
        if (name != NULL) {
            ksas_error(c, e->line, col, fmt, name_len, name);
        } else {
            ksas_error(c, e->line, col, "%s", fmt);
        }
    }
    return KSAS_EV_ERROR;
}

/* Endereço de (seção, deslocamento) no modo corrente. */
static ksas_ev location(const ksas_ctx *c, uint8_t sec, uint32_t off, ksas_value *out)
{
    if (c->final) {
        out->v = c->sec_base[sec] + off;
        out->base = KSAS_BASE_ABS;
    } else {
        /* .text começa em 0: seus deslocamentos já são endereços finais. */
        out->v = off;
        out->base = (sec == KSAS_SEC_TEXT) ? KSAS_BASE_ABS : sec;
    }
    return KSAS_EV_OK;
}

static ksas_ev eval_dot(ksas_ctx *c, const ksas_evalctx *e, uint32_t col, ksas_value *out)
{
    const ksas_item *it;

    if (e->dot_item == KSAS_NONE) {
        return fail(c, e, col, "'.' nao tem endereco neste contexto", NULL, 0);
    }
    if (!c->final && e->dot_item > c->layout_next) {
        return KSAS_EV_UNKNOWN;
    }
    it = &c->items[e->dot_item];
    return location(c, it->section, it->offset, out);
}

static ksas_ev eval_equ(ksas_ctx *c, const ksas_evalctx *e, uint32_t sym, uint32_t col,
                        unsigned depth, ksas_value *out)
{
    ksas_symbol *s = &c->sym.syms[sym];
    ksas_evalctx inner;
    ksas_ev r;

    if (s->bad) {
        return KSAS_EV_ERROR;
    }
    if (s->busy) {
        return fail(c, e, col, "definicao circular envolvendo '%.*s'",
                    ksas_sym_name(c, sym), s->name_len);
    }
    /* Erros dentro da definição apontam para a linha do .equ. */
    inner.line = s->line;
    inner.dot_item = s->item;
    inner.report = e->report;
    s->busy = true;
    r = eval(c, &inner, s->expr, depth + 1u, out);
    s->busy = false;
    if (r == KSAS_EV_ERROR && reporting(c, e)) {
        s->bad = true;    /* não repetir o mesmo erro a cada uso */
    }
    return r;
}

static ksas_ev eval_symbol(ksas_ctx *c, const ksas_evalctx *e, const ksas_node *n,
                           unsigned depth, ksas_value *out)
{
    const ksas_symbol *s = &c->sym.syms[n->a];

    switch (s->kind) {
    case KSAS_SYM_LABEL:
        if (!c->final && !s->placed) {
            return KSAS_EV_UNKNOWN;
        }
        return location(c, s->section, s->offset, out);
    case KSAS_SYM_EQU:
        return eval_equ(c, e, n->a, n->col, depth, out);
    default:
        if (!reporting(c, e)) {
            return KSAS_EV_UNKNOWN;
        }
        return fail(c, e, n->col, "simbolo '%.*s' nao definido", ksas_sym_name(c, n->a),
                    s->name_len);
    }
}

static ksas_ev divide(ksas_ctx *c, const ksas_evalctx *e, const ksas_node *n,
                      uint32_t a, uint32_t b, uint32_t *r)
{
    int32_t sa = ksas_s32(a), sb = ksas_s32(b);

    if (b == 0u) {
        return fail(c, e, n->col, "divisao por zero", NULL, 0);
    }
    if (sa == INT32_MIN && sb == -1) {      /* único caso que estoura em C */
        *r = (n->op == KSAS_EN_DIV) ? a : 0u;
    } else {
        *r = (uint32_t)((n->op == KSAS_EN_DIV) ? sa / sb : sa % sb);
    }
    return KSAS_EV_OK;
}

/* + e - preservam a seção de um operando relativo; rel - rel da mesma
 * seção vira absoluto. Qualquer outra combinação com relativo é UNKNOWN
 * no passo 1 (no passo 2 tudo é absoluto). */
static ksas_ev add_sub(const ksas_node *n, ksas_value a, ksas_value b, ksas_value *out)
{
    bool sub = (n->op == KSAS_EN_SUB);

    out->v = sub ? a.v - b.v : a.v + b.v;
    if (b.base == KSAS_BASE_ABS) {
        out->base = a.base;
    } else if (!sub && a.base == KSAS_BASE_ABS) {
        out->base = b.base;
    } else if (sub && a.base == b.base) {
        out->base = KSAS_BASE_ABS;
    } else {
        return KSAS_EV_UNKNOWN;
    }
    return KSAS_EV_OK;
}

static ksas_ev arith(ksas_ctx *c, const ksas_evalctx *e, const ksas_node *n,
                     ksas_value a, ksas_value b, ksas_value *out)
{
    uint32_t x = a.v, y = b.v;

    if (n->op == KSAS_EN_ADD || n->op == KSAS_EN_SUB) {
        return add_sub(n, a, b, out);
    }
    if (a.base != KSAS_BASE_ABS || b.base != KSAS_BASE_ABS) {
        return KSAS_EV_UNKNOWN;
    }
    out->base = KSAS_BASE_ABS;
    switch (n->op) {
    case KSAS_EN_MUL: out->v = x * y; break;
    case KSAS_EN_DIV:
    case KSAS_EN_MOD: return divide(c, e, n, x, y, &out->v);
    case KSAS_EN_AND: out->v = x & y; break;
    case KSAS_EN_OR:  out->v = x | y; break;
    case KSAS_EN_XOR: out->v = x ^ y; break;
    case KSAS_EN_SHL: out->v = (y >= 32u) ? 0u : x << y; break;
    default:          out->v = (y >= 32u) ? 0u : x >> y; break;  /* SHR lógico */
    }
    return KSAS_EV_OK;
}

static ksas_ev eval_unary(ksas_ctx *c, const ksas_evalctx *e, const ksas_node *n,
                          unsigned depth, ksas_value *out)
{
    ksas_ev r = eval(c, e, n->a, depth + 1u, out);

    if (r != KSAS_EV_OK) {
        return r;
    }
    if (out->base != KSAS_BASE_ABS) {
        return KSAS_EV_UNKNOWN;
    }
    out->v = (n->op == KSAS_EN_NEG) ? 0u - out->v : ~out->v;
    return KSAS_EV_OK;
}

static ksas_ev eval_binary(ksas_ctx *c, const ksas_evalctx *e, const ksas_node *n,
                           unsigned depth, ksas_value *out)
{
    ksas_value a, b;
    ksas_ev ra = eval(c, e, n->a, depth + 1u, &a);
    ksas_ev rb;

    /* Erro à esquerda já foi reportado: avaliar a direita só repetiria o
     * diagnóstico ("Y + Y" com Y indefinido). Com a esquerda UNKNOWN (passo
     * 1) a direita é avaliada, para que os erros dela apareçam. */
    if (ra == KSAS_EV_ERROR) {
        return KSAS_EV_ERROR;
    }
    rb = eval(c, e, n->b, depth + 1u, &b);
    if (rb == KSAS_EV_ERROR) {
        return KSAS_EV_ERROR;
    }
    if (ra == KSAS_EV_UNKNOWN || rb == KSAS_EV_UNKNOWN) {
        return KSAS_EV_UNKNOWN;
    }
    return arith(c, e, n, a, b, out);
}

static ksas_ev eval(ksas_ctx *c, const ksas_evalctx *e, uint32_t node, unsigned depth,
                    ksas_value *out)
{
    const ksas_node *n = &c->nodes[node];

    if (depth > KSAS_MAX_EVAL_DEPTH) {
        return fail(c, e, n->col, "expressao (ou cadeia de .equ) profunda demais", NULL, 0);
    }
    switch (n->op) {
    case KSAS_EN_NUM:
        out->v = n->a;
        out->base = KSAS_BASE_ABS;
        return KSAS_EV_OK;
    case KSAS_EN_SYM:
        return eval_symbol(c, e, n, depth, out);
    case KSAS_EN_DOT:
        return eval_dot(c, e, n->col, out);
    case KSAS_EN_NEG:
    case KSAS_EN_CPL:
        return eval_unary(c, e, n, depth, out);
    default:
        return eval_binary(c, e, n, depth, out);
    }
}

ksas_ev ksas_expr_eval(ksas_ctx *c, const ksas_evalctx *e, uint32_t node, ksas_value *out)
{
    out->v = 0;
    out->base = KSAS_BASE_ABS;
    if (node == KSAS_NONE) {
        return KSAS_EV_OK;     /* operando omitido ([rb]) vale 0 */
    }
    return eval(c, e, node, 0, out);
}

ksas_ev ksas_expr_abs(ksas_ctx *c, const ksas_evalctx *e, uint32_t node, uint32_t *out)
{
    ksas_value v;
    ksas_ev r = ksas_expr_eval(c, e, node, &v);

    *out = v.v;
    if (r == KSAS_EV_OK && v.base != KSAS_BASE_ABS) {
        return KSAS_EV_UNKNOWN;
    }
    return r;
}
