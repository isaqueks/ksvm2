/*
 * ctx.c — diagnósticos, crescimento de arrays e pequenos utilitários do
 * contexto do assembler (ver ir.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ksvm/bytes.h"
#include "ir.h"

#define KSAS_MSG_MAX 512

static void deliver(ksas_ctx *c, const char *file, uint32_t line, uint32_t col,
                    const char *msg)
{
    char text[KSAS_MSG_MAX + 128];
    ksas_diag d;

    if (line == 0u) {
        snprintf(text, sizeof text, "%s: erro: %s", file, msg);
    } else if (col == 0u) {
        snprintf(text, sizeof text, "%s:%lu: erro: %s", file, (unsigned long)line, msg);
    } else {
        snprintf(text, sizeof text, "%s:%lu:%lu: erro: %s", file, (unsigned long)line,
                 (unsigned long)col, msg);
    }
    if (c->opt->diag != NULL) {
        d.file = file;
        d.line = line;
        d.col = col;
        d.message = msg;
        d.text = text;
        c->opt->diag(c->opt->diag_ctx, &d);
    }
}

/* Nome do arquivo e número da linha (1-based) de um índice de linha. */
static void locate(const ksas_ctx *c, uint32_t line, const char **file, uint32_t *lineno)
{
    if (line == KSAS_NONE || line >= c->nlines) {
        *file = (c->nfiles > 0u) ? c->files[0].name : "<fonte>";
        *lineno = 0;
        return;
    }
    *file = c->files[c->lines[line].file].name;
    *lineno = c->lines[line].lineno;
}

void ksas_verror(ksas_ctx *c, uint32_t line, uint32_t col, const char *fmt, va_list ap)
{
    char msg[KSAS_MSG_MAX];
    const char *file;
    uint32_t lineno;

    if (c->aborted) {
        return;
    }
    vsnprintf(msg, sizeof msg, fmt, ap);
    locate(c, line, &file, &lineno);
    deliver(c, file, lineno, (lineno == 0u) ? 0u : col, msg);
    c->errors++;
    if (c->errors >= KSAS_MAX_ERRORS) {
        deliver(c, file, 0, 0, "muitos erros; montagem abortada");
        c->aborted = true;
    }
}

void ksas_error(ksas_ctx *c, uint32_t line, uint32_t col, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ksas_verror(c, line, col, fmt, ap);
    va_end(ap);
}

bool ksas_perr(ksas_ctx *c, uint32_t col, const char *fmt, ...)
{
    va_list ap;

    if (!c->line_failed) {
        va_start(ap, fmt);
        ksas_verror(c, c->cur_line, col, fmt, ap);
        va_end(ap);
        c->line_failed = true;
    }
    return false;
}

void ksas_advance(ksas_ctx *c, ksas_cursor *cur)
{
    ksas_cur_advance(cur);
    if (cur->tok.kind == TK_ERROR) {
        (void)ksas_perr(c, cur->tok.col, "%s", cur->tok.err);
    }
}

void *ksas_grow(ksas_ctx *c, void *data, uint32_t *cap, uint32_t need,
                size_t elem, uint32_t limit, const char *what)
{
    uint32_t ncap;
    void *p;

    if (need <= *cap) {
        return data;
    }
    if (need > limit) {
        ksas_error(c, c->cur_line, 0, "limite de %s excedido (%lu)", what,
                   (unsigned long)limit);
        return NULL;
    }
    ncap = (*cap < 64u) ? 64u : *cap;
    while (ncap < need) {
        ncap = (ncap > limit / 2u) ? limit : ncap * 2u;
    }
    p = realloc(data, (size_t)ncap * elem);
    if (p == NULL) {
        ksas_error(c, c->cur_line, 0, "memoria insuficiente (%s)", what);
        return NULL;
    }
    *cap = ncap;
    return p;
}

uint32_t ksas_item_new(ksas_ctx *c, ksas_item_kind kind, uint32_t col)
{
    ksas_item *it = ksas_grow(c, c->items, &c->items_cap, c->nitems + 1u,
                              sizeof *it, KSAS_MAX_ITEMS, "itens");

    if (it == NULL) {
        return KSAS_NONE;
    }
    c->items = it;
    it = &c->items[c->nitems];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    it->section = c->section;
    it->line = c->cur_line;
    it->col = col;
    return c->nitems++;
}

uint32_t ksas_pool_add(ksas_ctx *c, const uint8_t *bytes, uint32_t len)
{
    uint8_t *p;
    uint32_t off = c->npool;

    if (len > KSAS_MAX_POOL - c->npool) {
        ksas_error(c, c->cur_line, 0, "limite de bytes de strings excedido (%lu)",
                   (unsigned long)KSAS_MAX_POOL);
        return KSAS_NONE;
    }
    p = ksas_grow(c, c->pool, &c->pool_cap, c->npool + len, 1u, KSAS_MAX_POOL, "strings");
    if (p == NULL) {
        return KSAS_NONE;
    }
    c->pool = p;
    if (len != 0u && bytes != NULL) {
        memcpy(c->pool + off, bytes, len);
    } else if (len != 0u) {
        memset(c->pool + off, 0, len);     /* só reserva: quem chamou preenche */
    }
    c->npool += len;
    return off;
}

const char *ksas_section_name(uint8_t sec)
{
    static const char *const names[KSAS_SEC_COUNT] = { ".text", ".rodata", ".data", ".bss" };
    return (sec < KSAS_SEC_COUNT) ? names[sec] : "?";
}

int32_t ksas_s32(uint32_t v)
{
    return ksvm_u32_to_i32(v);
}
