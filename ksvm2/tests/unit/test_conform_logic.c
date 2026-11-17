/*
 * test_conform_logic.c — conformidade de lógica, deslocamentos, rotações,
 * extensões e contagem de bits (doc 04 §5.5) contra o oráculo (oracle.h).
 *
 * Deslocamentos: contagens 0, 1, 31, 32, 33, 63, 0xFFFFFFFF... (sempre
 * & 31); o oráculo desloca um bit por vez, então C = último bit que saiu
 * sai da simulação, não de uma fórmula. ZN: C e V preservados; ZNC: V
 * preservado.
 */
#include "conform.h"

static o_res zn(uint32_t v, uint32_t fin)
{
    o_res r;
    r.v = v;
    r.flags = o_zn(v, fin);
    return r;
}

static o_res f_and(uint32_t a, uint32_t b, uint32_t fin) { return zn(a & b, fin); }
static o_res f_or(uint32_t a, uint32_t b, uint32_t fin)  { return zn(a | b, fin); }
static o_res f_xor(uint32_t a, uint32_t b, uint32_t fin) { return zn(a ^ b, fin); }

static o_res f_shl(uint32_t a, uint32_t b, uint32_t fin) { return o_shift(O_SHL, a, b, fin); }
static o_res f_shr(uint32_t a, uint32_t b, uint32_t fin) { return o_shift(O_SHR, a, b, fin); }
static o_res f_sar(uint32_t a, uint32_t b, uint32_t fin) { return o_shift(O_SAR, a, b, fin); }
static o_res f_rol(uint32_t a, uint32_t b, uint32_t fin) { return o_shift(O_ROL, a, b, fin); }
static o_res f_ror(uint32_t a, uint32_t b, uint32_t fin) { return o_shift(O_ROR, a, b, fin); }

static o_res u_not(uint32_t b, uint32_t fin) { return zn(~b, fin); }
static o_res u_sxb(uint32_t b, uint32_t fin) { return zn(o_sext(b, 8), fin); }
static o_res u_sxh(uint32_t b, uint32_t fin) { return zn(o_sext(b, 16), fin); }
static o_res u_zxb(uint32_t b, uint32_t fin) { return zn(b % 256u, fin); }
static o_res u_zxh(uint32_t b, uint32_t fin) { return zn(b % 65536u, fin); }
#if KSVM_ENABLE_BITOPS
static o_res u_clz(uint32_t b, uint32_t fin) { return zn(o_clz(b), fin); }
static o_res u_ctz(uint32_t b, uint32_t fin) { return zn(o_ctz(b), fin); }
static o_res u_pop(uint32_t b, uint32_t fin) { return zn(o_popcnt(b), fin); }
#endif

/* Contagens de deslocamento por registrador e por imediato. */
static const uint32_t shift_counts[] = {
    0u, 1u, 2u, 7u, 15u, 16u, 30u, 31u, 32u, 33u, 63u, 64u, 65u,
    0xFFFFFFFFu, 0xFFFFFFE0u, 0xFFFFFFE1u, 0x80000000u, 0x7FFFFFFFu, 0x100u, 0x11Fu,
};
static const int32_t shift_imms[] = {
    0, 1, 2, 15, 16, 31, 32, 33, 63, 64, -1, -31, -32, -33, 8191, -8192, 0x1FE1,
};

#define BIN(nm, fmt, fn, opts) \
    { #nm, KSVM_OP_##nm, CF_FMT_##fmt, fn, opts, NULL, 0u, NULL, 0u }
#define SHIFT(nm, fmt, fn) \
    { #nm, KSVM_OP_##nm, CF_FMT_##fmt, fn, 0u, \
      shift_counts, sizeof shift_counts / sizeof shift_counts[0], \
      shift_imms, sizeof shift_imms / sizeof shift_imms[0] }

static const cf_bin logic_ops[] = {
    BIN(AND,  R, f_and, 0u),
    BIN(ANDI, I, f_and, 0u),
    BIN(ANDL, L, f_and, 0u),
    BIN(OR,   R, f_or,  0u),
    BIN(ORI,  I, f_or,  0u),
    BIN(ORL,  L, f_or,  0u),
    BIN(XOR,  R, f_xor, 0u),
    BIN(XORI, I, f_xor, 0u),
    BIN(XORL, L, f_xor, 0u),
    BIN(TST,  R, f_and, CF_CMP),
    BIN(TSTI, I, f_and, CF_CMP),
    SHIFT(SHL,  R, f_shl),
    SHIFT(SHLI, I, f_shl),
    SHIFT(SHR,  R, f_shr),
    SHIFT(SHRI, I, f_shr),
    SHIFT(SAR,  R, f_sar),
    SHIFT(SARI, I, f_sar),
    SHIFT(ROL,  R, f_rol),
    SHIFT(ROR,  R, f_ror),
};
#define N_LOGIC (sizeof logic_ops / sizeof logic_ops[0])

static const cf_bin *find_op(uint8_t op)
{
    unsigned i;
    for (i = 0; i < N_LOGIC; i++) {
        if (logic_ops[i].op == op) return &logic_ops[i];
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

CHECK_OP(AND)
CHECK_OP(ANDI)
CHECK_OP(ANDL)
CHECK_OP(OR)
CHECK_OP(ORI)
CHECK_OP(ORL)
CHECK_OP(XOR)
CHECK_OP(XORI)
CHECK_OP(XORL)
CHECK_OP(TST)
CHECK_OP(TSTI)
CHECK_OP(SHL)
CHECK_OP(SHLI)
CHECK_OP(SHR)
CHECK_OP(SHRI)
CHECK_OP(SAR)
CHECK_OP(SARI)
CHECK_OP(ROL)
CHECK_OP(ROR)

TEST(conform_NOT) { cf_check_unary(t_, "NOT", KSVM_OP_NOT, u_not); }
TEST(conform_SXB) { cf_check_unary(t_, "SXB", KSVM_OP_SXB, u_sxb); }
TEST(conform_SXH) { cf_check_unary(t_, "SXH", KSVM_OP_SXH, u_sxh); }
TEST(conform_ZXB) { cf_check_unary(t_, "ZXB", KSVM_OP_ZXB, u_zxb); }
TEST(conform_ZXH) { cf_check_unary(t_, "ZXH", KSVM_OP_ZXH, u_zxh); }
#if KSVM_ENABLE_BITOPS
TEST(conform_CLZ)    { cf_check_unary(t_, "CLZ", KSVM_OP_CLZ, u_clz); }
TEST(conform_CTZ)    { cf_check_unary(t_, "CTZ", KSVM_OP_CTZ, u_ctz); }
TEST(conform_POPCNT) { cf_check_unary(t_, "POPCNT", KSVM_OP_POPCNT, u_pop); }

/* Cada bit isolado e cada máscara de prefixo/sufixo. */
TEST(bitops_every_bit_position)
{
    unsigned i;
    for (i = 0; i < 32u; i++) {
        uint32_t one = UINT32_C(1) << i;
        uint32_t low = (one - 1u) | one;          /* bits 0..i */
        uint32_t vals[3];
        unsigned k;
        vals[0] = one;
        vals[1] = low;
        vals[2] = ~(one - 1u);                    /* bits i..31 */
        for (k = 0; k < 3u; k++) {
            static const uint8_t ops[] = { KSVM_OP_CLZ, KSVM_OP_CTZ, KSVM_OP_POPCNT };
            unsigned j;
            for (j = 0; j < 3u; j++) {
                cf_case c;
                uint32_t v = (j == 0) ? o_clz(vals[k]) : (j == 1) ? o_ctz(vals[k]) : o_popcnt(vals[k]);
                c.what = ksvm_op_name(ops[j]);
                c.w0 = ksvm_enc_r(ops[j], CF_RD, CF_RB, 0);
                c.w1 = 0u;
                c.a = 0u;
                c.b = vals[k];
                c.fin = O_C | O_V;
                c.rd = CF_RD;
                c.v = v;
                c.fl = o_zn(v, O_C | O_V);
                cf_run(t_, &c);
                if (TEST_FAILED()) return;
            }
        }
    }
}
#endif

/* Todas as contagens 0..63 com padrões que expõem cada bit que sai. */
TEST(shifts_every_count)
{
    static const uint32_t pats[] = { 0x80000001u, 0x40000002u, 0xFFFFFFFFu, 0x7FFFFFFFu,
                                     0x80000000u, 0x00000001u, 0xA5A5A5A5u };
    static const struct { uint8_t r, i; int kind; } ops[] = {
        { KSVM_OP_SHL, KSVM_OP_SHLI, O_SHL }, { KSVM_OP_SHR, KSVM_OP_SHRI, O_SHR },
        { KSVM_OP_SAR, KSVM_OP_SARI, O_SAR }, { KSVM_OP_ROL, 0, O_ROL },
        { KSVM_OP_ROR, 0, O_ROR },
    };
    unsigned p, n, k;
    for (k = 0; k < sizeof ops / sizeof ops[0]; k++) {
        for (p = 0; p < sizeof pats / sizeof pats[0]; p++) {
            for (n = 0; n < 64u; n++) {
                cf_case c;
                o_res r = o_shift(ops[k].kind, pats[p], n, O_V | O_C);
                c.what = ksvm_op_name(ops[k].r);
                c.w0 = ksvm_enc_r(ops[k].r, CF_RD, CF_RA, CF_RB);
                c.w1 = 0u;
                c.a = pats[p];
                c.b = n;
                c.fin = O_V | O_C;
                c.rd = CF_RD;
                c.v = r.v;
                c.fl = r.flags;
                cf_run(t_, &c);
                if (TEST_FAILED()) return;
                if (ops[k].i != 0u) {
                    c.what = ksvm_op_name(ops[k].i);
                    c.w0 = ksvm_enc_i(ops[k].i, CF_RD, CF_RA, (int32_t)n);
                    c.b = 0xFFFFFFFFu;            /* rb não é usado */
                    cf_run(t_, &c);
                    if (TEST_FAILED()) return;
                }
            }
        }
    }
}

/* Valores literais do doc (independentes até do oráculo). */
TEST(shift_literal_examples)
{
    static const struct { uint8_t op; uint32_t a, n, v, fl; } t[] = {
        { KSVM_OP_SHL, 0x80000001u, 1u,  0x00000002u, O_C },
        { KSVM_OP_SHL, 0x00000001u, 31u, 0x80000000u, O_N },
        { KSVM_OP_SHL, 0x00000003u, 31u, 0x80000000u, O_N | O_C },
        { KSVM_OP_SHL, 0x00000001u, 32u, 0x00000001u, 0u },          /* & 31 = 0 */
        { KSVM_OP_SHL, 0x00000001u, 33u, 0x00000002u, 0u },
        { KSVM_OP_SHL, 0x00000001u, 0xFFFFFFFFu, 0x80000000u, O_N },
        { KSVM_OP_SHR, 0x80000000u, 31u, 0x00000001u, 0u },
        { KSVM_OP_SHR, 0xC0000000u, 31u, 0x00000001u, O_C },
        { KSVM_OP_SHR, 0x00000001u, 1u,  0x00000000u, O_Z | O_C },
        { KSVM_OP_SHR, 0x80000000u, 63u, 0x00000001u, 0u },
        { KSVM_OP_SAR, 0x80000000u, 31u, 0xFFFFFFFFu, O_N },
        { KSVM_OP_SAR, 0x80000000u, 32u, 0x80000000u, O_N },
        { KSVM_OP_SAR, 0x7FFFFFFFu, 31u, 0x00000000u, O_Z | O_C },
        { KSVM_OP_SAR, 0xFFFFFFFFu, 1u,  0xFFFFFFFFu, O_N | O_C },
        { KSVM_OP_SAR, 0x80000001u, 33u, 0xC0000000u, O_N | O_C },
        { KSVM_OP_ROL, 0x80000000u, 1u,  0x00000001u, O_C },
        { KSVM_OP_ROL, 0x12345678u, 4u,  0x23456781u, O_C },
        { KSVM_OP_ROL, 0x12345678u, 36u, 0x23456781u, O_C },
        { KSVM_OP_ROL, 0x00000001u, 31u, 0x80000000u, O_N },
        { KSVM_OP_ROR, 0x00000001u, 1u,  0x80000000u, O_N | O_C },
        { KSVM_OP_ROR, 0x12345678u, 4u,  0x81234567u, O_N | O_C },
        { KSVM_OP_ROR, 0x12345678u, 8u,  0x78123456u, 0u },
        { KSVM_OP_ROR, 0x80000000u, 31u, 0x00000001u, 0u },
        { KSVM_OP_ROR, 0x00000000u, 5u,  0x00000000u, O_Z },
    };
    unsigned i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        cf_case c;
        c.what = ksvm_op_name(t[i].op);
        c.w0 = ksvm_enc_r(t[i].op, CF_RD, CF_RA, CF_RB);
        c.w1 = 0u;
        c.a = t[i].a;
        c.b = t[i].n;
        c.fin = 0u;
        c.rd = CF_RD;
        c.v = t[i].v;
        c.fl = t[i].fl;
        cf_run(t_, &c);
        if (TEST_FAILED()) return;
        c.fin = O_V;                       /* V preservado nos ZNC */
        c.fl = t[i].fl | O_V;
        cf_run(t_, &c);
        if (TEST_FAILED()) return;
    }
}

TEST(aliasing_all_r_forms)
{
    unsigned i;
    for (i = 0; i < N_LOGIC; i++) {
        if (logic_ops[i].fmt == CF_FMT_R) {
            cf_check_aliasing(t_, &logic_ops[i]);
            if (TEST_FAILED()) return;
        }
    }
}

SUITE(conform_logic)
{
    RUN_TEST(conform_AND);
    RUN_TEST(conform_ANDI);
    RUN_TEST(conform_ANDL);
    RUN_TEST(conform_OR);
    RUN_TEST(conform_ORI);
    RUN_TEST(conform_ORL);
    RUN_TEST(conform_XOR);
    RUN_TEST(conform_XORI);
    RUN_TEST(conform_XORL);
    RUN_TEST(conform_NOT);
    RUN_TEST(conform_TST);
    RUN_TEST(conform_TSTI);
    RUN_TEST(conform_SHL);
    RUN_TEST(conform_SHLI);
    RUN_TEST(conform_SHR);
    RUN_TEST(conform_SHRI);
    RUN_TEST(conform_SAR);
    RUN_TEST(conform_SARI);
    RUN_TEST(conform_ROL);
    RUN_TEST(conform_ROR);
    RUN_TEST(conform_SXB);
    RUN_TEST(conform_SXH);
    RUN_TEST(conform_ZXB);
    RUN_TEST(conform_ZXH);
#if KSVM_ENABLE_BITOPS
    RUN_TEST(conform_CLZ);
    RUN_TEST(conform_CTZ);
    RUN_TEST(conform_POPCNT);
    RUN_TEST(bitops_every_bit_position);
#endif
    RUN_TEST(shifts_every_count);
    RUN_TEST(shift_literal_examples);
    RUN_TEST(aliasing_all_r_forms);
}
