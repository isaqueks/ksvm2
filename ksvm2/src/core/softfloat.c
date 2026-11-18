/*
 * softfloat.c — IEEE 754 binary32 só com inteiros (ver softfloat.h).
 *
 * O desenho segue o Berkeley SoftFloat 3 (John R. Hauser, licença BSD):
 * mesma representação intermediária e mesma rotina final de arredondamento;
 * o código é escrito do zero no estilo do projeto, e a raiz quadrada e a
 * divisão sem uint64_t usam algoritmos dígito a dígito no lugar das
 * aproximações por tabela do original.
 *
 * Representação intermediária: um valor finito não nulo é
 *     sig * 2^(exp - 156)
 * com sig < 2^31 em uint32_t e exp em int32_t. Normalizado, o bit
 * implícito fica no bit 30 e os bits 6..0 são de arredondamento: o bit 6 é
 * o de "meio" e os bits 5..0 só dizem se sobrou algo abaixo dele (sticky).
 * Com o bit 30 ligado, exp é o expoente com viés MENOS 1: ao empacotar,
 * (exp << 23) + (sig >> 7) soma o bit implícito ao expoente. Isso faz o
 * caso subnormal (exp = 0, sem bit implícito) e o arredondamento que sobe
 * de subnormal para normal ou de 1.111.. para 10.000.. saírem de graça.
 *
 * Portabilidade (doc 08 §1): só uint32_t/int32_t (int pode ter 16 bits);
 * todo shift tem contagem < 32 e é feito em valor sem sinal; nenhuma
 * aritmética com sinal pode estourar (expoentes ficam em ±600); 64 bits só
 * em div_sig e no ksvm_alu_mulhu, ambos com caminho só-32-bit para
 * KSVM_NO_INT64.
 */
#include "ksvm/softfloat.h"

#if KSVM_ENABLE_FLOAT

#include "ksvm/alu.h"
#include "ksvm/isa.h"

#define SIGN_BIT   KSVM_F32_SIGN
#define ABS_MASK   UINT32_C(0x7FFFFFFF)
#define EXP_MASK   UINT32_C(0x7F800000)
#define FRAC_MASK  UINT32_C(0x007FFFFF)
#define HIDDEN_BIT UINT32_C(0x00800000)
#define F32_ONE    UINT32_C(0x3F800000)
#define SIG_TOP    UINT32_C(0x40000000)   /* bit implícito normalizado */

/* ---- Classificação e campos ------------------------------------------- */

static bool is_nan(uint32_t a)  { return (a & ABS_MASK) > EXP_MASK; }
static bool is_inf(uint32_t a)  { return (a & ABS_MASK) == EXP_MASK; }
static bool is_zero(uint32_t a) { return (a & ABS_MASK) == 0u; }
static bool is_neg(uint32_t a)  { return (a & SIGN_BIT) != 0u; }

static uint32_t exp_field(uint32_t a)
{
    return (a >> 23) & 0xFFu;
}

/* a >> dist com OR dos bits que saíram no bit 0 (sticky). dist pode ser
 * qualquer valor: de 32 em diante sobra só o sticky. */
static uint32_t shr_jam(uint32_t a, uint32_t dist)
{
    if (dist == 0u) {
        return a;
    }
    if (dist < 32u) {
        return (a >> dist) | (((a << (32u - dist)) != 0u) ? 1u : 0u);
    }
    return (a != 0u) ? 1u : 0u;
}

/* Finito não nulo -> significando com o bit implícito no bit 23 e expoente
 * com viés; subnormais são normalizados (expoente <= 0). */
static uint32_t unpack(uint32_t a, int32_t *exp)
{
    uint32_t e = exp_field(a), sig = a & FRAC_MASK, shift;

    if (e != 0u) {
        *exp = (int32_t)e;
        return sig | HIDDEN_BIT;
    }
    shift = ksvm_alu_clz(sig) - 8u;          /* sig != 0: 1..23 */
    *exp = 1 - (int32_t)shift;
    return sig << shift;
}

/* ---- Arredondamento e empacotamento ----------------------------------- */

/*
 * sig: bit implícito no bit 30 (ou abaixo se o resultado é subnormal e
 * exp <= 0), sticky já incluído. Arredonda RNE e empacota com o sinal.
 * Resultado < 2^-149 * 1/2 vira zero; >= FLT_MAX + 1/2 ulp vira inf.
 */
static uint32_t round_pack(uint32_t sign, int32_t exp, uint32_t sig)
{
    uint32_t round_bits;

    if (exp < 0) {
        sig = shr_jam(sig, (uint32_t)(0 - exp));   /* subnormal (ou zero) */
        exp = 0;
    } else if (exp >= 0xFD) {
        /* 0xFD + 1 = 0xFE é o último expoente finito; o arredondamento
         * ainda pode levar sig a 2^31 (expoente 0xFF). */
        if (exp > 0xFD || sig + 0x40u >= SIGN_BIT) {
            return sign | KSVM_F32_INF;
        }
    }
    round_bits = sig & 0x7Fu;
    sig = (sig + 0x40u) >> 7;
    if (round_bits == 0x40u) {
        sig &= ~UINT32_C(1);                 /* empate exato: para o par */
    }
    return sign | (((uint32_t)exp << 23) + sig);
}

/* Como round_pack, para sig != 0 (< 2^31) em qualquer posição. */
static uint32_t norm_round_pack(uint32_t sign, int32_t exp, uint32_t sig)
{
    uint32_t shift = ksvm_alu_clz(sig) - 1u;

    return round_pack(sign, exp - (int32_t)shift, sig << shift);
}

/* ---- Soma e subtração ------------------------------------------------- */

/*
 * a + b. Os significandos vão para o bit 29 (o bit 30 recebe o carry da
 * soma); o menor é alinhado com sticky. Na subtração com expoentes que
 * diferem de 2 ou mais o resultado perde no máximo 2 posições na
 * normalização, então o sticky continua abaixo do bit de meio; com
 * diferença 0 ou 1 nada sai pela direita e o cancelamento é exato.
 */
static uint32_t add_signed(uint32_t a, uint32_t b)
{
    uint32_t ma, mb, sig, t;
    int32_t ea, eb;

    if (is_nan(a) || is_nan(b)) {
        return KSVM_F32_NAN;
    }
    if (is_inf(a)) {
        return (is_inf(b) && ((a ^ b) & SIGN_BIT) != 0u) ? KSVM_F32_NAN : a;
    }
    if (is_inf(b)) {
        return b;
    }
    if ((a & ABS_MASK) < (b & ABS_MASK)) {   /* |a| >= |b| daqui em diante */
        t = a;
        a = b;
        b = t;
    }
    if (is_zero(b)) {
        /* x + 0 = x; +0 + -0 = +0 e -0 + -0 = -0 (RNE) */
        return is_zero(a) ? (a & b) : a;
    }
    ma = unpack(a, &ea) << 6;
    mb = unpack(b, &eb) << 6;
    mb = shr_jam(mb, (uint32_t)(ea - eb));   /* ea >= eb */
    if (((a ^ b) & SIGN_BIT) == 0u) {
        sig = ma + mb;
    } else {
        sig = ma - mb;
        if (sig == 0u) {
            return 0u;                       /* x - x = +0 */
        }
    }
    return norm_round_pack(a & SIGN_BIT, ea, sig);
}

uint32_t ksvm_f32_add(uint32_t a, uint32_t b)
{
    return add_signed(a, b);
}

uint32_t ksvm_f32_sub(uint32_t a, uint32_t b)
{
    return add_signed(a, b ^ SIGN_BIT);      /* NaN continua NaN */
}

/* ---- Multiplicação ---------------------------------------------------- */

/*
 * Significandos nos bits 30 e 31: o produto de 64 bits tem o bit mais alto
 * no 61 ou 62; a palavra alta, com o sticky da baixa, já tem o formato de
 * round_pack (ajustado de 1 bit se precisar). A palavra alta vem de
 * ksvm_alu_mulhu, que tem o caminho sem uint64_t (KSVM_NO_INT64).
 */
uint32_t ksvm_f32_mul(uint32_t a, uint32_t b)
{
    uint32_t sign = (a ^ b) & SIGN_BIT, ma, mb, sig;
    int32_t ea, eb, exp;

    if (is_nan(a) || is_nan(b)) {
        return KSVM_F32_NAN;
    }
    if (is_inf(a) || is_inf(b)) {
        return (is_zero(a) || is_zero(b)) ? KSVM_F32_NAN : (sign | KSVM_F32_INF);
    }
    if (is_zero(a) || is_zero(b)) {
        return sign;
    }
    ma = unpack(a, &ea) << 7;
    mb = unpack(b, &eb) << 8;
    sig = ksvm_alu_mulhu(ma, mb) | ((ksvm_alu_mul(ma, mb) != 0u) ? 1u : 0u);
    exp = ea + eb - 0x7F;
    if (sig < SIG_TOP) {
        exp--;
        sig <<= 1;
    }
    return round_pack(sign, exp, sig);
}

/* ---- Divisão ---------------------------------------------------------- */

/*
 * floor(a * 2^shift / b) com "resto != 0" no bit 0. a e b têm o bit 23
 * ligado, a < 2b e shift é 30 ou 31 conforme a >= b: o quociente tem o
 * bit mais alto no 30.
 */
static uint32_t div_sig(uint32_t a, uint32_t b, unsigned shift)
{
#if KSVM_NO_INT64
    /* Divisão longa restauradora, um bit por passo: o resto fica < b < 2^24
     * e cabe folgado em 32 bits depois do shift. */
    uint32_t q = 0u, r = a;
    unsigned i;

    if (r >= b) {
        r -= b;
        q = 1u;
    }
    for (i = 0; i < shift; i++) {
        r <<= 1;
        q <<= 1;
        if (r >= b) {
            r -= b;
            q |= 1u;
        }
    }
    return q | ((r != 0u) ? 1u : 0u);
#else
    uint64_t num = (uint64_t)a << shift;
    uint32_t q = (uint32_t)(num / b);

    /* Se os 6 bits baixos já não são zero, o sticky não muda o
     * arredondamento (nem o de subnormal, ver round_pack). */
    if ((q & 0x3Fu) == 0u && (uint64_t)q * b != num) {
        q |= 1u;
    }
    return q;
#endif
}

uint32_t ksvm_f32_div(uint32_t a, uint32_t b)
{
    uint32_t sign = (a ^ b) & SIGN_BIT, ma, mb;
    int32_t ea, eb, exp;
    unsigned shift = 30u;

    if (is_nan(a) || is_nan(b)) {
        return KSVM_F32_NAN;
    }
    if (is_inf(a)) {
        return is_inf(b) ? KSVM_F32_NAN : (sign | KSVM_F32_INF);
    }
    if (is_inf(b)) {
        return sign;
    }
    if (is_zero(b)) {
        return is_zero(a) ? KSVM_F32_NAN : (sign | KSVM_F32_INF);   /* x / 0 = ±inf */
    }
    if (is_zero(a)) {
        return sign;
    }
    ma = unpack(a, &ea);
    mb = unpack(b, &eb);
    exp = ea - eb + 0x7E;
    if (ma < mb) {
        exp--;
        shift = 31u;
    }
    return round_pack(sign, exp, div_sig(ma, mb, shift));
}

/* ---- Raiz quadrada ---------------------------------------------------- */

/*
 * Raiz inteira dígito a dígito (dois bits do radicando por passo) de
 * x * 2^27, com x = significando (expoente sem viés par) ou o dobro dele
 * (ímpar): o radicando fica em [2^50, 2^52) e a raiz em [2^25, 2^26), 24
 * bits + 2 de guarda; o resto != 0 é o sticky. A raiz de um float nunca cai
 * exatamente no meio de dois floats, então 2 bits de guarda bastam.
 *
 * Expoente: com E = e - 127 (sem viés), o resultado tem expoente floor(E/2),
 * isto é, exp de round_pack = floor((e + 127) / 2) - 1 (e + 127 > 0 mesmo
 * para subnormais normalizados, e >= -22). E é ímpar quando e é par.
 */
uint32_t ksvm_f32_sqrt(uint32_t a)
{
    uint32_t m, x, src, rem = 0u, root = 0u, trial;
    int32_t e;
    unsigned i;

    if (is_nan(a)) {
        return KSVM_F32_NAN;
    }
    if (is_zero(a)) {
        return a;                            /* sqrt(-0) = -0 */
    }
    if (is_neg(a)) {
        return KSVM_F32_NAN;                 /* inclusive -inf */
    }
    if (is_inf(a)) {
        return a;
    }
    m = unpack(a, &e);
    x = (((uint32_t)e & 1u) == 0u) ? (m << 1) : m;   /* < 2^25 */
    src = x << 7;                            /* bits 51..20 do radicando */
    for (i = 0; i < 26u; i++) {
        rem = (rem << 2) | (src >> 30);      /* rem <= 2 * root < 2^27 */
        src <<= 2;
        trial = (root << 2) | 1u;
        root <<= 1;
        if (rem >= trial) {
            rem -= trial;
            root |= 1u;
        }
    }
    /* e + 127 > 0: shift sem sinal em vez de divisão com sinal (libgcc no AVR) */
    return round_pack(0u, (int32_t)((uint32_t)(e + 127) >> 1) - 1,
                      (root << 5) | ((rem != 0u) ? 1u : 0u));
}

/* ---- Mínimo, máximo, sinal -------------------------------------------- */

/* a < b para a e b não NaN, com -0 < +0 (ordem de min/max). */
static bool total_lt(uint32_t a, uint32_t b)
{
    if (((a ^ b) & SIGN_BIT) != 0u) {
        return is_neg(a);
    }
    return is_neg(a) ? (a > b) : (a < b);    /* sinal-magnitude */
}

static uint32_t min_max(uint32_t a, uint32_t b, bool want_max)
{
    bool na = is_nan(a), nb = is_nan(b);

    if (na || nb) {
        return (na && nb) ? KSVM_F32_NAN : (na ? b : a);
    }
    return (total_lt(a, b) != want_max) ? a : b;
}

uint32_t ksvm_f32_min(uint32_t a, uint32_t b)
{
    return min_max(a, b, false);
}

uint32_t ksvm_f32_max(uint32_t a, uint32_t b)
{
    return min_max(a, b, true);
}

uint32_t ksvm_f32_neg(uint32_t a)
{
    return a ^ SIGN_BIT;
}

uint32_t ksvm_f32_abs(uint32_t a)
{
    return a & ABS_MASK;
}

/* ---- Arredondamento para inteiro -------------------------------------- */

/*
 * Com expoente e em [127, 150), os bits de fração do padrão de bits estão
 * abaixo de unit = 2^(150 - e), que é o bit das unidades (para e = 127 é o
 * bit baixo do expoente, que vale 1: a parte inteira é 1, ímpar). Somar
 * unit ao padrão aumenta a magnitude de 1 com o carry indo para o expoente
 * quando precisa, então tudo é aritmética no padrão de bits.
 */
uint32_t ksvm_f32_round_int(uint32_t a, ksvm_f32_rounding mode)
{
    uint32_t e = exp_field(a), sign = a & SIGN_BIT, unit, frac_mask, frac, half;
    bool up;

    if (e == 0xFFu) {
        return is_nan(a) ? KSVM_F32_NAN : a;
    }
    if (e >= 150u || is_zero(a)) {
        return a;                            /* já inteiro (ou ±0) */
    }
    if (e < 127u) {                          /* 0 < |a| < 1 */
        switch (mode) {
        case KSVM_F32_RNE:
            /* > 1/2 vai para 1; exatamente 1/2 empata com 0 (par) */
            return (e == 126u && (a & FRAC_MASK) != 0u) ? (sign | F32_ONE) : sign;
        case KSVM_F32_FLOOR: return (sign != 0u) ? (SIGN_BIT | F32_ONE) : 0u;
        case KSVM_F32_CEIL:  return (sign != 0u) ? SIGN_BIT : F32_ONE;
        default:             return sign;
        }
    }
    unit = UINT32_C(1) << (150u - e);        /* 2^1 .. 2^23 */
    frac_mask = unit - 1u;
    frac = a & frac_mask;
    if (frac == 0u) {
        return a;
    }
    switch (mode) {
    case KSVM_F32_RNE:
        half = unit >> 1;
        up = frac > half || (frac == half && (a & unit) != 0u);   /* empate: par */
        break;
    case KSVM_F32_FLOOR: up = sign != 0u; break;   /* magnitude sobe se negativo */
    case KSVM_F32_CEIL:  up = sign == 0u; break;
    default:             up = false;      break;
    }
    a &= ~frac_mask;
    return up ? a + unit : a;
}

/* ---- Conversões ------------------------------------------------------- */

/* Magnitude inteira truncada de |a|, com e = expoente em [127, 158]. */
static uint32_t int_magnitude(uint32_t a, uint32_t e)
{
    uint32_t m = (a & FRAC_MASK) | HIDDEN_BIT;

    return (e >= 150u) ? (m << (e - 150u)) : (m >> (150u - e));
}

uint32_t ksvm_f32_to_i32(uint32_t a)
{
    uint32_t e = exp_field(a), mag;

    if (is_nan(a) || e < 127u) {
        return 0u;                           /* NaN, ±0, |a| < 1 */
    }
    if (e >= 158u) {                         /* |a| >= 2^31 (inclusive inf) */
        return is_neg(a) ? UINT32_C(0x80000000) : UINT32_C(0x7FFFFFFF);
    }
    mag = int_magnitude(a, e);               /* < 2^31 */
    return is_neg(a) ? (uint32_t)(0u - mag) : mag;
}

uint32_t ksvm_f32_to_u32(uint32_t a)
{
    uint32_t e = exp_field(a);

    if (is_nan(a) || is_neg(a) || e < 127u) {
        return 0u;                           /* negativo trunca/satura em 0 */
    }
    if (e >= 159u) {                         /* >= 2^32 (inclusive +inf) */
        return UINT32_C(0xFFFFFFFF);
    }
    return int_magnitude(a, e);
}

/* Inteiro sem sinal mag -> float com o sinal dado: mag = sig * 2^(156 - 156). */
static uint32_t from_magnitude(uint32_t sign, uint32_t mag)
{
    if (mag == 0u) {
        return 0u;                           /* inteiro 0 -> +0 */
    }
    if ((mag & SIGN_BIT) != 0u) {            /* bit 31: desce 1 com sticky */
        return round_pack(sign, 157, shr_jam(mag, 1u));
    }
    return norm_round_pack(sign, 156, mag);
}

uint32_t ksvm_i32_to_f32(uint32_t a)
{
    return is_neg(a) ? from_magnitude(SIGN_BIT, (uint32_t)(0u - a)) : from_magnitude(0u, a);
}

uint32_t ksvm_u32_to_f32(uint32_t a)
{
    return from_magnitude(0u, a);
}

/* ---- Comparação ------------------------------------------------------- */

uint32_t ksvm_f32_cmp(uint32_t a, uint32_t b)
{
    if (is_nan(a) || is_nan(b)) {
        return KSVM_FLAG_C | KSVM_FLAG_V;    /* unordered */
    }
    if (a == b || (is_zero(a) && is_zero(b))) {
        return KSVM_FLAG_Z | KSVM_FLAG_C;
    }
    return total_lt(a, b) ? KSVM_FLAG_N : KSVM_FLAG_C;
}

#else  /* !KSVM_ENABLE_FLOAT */

/* ISO C proíbe unidade de tradução vazia. */
typedef int ksvm_softfloat_disabled;

#endif /* KSVM_ENABLE_FLOAT */
