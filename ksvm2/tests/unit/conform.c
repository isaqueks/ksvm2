/*
 * conform.c — executor de instrução única (ver conform.h).
 */
#include <string.h>

#include "conform.h"

const uint32_t cf_vals[] = {
    0x00000000u, 0x00000001u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u,
    0x00000002u, 0xFFFFFFFEu, 0x7FFFFFFEu, 0x80000001u,
    0x0000FFFFu, 0x00010000u, 0x000000FFu, 0x00000080u, 0x00008000u,
    0x12345678u, 0xDEADBEEFu, 0x55555555u, 0xAAAAAAAAu,
};
const unsigned cf_nvals = sizeof cf_vals / sizeof cf_vals[0];

const uint32_t cf_flag_sets[] = {
    0u, KSVM_FLAGS_MASK, KSVM_FLAG_Z | KSVM_FLAG_C, KSVM_FLAG_N | KSVM_FLAG_V,
};
const unsigned cf_nflag_sets = sizeof cf_flag_sets / sizeof cf_flag_sets[0];

uint32_t cf_sentinel(unsigned i)
{
    return UINT32_C(0xC0DE0000) | ((uint32_t)i << 8) | (uint32_t)i;
}

unsigned cf_len(uint32_t w0)
{
    return ksvm_isa_get(w0 & 0xFFu).len == 8u ? 8u : 4u;
}

void cf_prepare(fixture *f, uint32_t w0, uint32_t w1, uint32_t a, uint32_t b, uint32_t fin)
{
    uint32_t prog[4];
    unsigned i;

    prog[0] = w0;
    prog[1] = (cf_len(w0) == 8u) ? w1 : HALT;
    prog[2] = HALT;
    prog[3] = HALT;
    fix_load(f, prog, 4u);
    for (i = 1; i < 31u; i++) {
        f->vm.cpu.r[i] = cf_sentinel(i);
    }
    f->vm.cpu.r[CF_RA] = a;
    f->vm.cpu.r[CF_RB] = b;
    f->vm.cpu.flags = fin;
}

static int ram_is_zero(const fixture *f)
{
    unsigned i;
    for (i = 0; i < FIX_RAM_SIZE; i++) {
        if (f->ram[i] != 0u) return 0;
    }
    return 1;
}

static void note_case(test_ctx *t_, const cf_case *c)
{
    test_note(t_, "%s w0=0x%08lX a=0x%08lX b=0x%08lX flags_in=0x%lX",
              c->what, (unsigned long)c->w0, (unsigned long)c->a,
              (unsigned long)c->b, (unsigned long)c->fin);
}

void cf_run(test_ctx *t_, const cf_case *c)
{
    fixture f;
    ksvm_cpu before, want;
    unsigned len = cf_len(c->w0);

    cf_prepare(&f, c->w0, c->w1, c->a, c->b, c->fin);
    before = f.vm.cpu;
    want = before;
    if (c->rd < 32u) {
        want.r[c->rd] = c->v;
    }
    want.r[0] = 0u;
    want.flags = c->fl;
    want.pc = len;

    (void)ksvm_step(&f.vm);
    if (f.vm.status != KSVM_RUNNING || f.vm.insn_count != 1u ||
        memcmp(&want, &f.vm.cpu, sizeof want) != 0 || !ram_is_zero(&f)) {
        note_case(t_, c);
        if (f.vm.status == KSVM_FAULT) {
            test_note(t_, "fault inesperado %s addr=0x%08lX", ksvm_fault_name(f.vm.fault),
                      (unsigned long)f.vm.fault_addr);
        }
    }
    ASSERT_EQ(f.vm.status, KSVM_RUNNING);
    ASSERT_EQ_U32(f.vm.insn_count, 1u);
    ASSERT_EQ_U32(f.vm.cpu.pc, len);
    if (c->rd < 32u) {
        ASSERT_EQ_U32(f.vm.cpu.r[c->rd], c->v);
    }
    ASSERT_EQ_U32(f.vm.cpu.flags, c->fl);
    ASSERT_MEM_EQ(&want, &f.vm.cpu, sizeof want);   /* nenhum outro registrador */
    ASSERT_TRUE(ram_is_zero(&f));
}

void cf_run_fault(test_ctx *t_, const cf_case *c, ksvm_fault fault, uint32_t addr)
{
    fixture f;
    ksvm_cpu before;

    cf_prepare(&f, c->w0, c->w1, c->a, c->b, c->fin);
    before = f.vm.cpu;
    (void)ksvm_step(&f.vm);
    if (f.vm.status != KSVM_FAULT || f.vm.fault != fault || f.vm.fault_addr != addr ||
        memcmp(&before, &f.vm.cpu, sizeof before) != 0) {
        note_case(t_, c);
    }
    ASSERT_EQ(f.vm.status, KSVM_FAULT);
    ASSERT_EQ(f.vm.fault, fault);
    ASSERT_EQ_U32(f.vm.fault_addr, addr);
    ASSERT_EQ_U32(f.vm.insn_count, 0u);
    ASSERT_MEM_EQ(&before, &f.vm.cpu, sizeof before);
    ASSERT_TRUE(ram_is_zero(&f));
    ASSERT_EQ(ksvm_step(&f.vm), KSVM_FAULT);
    ASSERT_MEM_EQ(&before, &f.vm.cpu, sizeof before);
}

/* ---- Verificação em massa -------------------------------------------- */

const int32_t cf_imms[] = {
    0, 1, -1, 2, -2, 31, 32, 33, 63, 64, -31, -32, -33,
    8191, -8192, 8190, -8191, 0x1000, -0x1000, 0xFF, 0x80, -0x80, 0x1555, -0x1556,
};
const unsigned cf_nimms = sizeof cf_imms / sizeof cf_imms[0];

/* Instrução e registradores iniciais conforme o formato. */
static uint32_t enc_bin(const cf_bin *d, uint8_t rd, uint8_t ra, uint8_t rb, int32_t imm)
{
    int cmp = (d->opts & CF_CMP) != 0u;
    switch (d->fmt) {
    case CF_FMT_R: return cmp ? ksvm_enc_r(d->op, ra, rb, 0) : ksvm_enc_r(d->op, rd, ra, rb);
    case CF_FMT_I: return cmp ? ksvm_enc_i(d->op, ra, 0, imm) : ksvm_enc_i(d->op, rd, ra, imm);
    default:       return cmp ? ksvm_enc_l(d->op, ra, 0) : ksvm_enc_l(d->op, rd, ra);
    }
}

static void one_binary(test_ctx *t_, const cf_bin *d, uint32_t w0, uint32_t w1,
                       uint32_t a, uint32_t r3, uint32_t opa, uint32_t opb,
                       uint32_t fin, unsigned rd)
{
    cf_case c;
    o_res r;

    c.what = d->name;
    c.w0 = w0;
    c.w1 = w1;
    c.a = a;
    c.b = r3;
    c.fin = fin;
    if ((d->opts & CF_DIV) != 0u && opb == 0u) {
        cf_run_fault(t_, &c, KSVM_FAULT_DIV_ZERO, 0u);
        return;
    }
    r = d->fn(opa, opb, fin);
    c.rd = ((d->opts & CF_CMP) != 0u) ? CF_NONE : rd;
    c.v = r.v;
    c.fl = r.flags;
    cf_run(t_, &c);
}

void cf_check_binary(test_ctx *t_, const cf_bin *d)
{
    const uint32_t *bs = d->bset ? d->bset : cf_vals;
    unsigned nb = d->bset ? d->nb : cf_nvals;
    const int32_t *is = d->iset ? d->iset : cf_imms;
    unsigned ni = d->iset ? d->ni : cf_nimms;
    unsigned ia, ib, ifl;

    for (ia = 0; ia < cf_nvals; ia++) {
        uint32_t a = cf_vals[ia];
        unsigned n = (d->fmt == CF_FMT_I) ? ni : nb;
        for (ib = 0; ib < n; ib++) {
            for (ifl = 0; ifl < cf_nflag_sets; ifl++) {
                uint32_t fin = cf_flag_sets[ifl];
                if (d->fmt == CF_FMT_I) {
                    uint32_t imm = (uint32_t)is[ib];
                    /* r3 com lixo: o imediato não pode vir de registrador */
                    one_binary(t_, d, enc_bin(d, CF_RD, CF_RA, CF_RB, is[ib]), 0u,
                               a, UINT32_C(0x13579BDF), a, imm, fin, CF_RD);
                } else if (d->fmt == CF_FMT_L) {
                    one_binary(t_, d, enc_bin(d, CF_RD, CF_RA, CF_RB, 0), bs[ib],
                               a, UINT32_C(0x13579BDF), a, bs[ib], fin, CF_RD);
                } else {
                    one_binary(t_, d, enc_bin(d, CF_RD, CF_RA, CF_RB, 0), 0u,
                               a, bs[ib], a, bs[ib], fin, CF_RD);
                }
                if (TEST_FAILED()) return;
            }
        }
    }
}

void cf_check_aliasing(test_ctx *t_, const cf_bin *d)
{
    unsigned ia, ib;
    int cmp = (d->opts & CF_CMP) != 0u;

    for (ia = 0; ia < cf_nvals; ia++) {
        for (ib = 0; ib < cf_nvals; ib += 3) {
            uint32_t a = cf_vals[ia], b = cf_vals[ib];
            uint32_t fin = cf_flag_sets[(ia + ib) % cf_nflag_sets];
            if (!cmp) {
                /* rd = ra */
                one_binary(t_, d, enc_bin(d, CF_RA, CF_RA, CF_RB, 0), 0u, a, b, a, b, fin, CF_RA);
                if (TEST_FAILED()) return;
                /* rd = rb */
                one_binary(t_, d, enc_bin(d, CF_RB, CF_RA, CF_RB, 0), 0u, a, b, a, b, fin, CF_RB);
                if (TEST_FAILED()) return;
                /* rd = ra = rb */
                one_binary(t_, d, enc_bin(d, CF_RA, CF_RA, CF_RA, 0), 0u, a, b, a, a, fin, CF_RA);
                if (TEST_FAILED()) return;
            }
            /* ra = rb */
            one_binary(t_, d, enc_bin(d, CF_RD, CF_RA, CF_RA, 0), 0u, a, b, a, a, fin, CF_RD);
            if (TEST_FAILED()) return;
            /* zr como fonte (lido como 0) */
            one_binary(t_, d, enc_bin(d, CF_RD, 0, CF_RB, 0), 0u, a, b, 0u, b, fin, CF_RD);
            if (TEST_FAILED()) return;
            one_binary(t_, d, enc_bin(d, CF_RD, CF_RA, 0, 0), 0u, a, b, a, 0u, fin, CF_RD);
            if (TEST_FAILED()) return;
        }
    }
}

void cf_check_unary(test_ctx *t_, const char *name, uint8_t op, cf_un_fn fn)
{
    unsigned ib, ifl;

    for (ib = 0; ib < cf_nvals; ib++) {
        for (ifl = 0; ifl < cf_nflag_sets; ifl++) {
            cf_case c;
            o_res r = fn(cf_vals[ib], cf_flag_sets[ifl]);
            c.what = name;
            c.w0 = ksvm_enc_r(op, CF_RD, CF_RB, 0);
            c.w1 = 0u;
            c.a = UINT32_C(0x2468ACE0);    /* ra não é operando */
            c.b = cf_vals[ib];
            c.fin = cf_flag_sets[ifl];
            c.rd = CF_RD;
            c.v = r.v;
            c.fl = r.flags;
            cf_run(t_, &c);
            if (TEST_FAILED()) return;
            /* rd = rb */
            c.w0 = ksvm_enc_r(op, CF_RB, CF_RB, 0);
            c.rd = CF_RB;
            cf_run(t_, &c);
            if (TEST_FAILED()) return;
        }
    }
}
