/*
 * layout.c — passo 1: endereços e tamanhos (ver layout.h).
 */
#include "ksvm/encode.h"
#include "ksvm/isa.h"
#include "expr.h"
#include "layout.h"
#include "symtab.h"

#define KSAS_MAX_ALIGN 65536u
#define KSAS_RAM_ALIGN 16u      /* doc 05: ram_base = código alinhado a 16 */

static uint32_t node_col(const ksas_ctx *c, uint32_t node)
{
    return (node == KSAS_NONE) ? 0u : c->nodes[node].col;
}

/* Expressão que precisa de valor já no passo 1 (.space, .align, .org). */
static bool layout_const(ksas_ctx *c, uint32_t idx, uint32_t node, uint32_t *v,
                         const char *what)
{
    const ksas_item *it = &c->items[idx];
    ksas_evalctx e;
    ksas_ev r;

    e.line = it->line;
    e.dot_item = idx;
    e.report = true;
    r = ksas_expr_abs(c, &e, node, v);
    if (r == KSAS_EV_UNKNOWN) {
        ksas_error(c, it->line, node_col(c, node),
                   "%s precisa ser constante conhecida neste ponto "
                   "(sem labels adiante nem enderecos de dados)", what);
    }
    return r == KSAS_EV_OK;
}

/* Forma curta (I, imm14) ou longa (L, imm32): decidida uma única vez. */
static void choose_form(ksas_ctx *c, uint32_t idx, ksas_insn *in)
{
    ksas_evalctx e;
    uint32_t v;
    bool go_long;

    e.line = c->items[idx].line;
    e.dot_item = idx;
    e.report = false;            /* o passo 2 reavalia e reporta */
    if (ksas_expr_abs(c, &e, in->expr, &v) == KSAS_EV_OK) {
        go_long = !ksvm_imm14_fits(ksas_s32(v));
    } else {
        go_long = (in->flags & KSAS_INSN_LONG_UNK) != 0u;
    }
    if (go_long) {
        in->op = in->long_op;
        in->imm_kind = KSAS_IMM_32;
        if ((in->flags & KSAS_INSN_NEG_LONG) != 0u) {
            in->flags |= KSAS_INSN_NEGATE;
        }
    }
}

static bool size_insn(ksas_ctx *c, uint32_t idx, ksas_item *it)
{
    ksas_insn *in = &it->u.insn;

    if ((it->offset & 3u) != 0u) {
        ksas_error(c, it->line, it->col,
                   "instrucao em endereco desalinhado (0x%lX): use .align 4 antes",
                   (unsigned long)it->offset);
        return false;
    }
    if ((in->flags & KSAS_INSN_VAR) != 0u) {
        choose_form(c, idx, in);
    }
    it->size = ksvm_isa[in->op].len;
    return true;
}

static bool size_space(ksas_ctx *c, uint32_t idx, ksas_item *it)
{
    uint32_t n;

    if (!layout_const(c, idx, it->u.space.count, &n, "o tamanho de .space")) {
        return false;
    }
    if (ksas_s32(n) < 0) {
        ksas_error(c, it->line, node_col(c, it->u.space.count),
                   "tamanho de .space negativo (%ld)", (long)ksas_s32(n));
        return false;
    }
    it->size = n;
    return true;
}

static bool size_align(ksas_ctx *c, uint32_t idx, ksas_item *it)
{
    uint32_t n;

    if (!layout_const(c, idx, it->u.expr, &n, "o argumento de .align")) {
        return false;
    }
    if (n == 0u || n > KSAS_MAX_ALIGN || (n & (n - 1u)) != 0u) {
        ksas_error(c, it->line, node_col(c, it->u.expr),
                   ".align exige potencia de 2 entre 1 e %u (recebeu %lu)", KSAS_MAX_ALIGN,
                   (unsigned long)n);
        return false;
    }
    if (n > c->sec_align[it->section]) {
        c->sec_align[it->section] = n;    /* a base da seção herda o alinhamento */
    }
    it->size = (0u - it->offset) & (n - 1u);
    return true;
}

/* .org: deslocamento absoluto dentro da seção, só para frente. */
static bool size_org(ksas_ctx *c, uint32_t idx, ksas_item *it)
{
    uint32_t target;

    if (!layout_const(c, idx, it->u.expr, &target, "o argumento de .org")) {
        return false;
    }
    if (target < it->offset) {
        ksas_error(c, it->line, node_col(c, it->u.expr),
                   ".org nao pode voltar (0x%lX < posicao atual 0x%lX em %s)",
                   (unsigned long)target, (unsigned long)it->offset,
                   ksas_section_name(it->section));
        return false;
    }
    it->size = target - it->offset;
    return true;
}

static bool item_size(ksas_ctx *c, uint32_t idx, ksas_item *it)
{
    ksas_symbol *s;

    switch (it->kind) {
    case KSAS_IT_LABEL:
        s = &c->sym.syms[it->u.sym];
        s->offset = it->offset;
        s->placed = true;
        return true;
    case KSAS_IT_INSN:  return size_insn(c, idx, it);
    case KSAS_IT_DATA:  it->size = it->u.data.count * it->u.data.width; return true;
    case KSAS_IT_SPACE: return size_space(c, idx, it);
    case KSAS_IT_ALIGN: return size_align(c, idx, it);
    case KSAS_IT_ORG:   return size_org(c, idx, it);
    default:            return true;   /* EQU: tamanho 0; BYTES: já definido */
    }
}

/* Avança a posição da seção; .bss só precisa caber em 32 bits, as outras
 * viram bytes no arquivo e têm limite (KSAS_MAX_SECTION). */
static bool advance(ksas_ctx *c, const ksas_item *it, uint32_t *pos)
{
    uint64_t end = (uint64_t)*pos + it->size;
    uint64_t limit = (it->section == KSAS_SEC_BSS) ? UINT64_C(0xFFFFFFFF) : KSAS_MAX_SECTION;

    if (end > limit) {
        ksas_error(c, it->line, it->col, "secao %s excede o limite de %llu bytes",
                   ksas_section_name(it->section), (unsigned long long)limit);
        return false;
    }
    *pos = (uint32_t)end;
    return true;
}

static uint64_t align_up(uint64_t v, uint32_t a)
{
    return (v + a - 1u) & ~(uint64_t)(a - 1u);
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return (a > b) ? a : b;
}

/* A RAM começa alinhada ao maior .align de .data e .bss, para que o
 * alinhamento relativo à seção também valha no endereço absoluto. */
static bool compute_bases(ksas_ctx *c)
{
    uint32_t ram_align = max_u32(KSAS_RAM_ALIGN, max_u32(c->sec_align[KSAS_SEC_DATA],
                                                         c->sec_align[KSAS_SEC_BSS]));
    uint64_t rodata = align_up(c->sec_size[KSAS_SEC_TEXT], c->sec_align[KSAS_SEC_RODATA]);
    uint64_t code = rodata + c->sec_size[KSAS_SEC_RODATA];
    uint64_t ram = align_up(code, ram_align);
    uint64_t data = align_up(c->sec_size[KSAS_SEC_DATA], c->sec_align[KSAS_SEC_BSS]);
    uint64_t end = ram + data + c->sec_size[KSAS_SEC_BSS];

    if (code > KSAS_MAX_SECTION || end > UINT64_C(0xFFFFFFFF)) {
        ksas_error(c, KSAS_NONE, 0, "imagem nao cabe no espaco de enderecos de 32 bits");
        return false;
    }
    c->sec_base[KSAS_SEC_TEXT] = 0;
    c->sec_base[KSAS_SEC_RODATA] = (uint32_t)rodata;
    c->sec_base[KSAS_SEC_DATA] = (uint32_t)ram;
    c->sec_base[KSAS_SEC_BSS] = (uint32_t)(ram + data);
    c->code_size = (uint32_t)code;
    c->ram_base = (uint32_t)ram;
    c->data_size = (uint32_t)data;
    c->bss_size = c->sec_size[KSAS_SEC_BSS];
    return true;
}

bool ksas_layout(ksas_ctx *c)
{
    uint32_t pos[KSAS_SEC_COUNT] = { 0 };
    uint32_t i, sec;

    for (sec = 0; sec < KSAS_SEC_COUNT; sec++) {
        c->sec_align[sec] = 4u;
    }
    for (i = 0; i < c->nitems && !c->aborted; i++) {
        ksas_item *it = &c->items[i];

        c->layout_next = i;
        it->offset = pos[it->section];
        if (!item_size(c, i, it)) {
            it->size = 0;
        }
        if (!advance(c, it, &pos[it->section])) {
            return false;
        }
    }
    c->layout_next = c->nitems;
    for (sec = 0; sec < KSAS_SEC_COUNT; sec++) {
        c->sec_size[sec] = pos[sec];
    }
    return !c->aborted && compute_bases(c);
}
