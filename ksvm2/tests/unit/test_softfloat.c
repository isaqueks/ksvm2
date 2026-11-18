/*
 * test_softfloat.c — soft-float binary32 (softfloat.c) contra o float do
 * host, comparando bit a bit (doc 09; doc 04 §5.7).
 *
 * Oráculo: as operações de float do próprio host (+ - * /, comparações,
 * conversões int <-> float do C), feitas através de volatile para que o
 * compilador não as dobre nem as mantenha com precisão extra. Em x86-64 é
 * SSE (IEEE estrito); no PowerPC do test-bigendian é a FPU do qemu-ppc;
 * -std=c99 desliga a contração em FMA. Qualquer NaN do host vale como
 * "esperado NaN" e então o resultado tem de ser exatamente o NaN canônico
 * 0x7FC00000. Sem libm (o link dos testes não usa -lm): floor/ceil/trunc/
 * rint saem de conversões int <-> float do C, e a raiz é conferida por
 * enquadramento exato em double (ver sqrt_is_correct).
 *
 * Casos: (1) valores escritos à mão (os da especificação); (2) todos os
 * pares de uma tabela de bordas; (3) um gerador xorshift32 de semente fixa
 * com viés para expoentes extremos e padrões de mantissa, SF_PAIRS pares
 * por operação (200000; menos com ASan, ou -DKSVM_TEST_FLOAT_PAIRS=n);
 * (4) varredura em passo fixo de todo o espaço de 32 bits para as
 * operações de um operando.
 */
#include <string.h>

#include "ksvm/isa.h"
#include "ksvm/softfloat.h"
#include "test.h"

#if KSVM_ENABLE_FLOAT

#ifndef KSVM_TEST_FLOAT_PAIRS
#if defined(__SANITIZE_ADDRESS__)
#define KSVM_TEST_FLOAT_PAIRS 20000u
#else
#define KSVM_TEST_FLOAT_PAIRS 200000u
#endif
#endif
#define SF_PAIRS KSVM_TEST_FLOAT_PAIRS
#define SF_SWEEP_STEP 0x10001u        /* ~65536 valores na varredura */

#define NAN_C  KSVM_F32_NAN
#define SIGN   KSVM_F32_SIGN
#define F_Z    KSVM_FLAG_Z
#define F_N    KSVM_FLAG_N
#define F_C    KSVM_FLAG_C
#define F_V    KSVM_FLAG_V

/* ---- Host ------------------------------------------------------------- */

static float float_of(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static uint32_t bits_of(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static int bits_nan(uint32_t a)
{
    return (a & 0x7FFFFFFFu) > 0x7F800000u;
}

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MIN, OP_MAX, OP_CMP };

static uint32_t host_arith(int op, uint32_t a, uint32_t b)
{
    volatile float x = float_of(a), y = float_of(b), r;

    switch (op) {
    case OP_ADD: r = x + y; break;
    case OP_SUB: r = x - y; break;
    case OP_MUL: r = x * y; break;
    default:     r = x / y; break;
    }
    return bits_of(r);
}

static uint32_t h_add(uint32_t a, uint32_t b) { return host_arith(OP_ADD, a, b); }
static uint32_t h_sub(uint32_t a, uint32_t b) { return host_arith(OP_SUB, a, b); }
static uint32_t h_mul(uint32_t a, uint32_t b) { return host_arith(OP_MUL, a, b); }
static uint32_t h_div(uint32_t a, uint32_t b) { return host_arith(OP_DIV, a, b); }

/* min/max pela comparação do host; iguais só podem ser bits iguais ou
 * +0/-0, e aí min prefere o negativo e max o positivo. */
static uint32_t host_minmax(uint32_t a, uint32_t b, int want_max)
{
    volatile float x = float_of(a), y = float_of(b);

    if (x != x && y != y) return NAN_C;
    if (x != x) return b;
    if (y != y) return a;
    if (x < y) return want_max ? b : a;
    if (y < x) return want_max ? a : b;
    if (want_max) return (a & SIGN) ? b : a;
    return (a & SIGN) ? a : b;
}

static uint32_t h_min(uint32_t a, uint32_t b) { return host_minmax(a, b, 0); }
static uint32_t h_max(uint32_t a, uint32_t b) { return host_minmax(a, b, 1); }

static uint32_t h_cmp(uint32_t a, uint32_t b)
{
    volatile float x = float_of(a), y = float_of(b);

    if (x < y) return F_N;
    if (x == y) return F_Z | F_C;
    if (x > y) return F_C;
    return F_C | F_V;
}

/* Inteiro mais próximo sem libm: |x| < 2^23 cabe num int32_t e a fração
 * x - trunc(x) é exata. Zero preserva o sinal de x. */
static uint32_t host_round(uint32_t a, ksvm_f32_rounding mode)
{
    volatile float x = float_of(a), t, frac, r;
    int32_t it;

    if (x != x) return NAN_C;
    if (!(x < 8388608.0f && x > -8388608.0f)) return a;     /* inteiro ou inf */
    it = (int32_t)x;
    t = (float)it;
    frac = x - t;
    switch (mode) {
    case KSVM_F32_FLOOR: r = (frac < 0.0f) ? t - 1.0f : t; break;
    case KSVM_F32_CEIL:  r = (frac > 0.0f) ? t + 1.0f : t; break;
    case KSVM_F32_TRUNC: r = t; break;
    default:
        if (frac > 0.5f || (frac == 0.5f && it % 2 != 0)) {
            r = t + 1.0f;
        } else if (frac < -0.5f || (frac == -0.5f && it % 2 != 0)) {
            r = t - 1.0f;
        } else {
            r = t;
        }
        break;
    }
    if (r == 0.0f) return a & SIGN;
    return bits_of(r);
}

static uint32_t h_rint(uint32_t a)  { return host_round(a, KSVM_F32_RNE); }
static uint32_t h_floor(uint32_t a) { return host_round(a, KSVM_F32_FLOOR); }
static uint32_t h_ceil(uint32_t a)  { return host_round(a, KSVM_F32_CEIL); }
static uint32_t h_trunc(uint32_t a) { return host_round(a, KSVM_F32_TRUNC); }

static uint32_t h_ftoi(uint32_t a)
{
    volatile float x = float_of(a);

    if (x != x) return 0u;
    if (x >= 2147483648.0f) return 0x7FFFFFFFu;
    if (x <= -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)x;
}

static uint32_t h_ftou(uint32_t a)
{
    volatile float x = float_of(a);

    if (x != x || x <= -1.0f) return 0u;
    if (x >= 4294967296.0f) return 0xFFFFFFFFu;
    return (uint32_t)x;                     /* (-1, 0) trunca para 0 */
}

static uint32_t h_itof(uint32_t a)
{
    int32_t s = (a & SIGN) ? -(int32_t)(~a) - 1 : (int32_t)a;
    volatile float r = (float)s;
    return bits_of(r);
}

static uint32_t h_utof(uint32_t a)
{
    volatile float r = (float)a;
    return bits_of(r);
}

/*
 * Raiz: got está correta se o radicando cai estritamente entre os
 * quadrados dos pontos médios de got com os vizinhos. Pontos médios de
 * floats têm 25 bits significativos: o quadrado (50 bits) é exato em
 * double, então a conferência não arredonda nada.
 */
static int sqrt_is_correct(uint32_t a, uint32_t got)
{
    volatile double x, lo, hi;

    if (bits_nan(a) || ((a & SIGN) && (a & 0x7FFFFFFFu) != 0u)) return got == NAN_C;
    if ((a & 0x7FFFFFFFu) == 0u || a == 0x7F800000u) return got == a;
    if ((got & SIGN) || got == 0u || got >= 0x7F800000u) return 0;
    x = (double)float_of(a);
    lo = ((double)float_of(got - 1u) + (double)float_of(got)) / 2.0;
    hi = ((double)float_of(got + 1u) + (double)float_of(got)) / 2.0;
    return lo * lo < x && x < hi * hi;
}

/* ---- Conferência ------------------------------------------------------ */

/* NaN esperado = exatamente o canônico; senão bit a bit. */
static int same(uint32_t got, uint32_t want)
{
    return bits_nan(want) ? got == NAN_C : got == want;
}

typedef uint32_t (*bin_fn)(uint32_t, uint32_t);
typedef uint32_t (*un_fn)(uint32_t);

typedef struct bin_op {
    const char *name;
    bin_fn      soft, host;
} bin_op;

typedef struct un_op {
    const char *name;
    un_fn       soft, host;   /* host NULL: sqrt (conferida por sqrt_is_correct) */
    int         int_result;   /* resultado inteiro: bit a bit, sem NaN */
} un_op;

static uint32_t s_rint(uint32_t a)  { return ksvm_f32_round_int(a, KSVM_F32_RNE); }
static uint32_t s_floor(uint32_t a) { return ksvm_f32_round_int(a, KSVM_F32_FLOOR); }
static uint32_t s_ceil(uint32_t a)  { return ksvm_f32_round_int(a, KSVM_F32_CEIL); }
static uint32_t s_trunc(uint32_t a) { return ksvm_f32_round_int(a, KSVM_F32_TRUNC); }

static const bin_op bin_ops[] = {
    { "fadd", ksvm_f32_add, h_add }, { "fsub", ksvm_f32_sub, h_sub },
    { "fmul", ksvm_f32_mul, h_mul }, { "fdiv", ksvm_f32_div, h_div },
    { "fmin", ksvm_f32_min, h_min }, { "fmax", ksvm_f32_max, h_max },
    { "fcmp", ksvm_f32_cmp, h_cmp },
};

/* Operações float -> algo (o argumento é um float). */
static const un_op un_ops[] = {
    { "fsqrt", ksvm_f32_sqrt, NULL, 0 },
    { "frint", s_rint, h_rint, 0 },   { "ffloor", s_floor, h_floor, 0 },
    { "fceil", s_ceil, h_ceil, 0 },   { "ftrunc", s_trunc, h_trunc, 0 },
    { "ftoi", ksvm_f32_to_i32, h_ftoi, 1 }, { "ftou", ksvm_f32_to_u32, h_ftou, 1 },
};

/* Operações inteiro -> float (o argumento é um inteiro). */
static const un_op int_ops[] = {
    { "itof", ksvm_i32_to_f32, h_itof, 0 }, { "utof", ksvm_u32_to_f32, h_utof, 0 },
};

#define COUNT(a) (sizeof (a) / sizeof (a)[0])

static int check_bin(test_ctx *t_, const bin_op *op, uint32_t a, uint32_t b)
{
    uint32_t got = op->soft(a, b), want = op->host(a, b);

    if (!same(got, want)) {
        test_note(t_, "%s(0x%08lX, 0x%08lX) = 0x%08lX, host 0x%08lX", op->name,
                  (unsigned long)a, (unsigned long)b, (unsigned long)got, (unsigned long)want);
        test_fail(t_, __FILE__, __LINE__, "resultado diferente do host");
        return 0;
    }
    return 1;
}

static int check_un(test_ctx *t_, const un_op *op, uint32_t a)
{
    uint32_t got = op->soft(a);
    int ok;

    if (op->host == NULL) {
        ok = sqrt_is_correct(a, got);
    } else {
        ok = op->int_result ? got == op->host(a) : same(got, op->host(a));
    }

    if (!ok) {
        test_note(t_, "%s(0x%08lX) = 0x%08lX, host 0x%08lX", op->name, (unsigned long)a,
                  (unsigned long)got, (unsigned long)(op->host != NULL ? op->host(a) : 0u));
        test_fail(t_, __FILE__, __LINE__, "resultado diferente do host");
        return 0;
    }
    return 1;
}

/* ---- Valores de borda ------------------------------------------------- */

static const uint32_t edges[] = {
    0x00000000u, 0x80000000u,             /* +0, -0 */
    0x7F800000u, 0xFF800000u,             /* +inf, -inf */
    0x7FC00000u, 0xFFC00000u,             /* NaN quieto (canônico e negativo) */
    0x7F800001u, 0xFFBFFFFFu, 0x7FFFFFFFu, 0x7FA00000u,   /* NaNs com payload */
    0x00000001u, 0x80000001u,             /* menor subnormal */
    0x00000002u, 0x00000003u, 0x00400000u, 0x00400001u,
    0x007FFFFFu, 0x807FFFFFu,             /* maior subnormal */
    0x00800000u, 0x80800000u,             /* FLT_MIN */
    0x00800001u, 0x00FFFFFFu, 0x01000000u,
    0x7F7FFFFFu, 0xFF7FFFFFu,             /* FLT_MAX */
    0x7F7FFFFEu, 0x7F000000u, 0x7E800000u,
    0x3F800000u, 0xBF800000u,             /* 1, -1 */
    0x3F800001u, 0x3F7FFFFFu, 0xBF7FFFFFu,
    0x3F000000u, 0xBF000000u, 0x3F000001u, 0x3EFFFFFFu,  /* 1/2 e vizinhos */
    0x3FC00000u, 0xBFC00000u,             /* 1.5 */
    0x40200000u, 0xC0200000u,             /* 2.5 */
    0x40600000u, 0x40000000u, 0x40400000u, 0x40A00000u,  /* 3.5, 2, 3, 5 */
    0x3EAAAAABu, 0x3DCCCCCDu,             /* 1/3, 0.1 */
    0x3FB504F3u, 0x3FB504F4u,             /* sqrt(2) */
    0x34000000u, 0x33800000u, 0x33FFFFFFu,   /* 2^-23, 2^-24 */
    0x4AFFFFFFu, 0x4B000000u, 0x4B000001u, 0x4B7FFFFFu, 0x4B800000u, 0x4B800001u,
    0x4EFFFFFFu, 0x4F000000u, 0xCF000000u, 0xCF000001u, 0xCEFFFFFFu,
    0x4F7FFFFFu, 0x4F800000u, 0xBF800001u,
    0x1F800000u, 0x5F800000u, 0x0C000000u, 0x72000000u,  /* 2^-64, 2^64, ... */
    0x00000005u, 0x00012345u, 0x3456789Au, 0xC0490FDBu,  /* pi negativo */
};

/* ---- Gerador ---------------------------------------------------------- */

static uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Float com viés: expoentes nos extremos e perto de 1/2^23/2^31,
 * mantissas zero, cheias, de um bit, só bits baixos ou altos. */
static uint32_t gen_float(uint32_t *s)
{
    uint32_t r = rnd(s), e, m;

    switch ((r >> 1) % 10u) {
    case 0:  e = 0u; break;                               /* zero / subnormal */
    case 1:  e = 0xFFu; break;                            /* inf / NaN */
    case 2:  e = 1u + (r >> 8) % 4u; break;               /* fronteira subnormal */
    case 3:  e = 0xFBu + (r >> 8) % 4u; break;            /* perto de overflow */
    case 4:  e = 124u + (r >> 8) % 7u; break;             /* perto de 1 */
    case 5:  e = 148u + (r >> 8) % 12u; break;            /* 2^21..2^32 */
    default: e = (r >> 8) & 0xFFu; break;
    }
    r = rnd(s);
    switch (r % 8u) {
    case 0:  m = 0u; break;
    case 1:  m = 0x7FFFFFu; break;
    case 2:  m = 1u << ((r >> 3) % 23u); break;
    case 3:  m = (r >> 3) & 0xFu; break;
    case 4:  m = 0x400000u | ((r >> 3) & 0x7u); break;
    case 5:  m = 0x7FFFFFu - ((r >> 3) & 0xFu); break;
    default: m = (r >> 3) & 0x7FFFFFu; break;
    }
    return (rnd(s) & SIGN) | (e << 23) | m;
}

/* b relacionado a a: vizinho em bits, mesmo valor com sinal trocado,
 * expoente próximo (cancelamento) ou na fronteira do alinhamento (24-26). */
static uint32_t gen_partner(uint32_t *s, uint32_t a)
{
    uint32_t r = rnd(s), e = (a >> 23) & 0xFFu, d = (r >> 4) % 4u;

    switch (r % 6u) {
    case 0:  return a + ((r >> 8) & 0x7u) - 3u;
    case 1:  return (a ^ SIGN) + ((r >> 8) & 0x3u);
    case 2:  return (a & SIGN) ^ (((r >> 9) & 1u) ? SIGN : 0u) ^
                    ((((e + d) & 0xFFu) << 23) | (rnd(s) & 0x7FFFFFu));
    case 3:  return (rnd(s) & SIGN) | ((((e - 24u - d) & 0xFFu) << 23) | (rnd(s) & 0x7FFFFFu));
    default: return gen_float(s);
    }
}

/* Inteiro com viés: pequenos, perto de 2^24 (onde o float perde a
 * unidade), dos extremos de int32/uint32, um bit, aleatório. */
static uint32_t gen_int(uint32_t *s)
{
    uint32_t r = rnd(s), k = (r >> 4) % 32u;

    switch (r % 7u) {
    case 0:  return (r >> 8) % 64u - 32u;
    case 1:  return (UINT32_C(1) << (24u + (r >> 9) % 8u)) + ((r >> 16) % 16u) - 8u;
    case 2:  return 0x80000000u + ((r >> 8) % 256u) - 128u;
    case 3:  return 0xFFFFFFFFu - ((r >> 8) % 256u);
    case 4:  return UINT32_C(1) << k;
    case 5:  return rnd(s) >> k;
    default: return rnd(s);
    }
}

/* ---- Casos escritos à mão (especificação) ---------------------------- */

TEST(spec_examples)
{
    /* 1/3, sqrt(2), sqrt de negativo, divisão por zero */
    ASSERT_EQ_U32(ksvm_f32_div(0x3F800000u, 0x40400000u), 0x3EAAAAABu);
    ASSERT_EQ_U32(ksvm_f32_sqrt(0x40000000u), 0x3FB504F3u);
    ASSERT_EQ_U32(ksvm_f32_sqrt(0xBF800000u), NAN_C);
    ASSERT_EQ_U32(ksvm_f32_sqrt(0x80000000u), 0x80000000u);
    ASSERT_EQ_U32(ksvm_f32_sqrt(0xFF800000u), NAN_C);
    ASSERT_EQ_U32(ksvm_f32_div(0x3F800000u, 0x00000000u), 0x7F800000u);
    ASSERT_EQ_U32(ksvm_f32_div(0x3F800000u, 0x80000000u), 0xFF800000u);
    ASSERT_EQ_U32(ksvm_f32_div(0x00000000u, 0x00000000u), NAN_C);
    ASSERT_EQ_U32(ksvm_f32_mul(0x7F800000u, 0x80000000u), NAN_C);
    ASSERT_EQ_U32(ksvm_f32_add(0x7F800000u, 0xFF800000u), NAN_C);
    /* overflow -> inf, underflow -> subnormal (e empate para par em 0) */
    ASSERT_EQ_U32(ksvm_f32_add(0x7F7FFFFFu, 0x7F7FFFFFu), 0x7F800000u);
    ASSERT_EQ_U32(ksvm_f32_mul(0xFF7FFFFFu, 0x40000000u), 0xFF800000u);
    /* FLT_MAX + ulp/2 empata entre FLT_MAX (ímpar) e 2^128: vai para inf */
    ASSERT_EQ_U32(ksvm_f32_add(0x7F7FFFFFu, 0x73000000u), 0x7F800000u);
    ASSERT_EQ_U32(ksvm_f32_add(0x7F7FFFFFu, 0x72FFFFFFu), 0x7F7FFFFFu);
    ASSERT_EQ_U32(ksvm_f32_div(0x00800000u, 0x40000000u), 0x00400000u);
    ASSERT_EQ_U32(ksvm_f32_mul(0x00000001u, 0x3F000000u), 0x00000000u);   /* 0.5 ulp: par */
    ASSERT_EQ_U32(ksvm_f32_mul(0x00000001u, 0x3FC00000u), 0x00000002u);   /* 1.5 ulp: par */
    ASSERT_EQ_U32(ksvm_f32_mul(0x00000003u, 0x3F000000u), 0x00000002u);   /* 1.5 ulp */
    ASSERT_EQ_U32(ksvm_f32_mul(0x007FFFFFu, 0x3F800001u), 0x00800000u);   /* sobe a normal */
    /* meio exato: 2^24 + 1 não cabe; empate vai para o par */
    ASSERT_EQ_U32(ksvm_f32_add(0x4B800000u, 0x3F800000u), 0x4B800000u);
    ASSERT_EQ_U32(ksvm_f32_add(0x4B800001u, 0x3F800000u), 0x4B800002u);
    /* cancelamento catastrófico: exato */
    ASSERT_EQ_U32(ksvm_f32_sub(0x3F800001u, 0x3F800000u), 0x34000000u);
    ASSERT_EQ_U32(ksvm_f32_sub(0x3F800000u, 0x3F800000u), 0x00000000u);   /* +0 */
    ASSERT_EQ_U32(ksvm_f32_add(0x80000000u, 0x80000000u), 0x80000000u);   /* -0 + -0 */
    ASSERT_EQ_U32(ksvm_f32_add(0x80000000u, 0x00000000u), 0x00000000u);
    ASSERT_EQ_U32(ksvm_f32_sub(0x00800000u, 0x007FFFFFu), 0x00000001u);
    /* NaN de entrada qualquer -> canônico */
    ASSERT_EQ_U32(ksvm_f32_add(0xFFC12345u, 0x3F800000u), NAN_C);
    ASSERT_EQ_U32(ksvm_f32_mul(0x3F800000u, 0x7F800001u), NAN_C);
    ASSERT_EQ_U32(ksvm_f32_round_int(0xFFFFFFFFu, KSVM_F32_FLOOR), NAN_C);
    /* neg/abs só mexem no sinal, inclusive em NaN */
    ASSERT_EQ_U32(ksvm_f32_neg(0x7F800001u), 0xFF800001u);
    ASSERT_EQ_U32(ksvm_f32_abs(0xFFC12345u), 0x7FC12345u);
    ASSERT_EQ_U32(ksvm_f32_neg(0x00000000u), 0x80000000u);
}

TEST(spec_min_max)
{
    ASSERT_EQ_U32(ksvm_f32_min(0x80000000u, 0x00000000u), 0x80000000u);
    ASSERT_EQ_U32(ksvm_f32_min(0x00000000u, 0x80000000u), 0x80000000u);
    ASSERT_EQ_U32(ksvm_f32_max(0x80000000u, 0x00000000u), 0x00000000u);
    ASSERT_EQ_U32(ksvm_f32_max(0x00000000u, 0x80000000u), 0x00000000u);
    ASSERT_EQ_U32(ksvm_f32_min(0x7FC00001u, 0xBF800000u), 0xBF800000u);   /* NaN: o outro */
    ASSERT_EQ_U32(ksvm_f32_max(0x3F800000u, 0xFF800001u), 0x3F800000u);
    ASSERT_EQ_U32(ksvm_f32_min(0x7F800001u, 0xFFC00000u), NAN_C);         /* dois NaN */
    ASSERT_EQ_U32(ksvm_f32_min(0xFF800000u, 0x7F800000u), 0xFF800000u);
    ASSERT_EQ_U32(ksvm_f32_max(0xBF800000u, 0xC0000000u), 0xBF800000u);
}

TEST(spec_round_and_convert)
{
    ASSERT_EQ_U32(ksvm_f32_round_int(0x40200000u, KSVM_F32_RNE), 0x40000000u);   /* 2.5 -> 2 */
    ASSERT_EQ_U32(ksvm_f32_round_int(0x40600000u, KSVM_F32_RNE), 0x40800000u);   /* 3.5 -> 4 */
    ASSERT_EQ_U32(ksvm_f32_round_int(0x3FC00000u, KSVM_F32_RNE), 0x40000000u);   /* 1.5 -> 2 */
    ASSERT_EQ_U32(ksvm_f32_round_int(0xBF000000u, KSVM_F32_RNE), 0x80000000u);   /* -0.5 -> -0 */
    ASSERT_EQ_U32(ksvm_f32_round_int(0x3F000001u, KSVM_F32_RNE), 0x3F800000u);
    ASSERT_EQ_U32(ksvm_f32_round_int(0xBFC00000u, KSVM_F32_FLOOR), 0xC0000000u); /* -1.5 -> -2 */
    ASSERT_EQ_U32(ksvm_f32_round_int(0xBF000000u, KSVM_F32_CEIL), 0x80000000u);  /* -0.5 -> -0 */
    ASSERT_EQ_U32(ksvm_f32_round_int(0x3F000000u, KSVM_F32_CEIL), 0x3F800000u);
    ASSERT_EQ_U32(ksvm_f32_round_int(0xBF7FFFFFu, KSVM_F32_FLOOR), 0xBF800000u);
    ASSERT_EQ_U32(ksvm_f32_round_int(0xC0200000u, KSVM_F32_TRUNC), 0xC0000000u);
    ASSERT_EQ_U32(ksvm_f32_round_int(0xFF800000u, KSVM_F32_RNE), 0xFF800000u);
    /* FTOI/FTOU: truncam, saturam, NaN -> 0 */
    ASSERT_EQ_U32(ksvm_f32_to_i32(0x4F32D05Eu), 0x7FFFFFFFu);      /* 3e9 */
    ASSERT_EQ_U32(ksvm_f32_to_i32(0xCF32D05Eu), 0x80000000u);      /* -3e9 */
    ASSERT_EQ_U32(ksvm_f32_to_i32(0xCF000000u), 0x80000000u);      /* -2^31 exato */
    ASSERT_EQ_U32(ksvm_f32_to_i32(0x4EFFFFFFu), 0x7FFFFF80u);
    ASSERT_EQ_U32(ksvm_f32_to_i32(0xC0700000u), 0xFFFFFFFDu);      /* -3.75 -> -3 */
    ASSERT_EQ_U32(ksvm_f32_to_i32(0x7FC00000u), 0u);
    ASSERT_EQ_U32(ksvm_f32_to_i32(0xFF800000u), 0x80000000u);
    ASSERT_EQ_U32(ksvm_f32_to_u32(0xBFC00000u), 0u);               /* -1.5 */
    ASSERT_EQ_U32(ksvm_f32_to_u32(0xBF000000u), 0u);               /* -0.5 */
    ASSERT_EQ_U32(ksvm_f32_to_u32(0x4F95ED39u), 0xFFFFFFFFu);      /* 5e9 */
    ASSERT_EQ_U32(ksvm_f32_to_u32(0x4F7FFFFFu), 0xFFFFFF00u);
    ASSERT_EQ_U32(ksvm_f32_to_u32(0x7F800000u), 0xFFFFFFFFu);
    ASSERT_EQ_U32(ksvm_f32_to_u32(0xFFC00000u), 0u);
    /* ITOF/UTOF: RNE */
    ASSERT_EQ_U32(ksvm_i32_to_f32(16777217u), 0x4B800000u);        /* empate: par */
    ASSERT_EQ_U32(ksvm_i32_to_f32(16777219u), 0x4B800002u);
    ASSERT_EQ_U32(ksvm_i32_to_f32(0x80000000u), 0xCF000000u);
    ASSERT_EQ_U32(ksvm_i32_to_f32(0xFFFFFFFFu), 0xBF800000u);
    ASSERT_EQ_U32(ksvm_i32_to_f32(0u), 0u);
    ASSERT_EQ_U32(ksvm_u32_to_f32(0xFFFFFFFFu), 0x4F800000u);
    ASSERT_EQ_U32(ksvm_u32_to_f32(0xFFFFFF7Fu), 0x4F7FFFFFu);
    ASSERT_EQ_U32(ksvm_u32_to_f32(0xFFFFFF80u), 0x4F800000u);      /* empate: par */
}

TEST(spec_compare_flags)
{
    ASSERT_EQ_U32(ksvm_f32_cmp(0x3F800000u, 0x40000000u), F_N);           /* 1 < 2 */
    ASSERT_EQ_U32(ksvm_f32_cmp(0x40000000u, 0x40000000u), F_Z | F_C);
    ASSERT_EQ_U32(ksvm_f32_cmp(0x40000000u, 0x3F800000u), F_C);
    ASSERT_EQ_U32(ksvm_f32_cmp(0x7FC00000u, 0x3F800000u), F_C | F_V);
    ASSERT_EQ_U32(ksvm_f32_cmp(0x3F800000u, 0xFF800001u), F_C | F_V);
    ASSERT_EQ_U32(ksvm_f32_cmp(0x80000000u, 0x00000000u), F_Z | F_C);     /* -0 == +0 */
    ASSERT_EQ_U32(ksvm_f32_cmp(0xBF800000u, 0x80000001u), F_N);           /* -1 < -sub */
    ASSERT_EQ_U32(ksvm_f32_cmp(0xFF800000u, 0xFF7FFFFFu), F_N);
    ASSERT_EQ_U32(ksvm_f32_cmp(0x7F800000u, 0x7F800000u), F_Z | F_C);
}

/* ---- Tabela de bordas ------------------------------------------------- */

TEST(edge_pairs_binary)
{
    size_t k, i, j;
    for (k = 0; k < COUNT(bin_ops); k++) {
        for (i = 0; i < COUNT(edges); i++) {
            for (j = 0; j < COUNT(edges); j++) {
                if (!check_bin(t_, &bin_ops[k], edges[i], edges[j])) return;
            }
        }
    }
}

TEST(edge_values_unary)
{
    size_t k, i;
    for (k = 0; k < COUNT(un_ops); k++) {
        for (i = 0; i < COUNT(edges); i++) {
            if (!check_un(t_, &un_ops[k], edges[i])) return;
        }
    }
    for (k = 0; k < COUNT(int_ops); k++) {
        for (i = 0; i < COUNT(edges); i++) {
            if (!check_un(t_, &int_ops[k], edges[i])) return;
        }
    }
}

/* ---- Pseudoaleatório -------------------------------------------------- */

TEST(random_binary)
{
    size_t k;
    uint32_t n;
    for (k = 0; k < COUNT(bin_ops); k++) {
        uint32_t s = 0x2545F491u + (uint32_t)k;
        for (n = 0; n < SF_PAIRS; n++) {
            uint32_t a = gen_float(&s);
            uint32_t b = (n & 1u) ? gen_partner(&s, a) : gen_float(&s);
            if (!check_bin(t_, &bin_ops[k], a, b)) return;
        }
    }
}

/* Totalmente aleatórios (sem viés), para não depender só do gerador. */
TEST(random_binary_uniform)
{
    size_t k;
    uint32_t n, s = 0x9E3779B9u;
    for (k = 0; k < COUNT(bin_ops); k++) {
        for (n = 0; n < SF_PAIRS / 4u; n++) {
            uint32_t a = rnd(&s), b = rnd(&s);
            if (!check_bin(t_, &bin_ops[k], a, b)) return;
        }
    }
}

TEST(random_unary)
{
    size_t k;
    uint32_t n;
    for (k = 0; k < COUNT(un_ops); k++) {
        uint32_t s = 0x6A09E667u + (uint32_t)k;
        for (n = 0; n < SF_PAIRS; n++) {
            uint32_t a = (n & 3u) ? gen_float(&s) : rnd(&s);
            if (!check_un(t_, &un_ops[k], a)) return;
        }
    }
    for (k = 0; k < COUNT(int_ops); k++) {
        uint32_t s = 0xBB67AE85u + (uint32_t)k;
        for (n = 0; n < SF_PAIRS; n++) {
            if (!check_un(t_, &int_ops[k], gen_int(&s))) return;
        }
    }
}

/* Varredura de todo o espaço de 32 bits em passo fixo (todo expoente, os
 * dois sinais, mantissas variadas), operações de um operando. */
TEST(sweep_unary)
{
    size_t k;
    uint32_t a;
    for (k = 0; k < COUNT(un_ops); k++) {
        for (a = 0u; a <= 0xFFFFFFFFu - SF_SWEEP_STEP; a += SF_SWEEP_STEP) {
            if (!check_un(t_, &un_ops[k], a)) return;
        }
    }
    for (k = 0; k < COUNT(int_ops); k++) {
        for (a = 0u; a <= 0xFFFFFFFFu - SF_SWEEP_STEP; a += SF_SWEEP_STEP) {
            if (!check_un(t_, &int_ops[k], a)) return;
        }
    }
}

/* neg/abs: só o bit de sinal (inclusive em NaN); e batem com o host
 * fora de NaN. */
TEST(neg_abs_sign_only)
{
    uint32_t n, s = 0x510E527Fu;
    for (n = 0; n < SF_PAIRS; n++) {
        uint32_t a = (n & 1u) ? gen_float(&s) : rnd(&s);
        volatile float x = float_of(a), y = -x;
        ASSERT_EQ_U32(ksvm_f32_neg(a), a ^ SIGN);
        ASSERT_EQ_U32(ksvm_f32_abs(a), a & 0x7FFFFFFFu);
        if (!bits_nan(a)) {
            ASSERT_EQ_U32(ksvm_f32_neg(a), bits_of(y));
        }
    }
}

/* O checker de raiz não é vazio: rejeita o vizinho do resultado certo. */
TEST(sqrt_checker_rejects_neighbors)
{
    uint32_t n, s = 0x1F83D9ABu;
    for (n = 0; n < 1000u; n++) {
        uint32_t a = gen_float(&s) & 0x7FFFFFFFu, r;
        if (bits_nan(a) || a == 0u || a == 0x7F800000u) continue;
        r = ksvm_f32_sqrt(a);
        ASSERT_TRUE(sqrt_is_correct(a, r));
        ASSERT_FALSE(sqrt_is_correct(a, r + 1u));
        ASSERT_FALSE(sqrt_is_correct(a, r - 1u));
    }
}

SUITE(softfloat)
{
    RUN_TEST(spec_examples);
    RUN_TEST(spec_min_max);
    RUN_TEST(spec_round_and_convert);
    RUN_TEST(spec_compare_flags);
    RUN_TEST(edge_pairs_binary);
    RUN_TEST(edge_values_unary);
    RUN_TEST(random_binary);
    RUN_TEST(random_binary_uniform);
    RUN_TEST(random_unary);
    RUN_TEST(sweep_unary);
    RUN_TEST(neg_abs_sign_only);
    RUN_TEST(sqrt_checker_rejects_neighbors);
}

#else  /* !KSVM_ENABLE_FLOAT */

/* Sem a extensão F, softfloat.c compila vazio; a falta dos opcodes é
 * conferida em test_exec_float.c. */
SUITE(softfloat)
{
    (void)t_;
}

#endif /* KSVM_ENABLE_FLOAT */
