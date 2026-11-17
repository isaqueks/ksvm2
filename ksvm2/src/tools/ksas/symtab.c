/*
 * symtab.c — tabela de símbolos: array estável + índice ordenado (ver symtab.h).
 */
#include <stdlib.h>
#include <string.h>

#include "symtab.h"

void ksas_sym_free(ksas_symtab *t)
{
    free(t->syms);
    free(t->order);
    free(t->names);
    memset(t, 0, sizeof *t);
}

const char *ksas_sym_name(const ksas_ctx *c, uint32_t sym)
{
    return c->sym.names + c->sym.syms[sym].name;
}

/* Ordem por bytes do nome (memcmp), mais curto primeiro em caso de prefixo. */
static int compare_name(const ksas_ctx *c, uint32_t sym, const char *name, uint32_t len)
{
    const ksas_symbol *s = &c->sym.syms[sym];
    uint32_t n = (s->name_len < len) ? s->name_len : len;
    int r = memcmp(c->sym.names + s->name, name, n);

    if (r != 0) {
        return r;
    }
    return (s->name_len < len) ? -1 : (s->name_len > len) ? 1 : 0;
}

/* Posição em order onde "name" está ou deveria entrar. */
static uint32_t lower_bound(const ksas_ctx *c, const char *name, uint32_t len)
{
    uint32_t lo = 0, hi = c->sym.count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (compare_name(c, c->sym.order[mid], name, len) < 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

uint32_t ksas_sym_find(const ksas_ctx *c, const char *name, uint32_t len)
{
    uint32_t pos = lower_bound(c, name, len);

    if (pos < c->sym.count && compare_name(c, c->sym.order[pos], name, len) == 0) {
        return c->sym.order[pos];
    }
    return KSAS_NONE;
}

static bool reserve(ksas_ctx *c, uint32_t name_len)
{
    ksas_symtab *t = &c->sym;
    void *p;

    p = ksas_grow(c, t->syms, &t->cap, t->count + 1u, sizeof *t->syms,
                  KSAS_MAX_SYMBOLS, "simbolos");
    if (p == NULL) {
        return false;
    }
    t->syms = p;
    p = ksas_grow(c, t->order, &t->order_cap, t->count + 1u, sizeof *t->order,
                  KSAS_MAX_SYMBOLS, "simbolos");
    if (p == NULL) {
        return false;
    }
    t->order = p;
    p = ksas_grow(c, t->names, &t->names_cap, t->names_len + name_len, 1u,
                  KSAS_MAX_POOL, "nomes de simbolos");
    if (p == NULL) {
        return false;
    }
    t->names = p;
    return true;
}

uint32_t ksas_sym_intern(ksas_ctx *c, const char *name, uint32_t len,
                         uint32_t line, uint32_t col)
{
    ksas_symtab *t = &c->sym;
    ksas_symbol *s;
    uint32_t pos, idx;

    idx = ksas_sym_find(c, name, len);
    if (idx != KSAS_NONE) {
        return idx;
    }
    if (len > KSAS_MAX_NAME) {
        ksas_error(c, line, col, "nome com mais de %u caracteres", KSAS_MAX_NAME);
        return KSAS_NONE;
    }
    if (!reserve(c, len)) {
        return KSAS_NONE;
    }
    pos = lower_bound(c, name, len);
    idx = t->count;
    s = &t->syms[idx];
    memset(s, 0, sizeof *s);
    s->name = t->names_len;
    s->name_len = (uint8_t)len;
    s->kind = KSAS_SYM_UNDEF;
    s->expr = KSAS_NONE;
    s->item = KSAS_NONE;
    s->line = line;
    s->col = col;
    memcpy(t->names + t->names_len, name, len);
    t->names_len += len;
    memmove(&t->order[pos + 1u], &t->order[pos], (size_t)(t->count - pos) * sizeof *t->order);
    t->order[pos] = idx;
    t->count++;
    return idx;
}

bool ksas_sym_define(ksas_ctx *c, uint32_t sym, ksas_sym_kind kind, uint32_t col)
{
    ksas_symbol *s = &c->sym.syms[sym];

    if (s->kind != KSAS_SYM_UNDEF) {
        const ksas_line *prev = &c->lines[s->line];
        return ksas_perr(c, col, "simbolo '%.*s' ja definido em %s:%lu", (int)s->name_len,
                         ksas_sym_name(c, sym), c->files[prev->file].name,
                         (unsigned long)prev->lineno);
    }
    s->kind = (uint8_t)kind;
    s->line = c->cur_line;
    s->col = col;
    return true;
}
