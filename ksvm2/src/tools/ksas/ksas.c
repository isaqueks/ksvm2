/*
 * ksas.c — driver do assembler: parse -> layout -> emit -> .ksx (doc 07).
 *
 * Sem E/S de arquivo: tudo entra e sai pela API de ksas.h.
 */
#include <stdlib.h>
#include <string.h>

#include "ksvm/bytes.h"
#include "ksvm/loader.h"
#include "ksvm/ksx_write.h"
#include "emit.h"
#include "expr.h"
#include "layout.h"
#include "listing.h"
#include "parser.h"
#include "symtab.h"

/* ---- Ponto de entrada e pilha ----------------------------------------- */

static uint32_t symbol_address(const ksas_ctx *c, const char *name)
{
    uint32_t sym = ksas_sym_find(c, name, (uint32_t)strlen(name));
    const ksas_symbol *s;

    if (sym == KSAS_NONE) {
        return KSAS_NONE;
    }
    s = &c->sym.syms[sym];
    return (s->kind == KSAS_SYM_LABEL) ? c->sec_base[s->section] + s->offset : KSAS_NONE;
}

static bool eval_directive(ksas_ctx *c, uint32_t node, uint32_t line, uint32_t *v)
{
    ksas_evalctx e;

    e.line = line;
    e.dot_item = KSAS_NONE;
    e.report = true;
    return ksas_expr_abs(c, &e, node, v) == KSAS_EV_OK;
}

/* .entry, senão _start, senão 0 (doc 07). */
static void resolve_entry(ksas_ctx *c)
{
    uint32_t v = 0;
    uint32_t col = 0;

    if (c->entry_expr != KSAS_NONE) {
        col = c->nodes[c->entry_expr].col;
        if (!eval_directive(c, c->entry_expr, c->entry_line, &v)) {
            return;
        }
    } else if (symbol_address(c, "_start") != KSAS_NONE) {
        v = symbol_address(c, "_start");
    }
    if (c->code_size != 0u && (v >= c->sec_size[KSAS_SEC_TEXT] || (v & 3u) != 0u)) {
        ksas_error(c, c->entry_line, col,
                   "ponto de entrada 0x%lX fora de .text ou desalinhado", (unsigned long)v);
    }
    c->entry = v;
}

static void resolve_stack(ksas_ctx *c)
{
    c->stack_size = KSAS_DEFAULT_STACK;
    if (c->stack_expr != KSAS_NONE) {
        (void)eval_directive(c, c->stack_expr, c->stack_line, &c->stack_size);
    }
}

static void check_globals(ksas_ctx *c)
{
    uint32_t i;

    for (i = 0; i < c->sym.count; i++) {
        const ksas_symbol *s = &c->sym.syms[i];
        if (s->global && s->kind == KSAS_SYM_UNDEF) {
            ksas_error(c, s->line, s->col, "simbolo global '%.*s' nao definido",
                       (int)s->name_len, ksas_sym_name(c, i));
        }
    }
}

/* ---- Tabela de símbolos do .ksx --------------------------------------- */

typedef struct sym_out {
    uint32_t addr;
    uint32_t rank;     /* posição na ordem por nome: desempate estável */
    uint32_t sym;
    uint8_t  kind;
} sym_out;

static int cmp_sym_out(const void *a, const void *b)
{
    const sym_out *x = a, *y = b;

    if (x->addr != y->addr) {
        return (x->addr < y->addr) ? -1 : 1;
    }
    return (x->rank < y->rank) ? -1 : (x->rank > y->rank) ? 1 : 0;
}

static bool symbol_exported(const ksas_ctx *c, const ksas_symbol *s)
{
    if (s->kind == KSAS_SYM_LABEL) {
        return s->global || c->opt->debug_symbols;
    }
    return s->kind == KSAS_SYM_EQU && s->global;
}

static uint8_t symbol_kind(const ksas_symbol *s)
{
    uint8_t k;

    if (s->kind == KSAS_SYM_EQU) {
        k = KSAS_SYMK_ABS;
    } else if (s->section == KSAS_SEC_DATA) {
        k = KSAS_SYMK_DATA;
    } else if (s->section == KSAS_SEC_BSS) {
        k = KSAS_SYMK_BSS;
    } else {
        k = KSAS_SYMK_CODE;
    }
    return (uint8_t)(k | (s->global ? KSAS_SYMK_GLOBAL : 0u));
}

static uint32_t symbol_value(ksas_ctx *c, uint32_t idx)
{
    const ksas_symbol *s = &c->sym.syms[idx];
    ksas_evalctx e;
    ksas_value v;

    if (s->kind == KSAS_SYM_LABEL) {
        return c->sec_base[s->section] + s->offset;
    }
    e.line = s->line;
    e.dot_item = s->item;
    e.report = false;
    (void)ksas_expr_eval(c, &e, s->expr, &v);
    return v.v;
}

/* Registros u32 addr, u8 kind, u8 len, name, ordenados por endereço. */
static uint8_t *symbol_blob(ksas_ctx *c, uint32_t *size)
{
    sym_out *list = malloc(((size_t)c->sym.count + 1u) * sizeof *list);
    uint32_t i, n = 0, total = 0;
    uint8_t *blob, *p;

    *size = 0;
    if (list == NULL) {
        return NULL;
    }
    for (i = 0; i < c->sym.count; i++) {
        uint32_t idx = c->sym.order[i];
        const ksas_symbol *s = &c->sym.syms[idx];
        if (symbol_exported(c, s)) {
            list[n].addr = symbol_value(c, idx);
            list[n].rank = i;
            list[n].sym = idx;
            list[n].kind = symbol_kind(s);
            total += 6u + s->name_len;
            n++;
        }
    }
    qsort(list, n, sizeof *list, cmp_sym_out);
    blob = malloc((size_t)total + 1u);
    for (i = 0, p = blob; blob != NULL && i < n; i++) {
        const ksas_symbol *s = &c->sym.syms[list[i].sym];
        ksvm_st32le(p, list[i].addr);
        p[4] = list[i].kind;
        p[5] = s->name_len;
        memcpy(p + 6, ksas_sym_name(c, list[i].sym), s->name_len);
        p += 6u + s->name_len;
    }
    free(list);
    *size = (blob != NULL) ? total : 0u;
    return blob;
}

/* ---- Imagem ----------------------------------------------------------- */

static void build_image(ksas_ctx *c, ksas_result *res)
{
    ksvm_ksx_desc d;
    uint32_t sym_size, size;
    uint8_t *sym = symbol_blob(c, &sym_size);

    memset(&d, 0, sizeof d);
    d.entry = c->entry;
    d.flags = (uint16_t)(c->pic ? KSVM_KSX_FLAG_PIC : 0u);
    d.code = c->code;
    d.code_size = c->code_size;
    d.data = c->data;
    d.data_size = c->data_size;
    d.bss_size = c->bss_size;
    d.ram_base = c->ram_base;
    d.stack_size = c->stack_size;
    d.sym = sym;
    d.sym_size = sym_size;
    size = ksvm_ksx_size(&d);
    res->image = (size != 0u) ? malloc(size) : NULL;
    if (res->image == NULL ||
        ksvm_ksx_write(&d, res->image, size, &res->image_size) != KSVM_OK) {
        ksas_error(c, KSAS_NONE, 0, "falha ao gerar a imagem .ksx");
        free(res->image);
        res->image = NULL;
        res->image_size = 0;
    } else {
        res->flags = (uint16_t)(d.flags | (sym_size != 0u ? KSVM_KSX_FLAG_SYMS : 0u));
    }
    free(sym);
}

/* ---- Driver ----------------------------------------------------------- */

static void add_defines(ksas_ctx *c)
{
    uint32_t file;
    size_t i;

    if (c->opt->num_defines == 0u) {
        return;
    }
    file = ksas_file_add(c, "<linha de comando>", NULL, NULL, false);
    for (i = 0; file != KSAS_NONE && i < c->opt->num_defines && !c->aborted; i++) {
        ksas_parse_define(c, file, c->opt->defines[i]);
    }
}

static void run(ksas_ctx *c, const char *src, ksas_result *res)
{
    const char *name = (c->opt->filename != NULL) ? c->opt->filename : "<fonte>";
    uint32_t main_file = ksas_file_add(c, name, NULL, NULL, true);

    if (main_file == KSAS_NONE) {
        return;
    }
    add_defines(c);            /* antes do fonte: redefinição acusa a linha do fonte */
    ksas_parse_source(c, main_file, src);
    if (c->aborted) {
        return;
    }
    check_globals(c);
    if (!ksas_layout(c) || !ksas_emit(c) || c->aborted) {
        return;
    }
    resolve_entry(c);
    resolve_stack(c);
    /* Só sem erros anteriores: uma linha inválida também deixa .text vazio. */
    if (c->errors == 0u && c->sec_size[KSAS_SEC_TEXT] == 0u) {
        ksas_error(c, KSAS_NONE, 0, "nenhuma instrucao em .text: a imagem nao teria ponto de entrada");
    }
    if (c->errors == 0u) {
        build_image(c, res);
    }
    if (c->errors == 0u) {
        ksas_listing(c);
    }
}

static void ctx_free(ksas_ctx *c)
{
    uint32_t i;

    for (i = 0; i < c->nfiles; i++) {
        free(c->files[i].owned_name);
        free(c->files[i].owned_text);
    }
    free(c->files);
    free(c->lines);
    free(c->items);
    free(c->nodes);
    free(c->elems);
    free(c->pool);
    free(c->code);
    free(c->data);
    ksas_sym_free(&c->sym);
    free(c);
}

int ksas_assemble(const char *src, const ksas_options *opt, ksas_result *res)
{
    static const ksas_options no_options;
    ksas_ctx *c;

    memset(res, 0, sizeof *res);
    c = calloc(1u, sizeof *c);
    if (c == NULL || src == NULL) {
        free(c);
        res->errors = 1;
        return -1;
    }
    c->opt = (opt != NULL) ? opt : &no_options;
    c->entry_expr = KSAS_NONE;
    c->stack_expr = KSAS_NONE;
    c->entry_line = KSAS_NONE;
    c->stack_line = KSAS_NONE;
    c->cur_line = KSAS_NONE;
    c->section = KSAS_SEC_TEXT;
    run(c, src, res);
    res->errors = c->errors;
    res->entry = c->entry;
    res->code_size = c->code_size;
    res->data_size = c->data_size;
    res->bss_size = c->bss_size;
    res->ram_base = c->ram_base;
    res->stack_size = c->stack_size;
    if (c->errors != 0u) {
        free(res->image);
        res->image = NULL;
        res->image_size = 0;
    }
    ctx_free(c);
    return (res->errors == 0u) ? 0 : -1;
}

void ksas_result_free(ksas_result *res)
{
    if (res != NULL) {
        free(res->image);
        res->image = NULL;
        res->image_size = 0;
    }
}
