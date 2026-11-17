/*
 * emit.c — passo 2: codificação e verificação de alcance (ver emit.h).
 */
#include <stdlib.h>
#include <string.h>

#include "ksvm/bytes.h"
#include "ksvm/encode.h"
#include "ksvm/isa.h"
#include "emit.h"
#include "expr.h"
#include "mnemonic.h"

/* Contexto de emissão de um item: posição para erros e endereço. */
typedef struct emit_at {
    uint32_t idx;
    uint32_t line;
    uint32_t col;      /* coluna do operando (ou do item) */
    uint32_t addr;     /* endereço absoluto do item */
} emit_at;

static void at_init(const ksas_ctx *c, uint32_t idx, uint32_t node, emit_at *at)
{
    const ksas_item *it = &c->items[idx];

    at->idx = idx;
    at->line = it->line;
    at->col = (node != KSAS_NONE) ? c->nodes[node].col : it->col;
    at->addr = c->sec_base[it->section] + it->offset;
}

static bool final_value(ksas_ctx *c, const emit_at *at, uint32_t node, uint32_t *v)
{
    ksas_evalctx e;

    e.line = at->line;
    e.dot_item = at->idx;
    e.report = true;
    return ksas_expr_abs(c, &e, node, v) == KSAS_EV_OK;
}

static bool in_range(int32_t v, int32_t lo, int32_t hi)
{
    return v >= lo && v <= hi;
}

/* ---- Imediatos -------------------------------------------------------- */

static bool check_s14(ksas_ctx *c, const emit_at *at, const ksas_insn *in, uint32_t v)
{
    char name[16];
    ksas_shape sh = ksas_op_shape(in->op);

    if (ksvm_imm14_fits(ksas_s32(v))) {
        return true;
    }
    if ((in->flags & KSAS_INSN_MEMABS) != 0u) {
        ksas_error(c, at->line, at->col,
                   "endereco 0x%lX nao cabe em [zr + imm14] (-8192..8191): "
                   "use 'la rN, simbolo' e depois [rN]", (unsigned long)v);
    } else if (sh == KSAS_SH_LOAD || sh == KSAS_SH_STORE) {
        ksas_error(c, at->line, at->col,
                   "deslocamento %ld fora do alcance de imm14 (-8192..8191)", (long)ksas_s32(v));
    } else {
        ksas_error(c, at->line, at->col,
                   "imediato %ld fora do alcance de imm14 (-8192..8191) em '%s'",
                   (long)ksas_s32(v), ksas_op_lower(in->op, name, sizeof name));
    }
    return false;
}

static bool check_unsigned14(ksas_ctx *c, const emit_at *at, const ksas_insn *in, uint32_t v)
{
    char name[16];

    if (v <= 0x3FFFu) {
        return true;
    }
    ksas_error(c, at->line, at->col, "valor %ld fora do alcance de 14 bits sem sinal "
               "(0..16383) em '%s'", (long)ksas_s32(v), ksas_op_lower(in->op, name, sizeof name));
    return false;
}

/* Desvio: alvo alinhado, dentro do código e ao alcance do campo. */
static bool branch_offset(ksas_ctx *c, const emit_at *at, const ksas_insn *in,
                          uint32_t pc_next, uint32_t *v)
{
    bool b20 = (in->imm_kind == KSAS_IMM_BR20);
    int32_t off = ksas_s32(*v - pc_next);

    if ((*v & 3u) != 0u) {
        ksas_error(c, at->line, at->col, "alvo de desvio 0x%lX desalinhado (precisa ser multiplo de 4)",
                   (unsigned long)*v);
        return false;
    }
    if (*v >= c->code_size) {
        ksas_error(c, at->line, at->col, "alvo de desvio 0x%lX fora do codigo (0..0x%lX)",
                   (unsigned long)*v, (unsigned long)c->code_size);
        return false;
    }
    if (b20 ? !ksvm_off20_fits(off) : !ksvm_off24_fits(off)) {
        ksas_error(c, at->line, at->col, "desvio de %ld bytes fora do alcance de %s (%s)",
                   (long)off, b20 ? "b<cond>" : "jmp/call", b20 ? "+-2 MiB" : "+-32 MiB");
        return false;
    }
    *v = (uint32_t)off;
    return true;
}

/* Converte o valor da expressão no conteúdo do campo e verifica alcance. */
static bool encode_imm(ksas_ctx *c, const emit_at *at, const ksas_insn *in, uint8_t kind,
                       uint32_t pc_next, uint32_t *v)
{
    switch (kind) {
    case KSAS_IMM_S14:
        return check_s14(c, at, in, *v);
    case KSAS_IMM_U14:
        return check_unsigned14(c, at, in, *v);
    case KSAS_IMM_SHIFT:
        if (*v <= 31u) {
            return true;
        }
        ksas_error(c, at->line, at->col, "contagem de deslocamento %ld fora de 0..31",
                   (long)ksas_s32(*v));
        return false;
    case KSAS_IMM_32:
        if ((in->flags & KSAS_INSN_NEGATE) != 0u) {
            *v = 0u - *v;
        }
        return true;
    case KSAS_IMM_PCREL14:
        *v -= pc_next;
        return check_s14(c, at, in, *v);
    case KSAS_IMM_PCREL32:
        *v -= pc_next;
        return true;
    case KSAS_IMM_BR20:
    case KSAS_IMM_J24:
        return branch_offset(c, at, in, pc_next, v);
    default:
        return true;
    }
}

static void emit_insn(ksas_ctx *c, uint32_t idx, ksas_item *it)
{
    ksas_insn in = it->u.insn;
    emit_at at;
    uint32_t v = 0, word = 0, pc_next;
    uint8_t *p;

    if (it->size == 0u) {
        return;           /* rejeitada no layout (desalinhada): sem espaço reservado */
    }
    if ((in.flags & KSAS_INSN_LA) != 0u && c->pic) {
        in.op = KSVM_OP_ADRL;              /* .pic: la = ADRL rd, label - PC_next */
        in.imm_kind = KSAS_IMM_PCREL32;
    }
    at_init(c, idx, in.expr, &at);
    pc_next = at.addr + ksvm_isa[in.op].len;
    if (in.expr != KSAS_NONE && !final_value(c, &at, in.expr, &v)) {
        return;
    }
    if (!encode_imm(c, &at, &in, in.imm_kind, pc_next, &v)) {
        return;
    }
    switch (ksvm_isa[in.op].format) {
    case KSVM_FMT_R: word = ksvm_enc_r(in.op, in.ra, in.rb, in.rc); break;
    case KSVM_FMT_I: word = ksvm_enc_i(in.op, in.ra, in.rb, ksas_s32(v)); break;
    case KSVM_FMT_L: word = ksvm_enc_l(in.op, in.ra, in.rb); break;
    case KSVM_FMT_B: word = ksvm_enc_b(in.cond, ksas_s32(v)); break;
    default:         word = ksvm_enc_j(in.op, ksas_s32(v)); break;
    }
    p = c->code + at.addr;
    ksvm_st32le(p, word);
    if (ksvm_isa[in.op].format == KSVM_FMT_L) {
        ksvm_st32le(p + 4, v);
    }
}

/* ---- Dados ------------------------------------------------------------ */

static bool check_width(ksas_ctx *c, const emit_at *at, uint32_t v, uint8_t width)
{
    int32_t s = ksas_s32(v);

    if (width == 1u && !in_range(s, -128, 255)) {
        ksas_error(c, at->line, at->col, "valor %ld nao cabe em um byte (-128..255)", (long)s);
        return false;
    }
    if (width == 2u && !in_range(s, -32768, 65535)) {
        ksas_error(c, at->line, at->col, "valor %ld nao cabe em .half (-32768..65535)", (long)s);
        return false;
    }
    return true;
}

static void store_le(uint8_t *p, uint32_t v, uint8_t width)
{
    uint8_t i;

    for (i = 0; i < width; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static void emit_data(ksas_ctx *c, uint32_t idx, const ksas_item *it, uint8_t *p)
{
    uint8_t w = it->u.data.width;
    uint32_t k;

    for (k = 0; k < it->u.data.count; k++) {
        uint32_t node = c->elems[it->u.data.first + k], v;
        emit_at at;

        at_init(c, idx, node, &at);
        if (final_value(c, &at, node, &v) && check_width(c, &at, v, w)) {
            store_le(p + (size_t)k * w, v, w);
        }
    }
}

static void emit_space(ksas_ctx *c, uint32_t idx, const ksas_item *it, uint8_t *p)
{
    uint32_t fill = 0;
    emit_at at;

    if (it->u.space.fill == KSAS_NONE || p == NULL) {
        return;                            /* buffers já começam zerados */
    }
    at_init(c, idx, it->u.space.fill, &at);
    if (final_value(c, &at, it->u.space.fill, &fill) && check_width(c, &at, fill, 1u)) {
        memset(p, (int)(fill & 0xFFu), it->size);
    }
}

/* Avalia constantes para reportar erros mesmo nas que nunca são usadas. */
static void check_equ(ksas_ctx *c, uint32_t idx)
{
    const ksas_symbol *s = &c->sym.syms[c->items[idx].u.sym];
    ksas_evalctx e;
    ksas_value v;

    e.line = s->line;
    e.dot_item = idx;
    e.report = true;
    (void)ksas_expr_eval(c, &e, s->expr, &v);
}

/* Ponteiro para os bytes do item (NULL em .bss). */
static uint8_t *item_bytes(ksas_ctx *c, const ksas_item *it)
{
    switch (it->section) {
    case KSAS_SEC_TEXT:
    case KSAS_SEC_RODATA:
        return c->code + c->sec_base[it->section] + it->offset;
    case KSAS_SEC_DATA:
        return c->data + it->offset;
    default:
        return NULL;
    }
}

static void emit_item(ksas_ctx *c, uint32_t idx)
{
    ksas_item *it = &c->items[idx];
    uint8_t *p = item_bytes(c, it);

    c->cur_line = it->line;
    switch (it->kind) {
    case KSAS_IT_INSN:  emit_insn(c, idx, it); break;
    case KSAS_IT_DATA:  emit_data(c, idx, it, p); break;
    case KSAS_IT_SPACE: emit_space(c, idx, it, p); break;
    case KSAS_IT_EQU:   check_equ(c, idx); break;
    case KSAS_IT_BYTES:
        if (it->u.bytes.len != 0u) {       /* .ascii "": pool pode ser NULL */
            memcpy(p, c->pool + it->u.bytes.off, it->u.bytes.len);
        }
        break;
    default:
        break;                             /* LABEL, ALIGN, ORG: nada (zeros) */
    }
}

bool ksas_emit(ksas_ctx *c)
{
    uint32_t i;

    c->final = true;
    c->code = calloc((size_t)c->code_size + 1u, 1u);   /* +1: nunca malloc(0) */
    c->data = calloc((size_t)c->data_size + 1u, 1u);
    if (c->code == NULL || c->data == NULL) {
        ksas_error(c, KSAS_NONE, 0, "memoria insuficiente para a imagem");
        return false;
    }
    for (i = 0; i < c->nitems && !c->aborted; i++) {
        emit_item(c, i);
    }
    return true;
}
