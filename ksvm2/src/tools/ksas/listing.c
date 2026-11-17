/*
 * listing.c — listing de montagem (ver listing.h).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "expr.h"
#include "listing.h"
#include "symtab.h"

#define ROW_BYTES   8u      /* bytes por linha do listing */
#define FILL_SHOWN  4u      /* .space/.align/.org: bytes mostrados */

typedef struct lst {
    ksas_ctx *c;
    char     *buf;
    size_t    cap;
    uint32_t  file;         /* arquivo da última linha listada */
} lst;

/* Formata e entrega uma linha; o buffer cresce com o tamanho do fonte. */
static void put(lst *L, size_t need, const char *fmt, ...)
{
    va_list ap;

    if (need > L->cap) {
        char *nb = realloc(L->buf, need);
        if (nb == NULL) {
            return;                        /* listing é auxiliar: só pula */
        }
        L->buf = nb;
        L->cap = need;
    }
    va_start(ap, fmt);
    vsnprintf(L->buf, L->cap, fmt, ap);
    va_end(ap);
    L->c->opt->listing(L->c->opt->listing_ctx, L->buf);
}

static void row(lst *L, const char *addr, const char *bytes, const ksas_line *src)
{
    if (src == NULL) {
        put(L, 64, "%-8s  %s\n", addr, bytes);
        return;
    }
    put(L, 96u + src->len, "%-8s  %-23s  %5lu  %.*s\n", addr, bytes,
        (unsigned long)src->lineno, (int)src->len, src->text);
}

static void hex_bytes(char *out, size_t cap, const uint8_t *p, uint32_t n)
{
    uint32_t i;
    size_t used = 0;

    out[0] = '\0';
    for (i = 0; i < n && used + 4u < cap; i++) {
        used += (size_t)snprintf(out + used, cap - used, (i == 0u) ? "%02X" : " %02X", p[i]);
    }
}

static const uint8_t *item_bytes(const ksas_ctx *c, const ksas_item *it)
{
    if (it->section == KSAS_SEC_DATA) {
        return c->data + it->offset;
    }
    return c->code + c->sec_base[it->section] + it->offset;
}

static void list_equ(lst *L, const ksas_item *it, const ksas_line *src)
{
    const ksas_symbol *s = &L->c->sym.syms[it->u.sym];
    ksas_evalctx e;
    ksas_value v;
    char text[32];

    e.line = it->line;
    e.dot_item = (uint32_t)(it - L->c->items);
    e.report = false;
    (void)ksas_expr_eval(L->c, &e, s->expr, &v);
    snprintf(text, sizeof text, "= 0x%08lX", (unsigned long)v.v);
    row(L, "", text, src);
}

/* Linhas de um item com bytes; src só na primeira. */
static void list_sized(lst *L, const ksas_item *it, const ksas_line *src)
{
    uint32_t addr = L->c->sec_base[it->section] + it->offset, off, n;
    bool filler = it->kind == KSAS_IT_SPACE || it->kind == KSAS_IT_ALIGN ||
                  it->kind == KSAS_IT_ORG;
    char a[16], b[48];

    snprintf(a, sizeof a, "%08lX", (unsigned long)addr);
    if (it->section == KSAS_SEC_BSS) {
        snprintf(b, sizeof b, "(reserva %lu)", (unsigned long)it->size);
        row(L, a, b, src);
        return;
    }
    if (filler && it->size > ROW_BYTES) {
        hex_bytes(b, sizeof b, item_bytes(L->c, it), FILL_SHOWN);
        snprintf(b + strlen(b), sizeof b - strlen(b), " ... (%lu)", (unsigned long)it->size);
        row(L, a, b, src);
        return;
    }
    for (off = 0; off < it->size; off += ROW_BYTES) {
        n = (it->size - off < ROW_BYTES) ? it->size - off : ROW_BYTES;
        snprintf(a, sizeof a, "%08lX", (unsigned long)(addr + off));
        hex_bytes(b, sizeof b, item_bytes(L->c, it) + off, n);
        row(L, a, b, (off == 0u) ? src : NULL);
    }
}

static void list_line(lst *L, uint32_t li, uint32_t end)
{
    ksas_ctx *c = L->c;
    const ksas_line *line = &c->lines[li];
    const ksas_line *src = line;
    uint32_t i;
    char a[16];

    if (line->file != L->file) {
        put(L, 64u + strlen(c->files[line->file].name), "; arquivo %s\n",
            c->files[line->file].name);
        L->file = line->file;
    }
    for (i = line->first_item; i < end; i++) {
        const ksas_item *it = &c->items[i];
        if (it->kind == KSAS_IT_EQU) {
            list_equ(L, it, src);
        } else if (it->size != 0u) {
            list_sized(L, it, src);
        } else {
            continue;
        }
        src = NULL;
    }
    if (src == NULL) {
        return;
    }
    a[0] = '\0';
    if (line->first_item < end) {          /* só labels: mostra o endereço */
        const ksas_item *it = &c->items[line->first_item];
        snprintf(a, sizeof a, "%08lX", (unsigned long)(c->sec_base[it->section] + it->offset));
    }
    row(L, a, "", src);
}

static void list_sections(lst *L)
{
    const ksas_ctx *c = L->c;
    uint8_t s;

    put(L, 64, "\n; secoes\n");
    for (s = 0; s < KSAS_SEC_COUNT; s++) {
        put(L, 96, ";   %-8s 0x%08lX  %lu bytes\n", ksas_section_name(s),
            (unsigned long)c->sec_base[s], (unsigned long)c->sec_size[s]);
    }
    put(L, 160, "; entry 0x%08lX  code %lu  data %lu  bss %lu  ram_base 0x%08lX  stack %lu%s\n",
        (unsigned long)c->entry, (unsigned long)c->code_size, (unsigned long)c->data_size,
        (unsigned long)c->bss_size, (unsigned long)c->ram_base, (unsigned long)c->stack_size,
        c->pic ? "  PIC" : "");
}

static void list_symbols(lst *L)
{
    ksas_ctx *c = L->c;
    uint32_t i;

    put(L, 64, "\n; simbolos\n");
    for (i = 0; i < c->sym.count; i++) {
        uint32_t idx = c->sym.order[i];
        const ksas_symbol *s = &c->sym.syms[idx];
        const char *kind = (s->kind == KSAS_SYM_EQU) ? "equ" : ksas_section_name(s->section);
        uint32_t v = 0;

        if (s->kind == KSAS_SYM_UNDEF) {
            continue;                      /* não ocorre sem erro; por garantia */
        }
        if (s->kind == KSAS_SYM_LABEL) {
            v = c->sec_base[s->section] + s->offset;
        } else {
            ksas_evalctx e = { s->line, s->item, false };
            ksas_value val;
            (void)ksas_expr_eval(c, &e, s->expr, &val);
            v = val.v;
        }
        put(L, 96u + s->name_len, ";   0x%08lX  %-7s %.*s%s\n", (unsigned long)v, kind,
            (int)s->name_len, ksas_sym_name(c, idx), s->global ? "  (global)" : "");
    }
}

void ksas_listing(ksas_ctx *c)
{
    lst L;
    uint32_t i;

    if (c->opt->listing == NULL) {
        return;
    }
    memset(&L, 0, sizeof L);
    L.c = c;
    L.file = KSAS_NONE;
    put(&L, 96, "%-8s  %-23s  %5s  %s\n", "ENDERECO", "BYTES", "LINHA", "FONTE");
    for (i = 0; i < c->nlines; i++) {
        uint32_t end = (i + 1u < c->nlines) ? c->lines[i + 1u].first_item : c->nitems;
        if (c->files[c->lines[i].file].listed) {
            list_line(&L, i, end);
        }
    }
    list_sections(&L);
    list_symbols(&L);
    free(L.buf);
}
