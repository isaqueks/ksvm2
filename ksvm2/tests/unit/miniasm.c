/*
 * miniasm.c — montador mínimo com rótulos (ver miniasm.h).
 */
#include <string.h>

#include "miniasm.h"

enum { MA_B, MA_J, MA_ADR, MA_ADRL, MA_ABS, MA_DIFF };

void ma_init(ma *a)
{
    unsigned i;
    memset(a, 0, sizeof *a);
    for (i = 0; i < MA_MAX_LABELS; i++) a->lab[i] = -1;
}

void ma_emit(ma *a, uint32_t w)
{
    if (a->n >= MA_MAX_WORDS) {
        a->err = 1;
        return;
    }
    a->w[a->n++] = w;
}

void ma_emit_l(ma *a, uint32_t w0, uint32_t imm32)
{
    ma_emit(a, w0);
    ma_emit(a, imm32);
}

void ma_label(ma *a, unsigned id)
{
    if (id >= MA_MAX_LABELS || a->lab[id] >= 0) {
        a->err = 1;
        return;
    }
    a->lab[id] = (long)a->n;
}

static void add_fix(ma *a, int kind, unsigned id, unsigned base_lab, uint32_t base)
{
    if (a->nfix >= MA_MAX_FIXUPS || id >= MA_MAX_LABELS || base_lab >= MA_MAX_LABELS) {
        a->err = 1;
        return;
    }
    a->fix[a->nfix].at = a->n;
    a->fix[a->nfix].lab = id;
    a->fix[a->nfix].base_lab = base_lab;
    a->fix[a->nfix].base = base;
    a->fix[a->nfix].kind = kind;
    a->nfix++;
}

void ma_b(ma *a, uint8_t cond, unsigned id)
{
    add_fix(a, MA_B, id, 0, 0);
    ma_emit(a, ksvm_enc_b(cond, 0));
}

void ma_j(ma *a, uint8_t op, unsigned id)
{
    add_fix(a, MA_J, id, 0, 0);
    ma_emit(a, ksvm_enc_j(op, 0));
}

void ma_adr(ma *a, uint8_t rd, unsigned id)
{
    add_fix(a, MA_ADR, id, 0, 0);
    ma_emit(a, ksvm_enc_i(KSVM_OP_ADR, rd, 0, 0));
}

void ma_adrl(ma *a, uint8_t rd, unsigned id)
{
    add_fix(a, MA_ADRL, id, 0, 0);
    ma_emit_l(a, ksvm_enc_l(KSVM_OP_ADRL, rd, 0), 0);
}

void ma_word_abs(ma *a, unsigned id, uint32_t base)
{
    add_fix(a, MA_ABS, id, 0, base);
    ma_emit(a, 0);
}

void ma_word_diff(ma *a, unsigned id, unsigned base_id)
{
    add_fix(a, MA_DIFF, id, base_id, 0);
    ma_emit(a, 0);
}

uint32_t ma_addr(const ma *a, unsigned id)
{
    return (id < MA_MAX_LABELS && a->lab[id] >= 0) ? (uint32_t)a->lab[id] * 4u : 0xFFFFFFFFu;
}

int ma_link(ma *a)
{
    unsigned i;
    for (i = 0; i < a->nfix && !a->err; i++) {
        const ma_fix *x = &a->fix[i];
        int32_t target, next;
        if (a->lab[x->lab] < 0 || (x->kind == MA_DIFF && a->lab[x->base_lab] < 0)) {
            a->err = 1;
            break;
        }
        target = (int32_t)a->lab[x->lab] * 4;
        next = (int32_t)x->at * 4 + 4;
        switch (x->kind) {
        case MA_B:
            if (!ksvm_off20_fits(target - next)) a->err = 1;
            a->w[x->at] = ksvm_enc_b(ksvm_field_cond(a->w[x->at]), target - next);
            break;
        case MA_J:
            if (!ksvm_off24_fits(target - next)) a->err = 1;
            a->w[x->at] = ksvm_enc_j(ksvm_field_op(a->w[x->at]), target - next);
            break;
        case MA_ADR:
            if (!ksvm_imm14_fits(target - next)) a->err = 1;
            a->w[x->at] = ksvm_enc_i(KSVM_OP_ADR, ksvm_field_ra(a->w[x->at]), 0, target - next);
            break;
        case MA_ADRL:
            a->w[x->at + 1u] = (uint32_t)(target - (next + 4));    /* PC_next = at + 8 */
            break;
        case MA_ABS:
            a->w[x->at] = x->base + (uint32_t)target;
            break;
        default:
            a->w[x->at] = (uint32_t)(target - (int32_t)a->lab[x->base_lab] * 4);
            break;
        }
    }
    return a->err ? -1 : 0;
}
