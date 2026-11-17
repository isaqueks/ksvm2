/*
 * test_alu.c — cada operação da ALU x valores extremos, conferida contra um
 * oráculo independente (doc 09 §3): somas/subtrações em uint64_t, sinal via
 * int64_t, deslocamentos e contagens por laços bit a bit. Resultado E cada
 * flag são verificados, com flags de entrada 0 e 0xF (para provar que as
 * flags não afetadas são preservadas e que ADC/SBC usam o C de entrada).
 */
#include "ksvm/alu.h"
#include "ksvm/isa.h"
#include "test.h"

static const uint32_t vals[] = {
    0x00000000u, 0x00000001u, 0x00000002u, 0x00000003u, 0x0000007Fu,
    0x00000080u, 0x000000FFu, 0x00007FFFu, 0x00008000u, 0x7FFFFFFEu,
    0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu,
    0x12345678u, 0xDEADBEEFu, 0x55555555u, 0xAAAAAAAAu
};
#define NVALS (sizeof vals / sizeof vals[0])

static const uint32_t counts[] = { 0u, 1u, 2u, 7u, 15u, 16u, 30u, 31u, 32u, 33u, 63u, 0xFFFFFFFFu };
#define NCOUNTS (sizeof counts / sizeof counts[0])

static const uint32_t in_flags[] = { 0u, 0xFu };

/* ---- Oráculo ---------------------------------------------------------- */

static int64_t s64(uint32_t x)
{
    return (x & 0x80000000u) ? (int64_t)x - INT64_C(4294967296) : (int64_t)x;
}

static uint32_t mk_flags(uint32_t res, int c, int v)
{
    uint32_t f = 0;
    if (res == 0u) f |= KSVM_FLAG_Z;
    if (res & 0x80000000u) f |= KSVM_FLAG_N;
    if (c) f |= KSVM_FLAG_C;
    if (v) f |= KSVM_FLAG_V;
    return f;
}

/* Flags esperadas quando só Z e N mudam. */
static uint32_t zn_keep(uint32_t res, uint32_t f0)
{
    return mk_flags(res, 0, 0) | (f0 & (KSVM_FLAG_C | KSVM_FLAG_V));
}

/* Flags esperadas quando Z, N e C mudam. */
static uint32_t znc_keep(uint32_t res, int c, uint32_t f0)
{
    return mk_flags(res, c, 0) | (f0 & KSVM_FLAG_V);
}

#define CHECK(got, want, a, b)                                              \
    do {                                                                    \
        if ((uint32_t)(got) != (uint32_t)(want)) {                          \
            test_note(t_, "a=0x%08lX b=0x%08lX", (unsigned long)(a), (unsigned long)(b)); \
            ASSERT_EQ_U32(got, want);                                       \
        }                                                                   \
    } while (0)

/* ---- Soma e subtração ------------------------------------------------- */

TEST(add_matches_oracle)
{
    size_t i, j, k;
    for (k = 0; k < 2; k++)
    for (i = 0; i < NVALS; i++)
    for (j = 0; j < NVALS; j++) {
        uint32_t a = vals[i], b = vals[j], f = in_flags[k];
        uint64_t s = (uint64_t)a + b;
        uint32_t res = (uint32_t)s;
        int v = (s64(a) + s64(b)) != s64(res);
        CHECK(ksvm_alu_add(a, b, &f), res, a, b);
        CHECK(f, mk_flags(res, (int)(s >> 32), v), a, b);
    }
}

TEST(sub_matches_oracle)
{
    size_t i, j, k;
    for (k = 0; k < 2; k++)
    for (i = 0; i < NVALS; i++)
    for (j = 0; j < NVALS; j++) {
        uint32_t a = vals[i], b = vals[j], f = in_flags[k];
        uint32_t res = (uint32_t)((uint64_t)a - b);
        int c = (uint64_t)a >= (uint64_t)b;
        int v = (s64(a) - s64(b)) != s64(res);
        CHECK(ksvm_alu_sub(a, b, &f), res, a, b);
        CHECK(f, mk_flags(res, c, v), a, b);
    }
}

TEST(adc_matches_oracle)
{
    size_t i, j, k;
    for (k = 0; k < 2; k++)
    for (i = 0; i < NVALS; i++)
    for (j = 0; j < NVALS; j++) {
        uint32_t a = vals[i], b = vals[j], f = in_flags[k];
        uint32_t cin = (f & KSVM_FLAG_C) ? 1u : 0u;
        uint64_t s = (uint64_t)a + b + cin;
        uint32_t res = (uint32_t)s;
        int v = (s64(a) + s64(b) + cin) != s64(res);
        CHECK(ksvm_alu_adc(a, b, &f), res, a, b);
        CHECK(f, mk_flags(res, (int)(s >> 32), v), a, b);
    }
}

TEST(sbc_matches_oracle)
{
    size_t i, j, k;
    for (k = 0; k < 2; k++)
    for (i = 0; i < NVALS; i++)
    for (j = 0; j < NVALS; j++) {
        uint32_t a = vals[i], b = vals[j], f = in_flags[k];
        int64_t borrow = (f & KSVM_FLAG_C) ? 0 : 1;
        int64_t diff = (int64_t)a - (int64_t)b - borrow;   /* operandos sem sinal */
        uint32_t res = (uint32_t)(uint64_t)diff;
        int v = (s64(a) - s64(b) - borrow) != s64(res);
        CHECK(ksvm_alu_sbc(a, b, &f), res, a, b);
        CHECK(f, mk_flags(res, diff >= 0, v), a, b);
    }
}

TEST(add_sub_flag_spot_checks)
{
    uint32_t f = 0;
    ksvm_alu_add(0xFFFFFFFFu, 1u, &f);
    ASSERT_EQ_U32(f, KSVM_FLAG_Z | KSVM_FLAG_C);
    ksvm_alu_add(0x7FFFFFFFu, 1u, &f);
    ASSERT_EQ_U32(f, KSVM_FLAG_N | KSVM_FLAG_V);
    ksvm_alu_sub(5u, 5u, &f);                      /* a >= b: C = 1 (ARM) */
    ASSERT_EQ_U32(f, KSVM_FLAG_Z | KSVM_FLAG_C);
    ksvm_alu_sub(0u, 1u, &f);                      /* borrow: C = 0 */
    ASSERT_EQ_U32(f, KSVM_FLAG_N);
    ksvm_alu_sub(0x80000000u, 1u, &f);
    ASSERT_EQ_U32(f, KSVM_FLAG_C | KSVM_FLAG_V);
}

/* ---- Lógica (ZN, preserva C e V) -------------------------------------- */

TEST(zn_preserves_c_and_v)
{
    size_t i, k;
    for (k = 0; k < 2; k++)
    for (i = 0; i < NVALS; i++) {
        uint32_t f = in_flags[k];
        CHECK(ksvm_alu_zn(vals[i], &f), vals[i], vals[i], 0u);
        CHECK(f, zn_keep(vals[i], in_flags[k]), vals[i], 0u);
    }
}

/* ---- Deslocamentos e rotações ----------------------------------------- */

typedef uint32_t (*shift_fn)(uint32_t, uint32_t, uint32_t *);

/* Oráculo: desloca 1 bit por vez, guardando o último bit que saiu. */
static uint32_t ref_shift(int kind, uint32_t a, uint32_t n, int *c)
{
    uint32_t i;
    n &= 31u;
    *c = 0;
    for (i = 0; i < n; i++) {
        switch (kind) {
        case 0: *c = (int)(a >> 31); a <<= 1; break;                                  /* SHL */
        case 1: *c = (int)(a & 1u); a >>= 1; break;                                   /* SHR */
        case 2: *c = (int)(a & 1u); a = (a >> 1) | (a & 0x80000000u); break;         /* SAR */
        case 3: *c = (int)(a >> 31); a = (a << 1) | (a >> 31); break;                 /* ROL */
        default: *c = (int)(a & 1u); a = (a >> 1) | (a << 31); break;                 /* ROR */
        }
    }
    return a;
}

static void check_shift(test_ctx *t_, int kind, shift_fn fn)
{
    size_t i, j, k;
    for (k = 0; k < 2; k++)
    for (i = 0; i < NVALS; i++)
    for (j = 0; j < NCOUNTS; j++) {
        uint32_t a = vals[i], n = counts[j], f = in_flags[k];
        int c;
        uint32_t res = ref_shift(kind, a, n, &c);
        CHECK(fn(a, n, &f), res, a, n);
        CHECK(f, znc_keep(res, c, in_flags[k]), a, n);
    }
}

TEST(shl_matches_oracle) { check_shift(t_, 0, ksvm_alu_shl); }
TEST(shr_matches_oracle) { check_shift(t_, 1, ksvm_alu_shr); }
TEST(sar_matches_oracle) { check_shift(t_, 2, ksvm_alu_sar); }
TEST(rol_matches_oracle) { check_shift(t_, 3, ksvm_alu_rol); }
TEST(ror_matches_oracle) { check_shift(t_, 4, ksvm_alu_ror); }

TEST(shift_count_zero_clears_carry)
{
    uint32_t f = KSVM_FLAG_C | KSVM_FLAG_V;
    ASSERT_EQ_U32(ksvm_alu_shl(0x80000000u, 32u, &f), 0x80000000u);  /* 32 & 31 = 0 */
    ASSERT_EQ_U32(f, KSVM_FLAG_N | KSVM_FLAG_V);
}

TEST(sar_replicates_sign)
{
    uint32_t f = 0;
    ASSERT_EQ_U32(ksvm_alu_sar(0x80000000u, 31u, &f), 0xFFFFFFFFu);
    ASSERT_EQ_U32(ksvm_alu_sar(0xFFFFFFF0u, 4u, &f), 0xFFFFFFFFu);
    ASSERT_EQ_U32(f, KSVM_FLAG_N);                  /* último bit que saiu: 0 */
    ASSERT_EQ_U32(ksvm_alu_sar(0x40000000u, 30u, &f), 1u);
}

/* ---- Multiplicação e divisão ------------------------------------------ */

TEST(mul_family_matches_oracle)
{
    size_t i, j;
    for (i = 0; i < NVALS; i++)
    for (j = 0; j < NVALS; j++) {
        uint32_t a = vals[i], b = vals[j];
        uint64_t pu = (uint64_t)a * b;
        uint64_t ps = (uint64_t)(s64(a) * s64(b));   /* cabe em int64_t */
        CHECK(ksvm_alu_mul(a, b), (uint32_t)pu, a, b);
        CHECK(ksvm_alu_mulhu(a, b), (uint32_t)(pu >> 32), a, b);
        CHECK(ksvm_alu_mulh(a, b), (uint32_t)(ps >> 32), a, b);
    }
}

TEST(div_family_matches_oracle)
{
    size_t i, j;
    for (i = 0; i < NVALS; i++)
    for (j = 0; j < NVALS; j++) {
        uint32_t a = vals[i], b = vals[j], q = 0xA5A5A5A5u;
        if (b == 0u) {
            ASSERT_FALSE(ksvm_alu_div(a, b, &q));
            ASSERT_FALSE(ksvm_alu_divu(a, b, &q));
            ASSERT_FALSE(ksvm_alu_rem(a, b, &q));
            ASSERT_FALSE(ksvm_alu_remu(a, b, &q));
            ASSERT_EQ_U32(q, 0xA5A5A5A5u);           /* saída intocada */
            continue;
        }
        ASSERT_TRUE(ksvm_alu_div(a, b, &q));
        CHECK(q, (uint32_t)(uint64_t)(s64(a) / s64(b)), a, b);   /* C99: trunca */
        ASSERT_TRUE(ksvm_alu_rem(a, b, &q));
        CHECK(q, (uint32_t)(uint64_t)(s64(a) % s64(b)), a, b);   /* sinal do dividendo */
        ASSERT_TRUE(ksvm_alu_divu(a, b, &q));
        CHECK(q, a / b, a, b);
        ASSERT_TRUE(ksvm_alu_remu(a, b, &q));
        CHECK(q, a % b, a, b);
    }
}

TEST(div_int_min_by_minus_one)
{
    uint32_t q = 0;
    ASSERT_TRUE(ksvm_alu_div(0x80000000u, 0xFFFFFFFFu, &q));
    ASSERT_EQ_U32(q, 0x80000000u);
    ASSERT_TRUE(ksvm_alu_rem(0x80000000u, 0xFFFFFFFFu, &q));
    ASSERT_EQ_U32(q, 0u);
    ASSERT_TRUE(ksvm_alu_div((uint32_t)-7, 2u, &q));
    ASSERT_EQ_U32(q, (uint32_t)-3);                  /* trunca para zero */
    ASSERT_TRUE(ksvm_alu_rem((uint32_t)-7, 2u, &q));
    ASSERT_EQ_U32(q, (uint32_t)-1);
}

/* ---- Contagem de bits ------------------------------------------------- */

TEST(bit_counts_match_oracle)
{
    size_t i;
    for (i = 0; i < NVALS + 32u; i++) {
        uint32_t a = (i < NVALS) ? vals[i] : (1u << (i - NVALS));
        uint32_t clz = 0, ctz = 0, pop = 0, bit;
        int seen = 0;
        for (bit = 32; bit-- > 0;) {                /* do bit 31 ao 0 */
            if (a & (1u << bit)) { seen = 1; pop++; } else if (!seen) clz++;
        }
        for (bit = 0; bit < 32u && !(a & (1u << bit)); bit++) ctz++;
        CHECK(ksvm_alu_clz(a), clz, a, 0u);
        CHECK(ksvm_alu_ctz(a), ctz, a, 0u);
        CHECK(ksvm_alu_popcnt(a), pop, a, 0u);
    }
}

SUITE(alu)
{
    RUN_TEST(add_matches_oracle);
    RUN_TEST(sub_matches_oracle);
    RUN_TEST(adc_matches_oracle);
    RUN_TEST(sbc_matches_oracle);
    RUN_TEST(add_sub_flag_spot_checks);
    RUN_TEST(zn_preserves_c_and_v);
    RUN_TEST(shl_matches_oracle);
    RUN_TEST(shr_matches_oracle);
    RUN_TEST(sar_matches_oracle);
    RUN_TEST(rol_matches_oracle);
    RUN_TEST(ror_matches_oracle);
    RUN_TEST(shift_count_zero_clears_carry);
    RUN_TEST(sar_replicates_sign);
    RUN_TEST(mul_family_matches_oracle);
    RUN_TEST(div_family_matches_oracle);
    RUN_TEST(div_int_min_by_minus_one);
    RUN_TEST(bit_counts_match_oracle);
}
