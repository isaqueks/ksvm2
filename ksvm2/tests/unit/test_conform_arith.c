/*
 * test_conform_arith.c — conformidade da aritmética inteira com o doc 04
 * §5.4, instrução por instrução, contra o oráculo independente (oracle.h).
 *
 * Cada operação é executada pela VM (decode + exec + alu) com todos os
 * pares de cf_vals (0, 1, -1, INT32_MAX, INT32_MIN, UINT32_MAX, vizinhos e
 * padrões) e quatro FLAGS de entrada; confere resultado, cada flag, que as
 * flags não afetadas são preservadas e que nada mais mudou.
 */
#include "conform.h"

static uint32_t cin(uint32_t fin) { return (fin & O_C) ? 1u : 0u; }

static o_res f_add(uint32_t a, uint32_t b, uint32_t fin) { (void)fin; return o_add(a, b, 0u); }
static o_res f_adc(uint32_t a, uint32_t b, uint32_t fin) { return o_add(a, b, cin(fin)); }
static o_res f_sub(uint32_t a, uint32_t b, uint32_t fin) { (void)fin; return o_sub(a, b, 0u); }
/* SBC: rd = ra - rb - (1 - C) */
static o_res f_sbc(uint32_t a, uint32_t b, uint32_t fin) { return o_sub(a, b, 1u - cin(fin)); }
static o_res f_neg(uint32_t b, uint32_t fin) { (void)fin; return o_sub(0u, b, 0u); }

#if KSVM_ENABLE_MULDIV
static o_res zn_only(uint32_t v, uint32_t fin)
{
    o_res r;
    r.v = v;
    r.flags = o_zn(v, fin);       /* C e V preservados */
    return r;
}

static o_res f_mul(uint32_t a, uint32_t b, uint32_t fin)   { return zn_only(o_mul(a, b), fin); }
static o_res f_mulh(uint32_t a, uint32_t b, uint32_t fin)  { return zn_only(o_mulh(a, b), fin); }
static o_res f_mulhu(uint32_t a, uint32_t b, uint32_t fin) { return zn_only(o_mulhu(a, b), fin); }
static o_res f_div(uint32_t a, uint32_t b, uint32_t fin)   { return zn_only(o_div(a, b), fin); }
static o_res f_divu(uint32_t a, uint32_t b, uint32_t fin)  { return zn_only(o_divu(a, b), fin); }
static o_res f_rem(uint32_t a, uint32_t b, uint32_t fin)   { return zn_only(o_rem(a, b), fin); }
static o_res f_remu(uint32_t a, uint32_t b, uint32_t fin)  { return zn_only(o_remu(a, b), fin); }
#endif

#define BIN(nm, fmt, fn, opts) \
    { #nm, KSVM_OP_##nm, CF_FMT_##fmt, fn, opts, NULL, 0u, NULL, 0u }

static const cf_bin arith_ops[] = {
    BIN(ADD,   R, f_add,   0u),
    BIN(ADDI,  I, f_add,   0u),
    BIN(ADDL,  L, f_add,   0u),
    BIN(SUB,   R, f_sub,   0u),
    BIN(SUBI,  I, f_sub,   0u),
    BIN(ADC,   R, f_adc,   0u),
    BIN(SBC,   R, f_sbc,   0u),
    BIN(CMP,   R, f_sub,   CF_CMP),
    BIN(CMPI,  I, f_sub,   CF_CMP),
    BIN(CMPL,  L, f_sub,   CF_CMP),
#if KSVM_ENABLE_MULDIV
    BIN(MUL,   R, f_mul,   0u),
    BIN(MULI,  I, f_mul,   0u),
    BIN(MULH,  R, f_mulh,  0u),
    BIN(MULHU, R, f_mulhu, 0u),
    BIN(DIV,   R, f_div,   CF_DIV),
    BIN(DIVU,  R, f_divu,  CF_DIV),
    BIN(REM,   R, f_rem,   CF_DIV),
    BIN(REMU,  R, f_remu,  CF_DIV),
#endif
};
#define N_ARITH (sizeof arith_ops / sizeof arith_ops[0])

static const cf_bin *find_op(uint8_t op)
{
    unsigned i;
    for (i = 0; i < N_ARITH; i++) {
        if (arith_ops[i].op == op) return &arith_ops[i];
    }
    return NULL;
}

#define CHECK_OP(nm)                                   \
    TEST(conform_##nm)                                 \
    {                                                  \
        const cf_bin *d = find_op(KSVM_OP_##nm);       \
        ASSERT_TRUE(d != NULL);                        \
        cf_check_binary(t_, d);                        \
    }

CHECK_OP(ADD)
CHECK_OP(ADDI)
CHECK_OP(ADDL)
CHECK_OP(SUB)
CHECK_OP(SUBI)
CHECK_OP(ADC)
CHECK_OP(SBC)
CHECK_OP(CMP)
CHECK_OP(CMPI)
CHECK_OP(CMPL)
#if KSVM_ENABLE_MULDIV
CHECK_OP(MUL)
CHECK_OP(MULI)
CHECK_OP(MULH)
CHECK_OP(MULHU)
CHECK_OP(DIV)
CHECK_OP(DIVU)
CHECK_OP(REM)
CHECK_OP(REMU)
#endif

TEST(conform_NEG)
{
    cf_check_unary(t_, "NEG", KSVM_OP_NEG, f_neg);
}

/* Todas as R de três operandos com destino/fonte repetidos e zr como fonte. */
TEST(aliasing_all_r_forms)
{
    unsigned i;
    for (i = 0; i < N_ARITH; i++) {
        if (arith_ops[i].fmt == CF_FMT_R) {
            cf_check_aliasing(t_, &arith_ops[i]);
            if (TEST_FAILED()) return;
        }
    }
}

/* Casos explícitos do doc 04: os sinais nas 4 combinações e INT32_MIN/-1. */
TEST(div_rem_sign_table)
{
#if KSVM_ENABLE_MULDIV
    static const struct { int32_t a, b, q, r; } t[] = {
        {  7,  2,  3,  1 }, { -7,  2, -3, -1 }, {  7, -2, -3,  1 }, { -7, -2,  3, -1 },
        {  6,  3,  2,  0 }, { -6,  3, -2,  0 }, {  6, -3, -2,  0 }, { -6, -3,  2,  0 },
        {  1,  2,  0,  1 }, { -1,  2,  0, -1 }, {  1, -2,  0,  1 }, { -1, -2,  0, -1 },
        { INT32_MIN, -1, INT32_MIN, 0 }, { INT32_MIN, 1, INT32_MIN, 0 },
        { INT32_MIN, 2, -0x40000000, 0 }, { INT32_MIN, INT32_MIN, 1, 0 },
        { INT32_MAX, -1, -INT32_MAX, 0 }, { INT32_MAX, INT32_MIN, 0, INT32_MAX },
        { INT32_MIN, INT32_MAX, -1, -1 }, { INT32_MIN, 3, -715827882, -2 },
    };
    unsigned i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        cf_case c;
        c.what = "DIV";
        c.w0 = OP_R(DIV, CF_RD, CF_RA, CF_RB);
        c.w1 = 0u;
        c.a = (uint32_t)t[i].a;
        c.b = (uint32_t)t[i].b;
        c.fin = O_C | O_V;
        c.rd = CF_RD;
        c.v = (uint32_t)t[i].q;
        c.fl = o_zn(c.v, O_C | O_V);
        cf_run(t_, &c);
        if (TEST_FAILED()) return;
        c.what = "REM";
        c.w0 = OP_R(REM, CF_RD, CF_RA, CF_RB);
        c.v = (uint32_t)t[i].r;
        c.fl = o_zn(c.v, O_C | O_V);
        cf_run(t_, &c);
        if (TEST_FAILED()) return;
    }
#else
    (void)t_;
#endif
}

/* Divisor zero: fault DIV_ZERO, addr = pc, nada muda (nem flags nem rd),
 * inclusive com o divisor vindo de zr. */
TEST(div_by_zero_has_no_side_effect)
{
#if KSVM_ENABLE_MULDIV
    static const uint8_t ops[] = { KSVM_OP_DIV, KSVM_OP_DIVU, KSVM_OP_REM, KSVM_OP_REMU };
    unsigned i, j, k;
    for (i = 0; i < sizeof ops; i++) {
        for (j = 0; j < cf_nvals; j++) {
            for (k = 0; k < cf_nflag_sets; k++) {
                cf_case c;
                c.what = ksvm_op_name(ops[i]);
                c.w0 = ksvm_enc_r(ops[i], CF_RD, CF_RA, CF_RB);
                c.w1 = 0u;
                c.a = cf_vals[j];
                c.b = 0u;
                c.fin = cf_flag_sets[k];
                c.rd = CF_RD;
                c.v = 0u;
                c.fl = 0u;
                cf_run_fault(t_, &c, KSVM_FAULT_DIV_ZERO, 0u);
                if (TEST_FAILED()) return;
                c.w0 = ksvm_enc_r(ops[i], CF_RA, CF_RA, 0);   /* divisor = zr */
                cf_run_fault(t_, &c, KSVM_FAULT_DIV_ZERO, 0u);
                if (TEST_FAILED()) return;
            }
        }
    }
#else
    (void)t_;
#endif
}

/* ADC/SBC são a base da aritmética multipalavra: C de entrada explícito
 * nos dois valores, inclusive a + b + 1 = 2^32 exato. */
TEST(adc_sbc_carry_edges)
{
    static const struct { uint8_t op; uint32_t a, b, c, v, fl; } t[] = {
        { KSVM_OP_ADC, 0xFFFFFFFFu, 0u, 1u, 0u, O_Z | O_C },
        { KSVM_OP_ADC, 0xFFFFFFFFu, 0xFFFFFFFFu, 1u, 0xFFFFFFFFu, O_N | O_C },
        { KSVM_OP_ADC, 0x7FFFFFFFu, 0u, 1u, 0x80000000u, O_N | O_V },
        { KSVM_OP_ADC, 0x80000000u, 0xFFFFFFFFu, 1u, 0x80000000u, O_N | O_C },
        { KSVM_OP_ADC, 0x80000000u, 0x80000000u, 0u, 0u, O_Z | O_C | O_V },
        { KSVM_OP_SBC, 0u, 0u, 0u, 0xFFFFFFFFu, O_N },
        { KSVM_OP_SBC, 0u, 0u, 1u, 0u, O_Z | O_C },
        { KSVM_OP_SBC, 0x80000000u, 0u, 0u, 0x7FFFFFFFu, O_C | O_V },
        { KSVM_OP_SBC, 0x80000000u, 0xFFFFFFFFu, 0u, 0x80000000u, O_N },
        { KSVM_OP_SBC, 0x7FFFFFFFu, 0xFFFFFFFFu, 1u, 0x80000000u, O_N | O_V },
        { KSVM_OP_SBC, 5u, 5u, 0u, 0xFFFFFFFFu, O_N },
        { KSVM_OP_SBC, 5u, 4u, 0u, 0u, O_Z | O_C },
    };
    unsigned i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        cf_case c;
        c.what = ksvm_op_name(t[i].op);
        c.w0 = ksvm_enc_r(t[i].op, CF_RD, CF_RA, CF_RB);
        c.w1 = 0u;
        c.a = t[i].a;
        c.b = t[i].b;
        c.fin = t[i].c ? O_C : 0u;
        c.rd = CF_RD;
        c.v = t[i].v;
        c.fl = t[i].fl;
        cf_run(t_, &c);
        if (TEST_FAILED()) return;
    }
}

SUITE(conform_arith)
{
    RUN_TEST(conform_ADD);
    RUN_TEST(conform_ADDI);
    RUN_TEST(conform_ADDL);
    RUN_TEST(conform_SUB);
    RUN_TEST(conform_SUBI);
    RUN_TEST(conform_ADC);
    RUN_TEST(conform_SBC);
    RUN_TEST(conform_NEG);
    RUN_TEST(conform_CMP);
    RUN_TEST(conform_CMPI);
    RUN_TEST(conform_CMPL);
#if KSVM_ENABLE_MULDIV
    RUN_TEST(conform_MUL);
    RUN_TEST(conform_MULI);
    RUN_TEST(conform_MULH);
    RUN_TEST(conform_MULHU);
    RUN_TEST(conform_DIV);
    RUN_TEST(conform_DIVU);
    RUN_TEST(conform_REM);
    RUN_TEST(conform_REMU);
#endif
    RUN_TEST(aliasing_all_r_forms);
    RUN_TEST(div_rem_sign_table);
    RUN_TEST(div_by_zero_has_no_side_effect);
    RUN_TEST(adc_sbc_carry_edges);
}
