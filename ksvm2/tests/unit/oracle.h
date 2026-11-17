/*
 * oracle.h — oráculo de referência INDEPENDENTE de alu.c (doc 09 §3).
 *
 * Tudo é calculado em uint64_t/int64_t direto da definição do doc 04, sem
 * os truques de bits do núcleo: soma/subtração com carry/borrow pela faixa
 * do resultado em 64 bits, V pela faixa com sinal, deslocamentos e rotações
 * simulados um bit por vez (o último bit que sai é o C), contagem de bits
 * por laço. Se teste e núcleo concordarem, é porque os dois seguem o doc,
 * não porque compartilham o mesmo erro.
 */
#ifndef KSVM_TEST_ORACLE_H
#define KSVM_TEST_ORACLE_H

#include <stdint.h>

#include "ksvm/isa.h"

#define O_Z KSVM_FLAG_Z
#define O_N KSVM_FLAG_N
#define O_C KSVM_FLAG_C
#define O_V KSVM_FLAG_V

typedef struct o_res {
    uint32_t v;       /* resultado */
    uint32_t flags;   /* flags finais (já combinadas com as preservadas) */
} o_res;

/* Valor com sinal do padrão de 32 bits, sem conversão implementation-defined. */
static inline int64_t o_s32(uint32_t x)
{
    return (x & UINT32_C(0x80000000)) ? (int64_t)x - INT64_C(0x100000000) : (int64_t)x;
}

/* 32 bits baixos de um int64_t (conversão para unsigned é módulo 2^64). */
static inline uint32_t o_lo(int64_t v)
{
    return (uint32_t)((uint64_t)v & UINT64_C(0xFFFFFFFF));
}

static inline int o_fits_s32(int64_t s)
{
    return s >= -INT64_C(0x80000000) && s <= INT64_C(0x7FFFFFFF);
}

/* Z e N do resultado; demais bits vêm de "keep" (flags preservadas). */
static inline uint32_t o_zn(uint32_t res, uint32_t keep)
{
    uint32_t f = keep & (O_C | O_V);
    if (res == 0u) f |= O_Z;
    if (res >= UINT32_C(0x80000000)) f |= O_N;
    return f;
}

/* a + b + cin: C = resultado sem sinal >= 2^32; V = resultado com sinal
 * fora de [-2^31, 2^31). */
static inline o_res o_add(uint32_t a, uint32_t b, uint32_t cin)
{
    uint64_t u = (uint64_t)a + (uint64_t)b + (uint64_t)cin;
    int64_t  s = o_s32(a) + o_s32(b) + (int64_t)cin;
    o_res r;
    r.v = (uint32_t)(u & UINT64_C(0xFFFFFFFF));
    r.flags = o_zn(r.v, 0u);
    if (u > UINT64_C(0xFFFFFFFF)) r.flags |= O_C;
    if (!o_fits_s32(s)) r.flags |= O_V;
    return r;
}

/* a - b - borrow: C = não-borrow (diferença sem sinal >= 0), convenção ARM. */
static inline o_res o_sub(uint32_t a, uint32_t b, uint32_t borrow)
{
    int64_t d = (int64_t)a - (int64_t)b - (int64_t)borrow;
    int64_t s = o_s32(a) - o_s32(b) - (int64_t)borrow;
    o_res r;
    r.v = o_lo(d);
    r.flags = o_zn(r.v, 0u);
    if (d >= 0) r.flags |= O_C;
    if (!o_fits_s32(s)) r.flags |= O_V;
    return r;
}

static inline uint32_t o_mul(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a * (uint64_t)b) & UINT64_C(0xFFFFFFFF));
}

static inline uint32_t o_mulhu(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
}

/* |a*b| <= 2^62: cabe em int64_t. */
static inline uint32_t o_mulh(uint32_t a, uint32_t b)
{
    int64_t p = o_s32(a) * o_s32(b);
    return (uint32_t)((uint64_t)p >> 32);
}

/* C99: "/" e "%" truncam para zero. Em 64 bits INT32_MIN / -1 = 2^31 cabe
 * e os 32 bits baixos são 0x80000000, exatamente o que o doc 04 define. */
static inline uint32_t o_div(uint32_t a, uint32_t b)  { return o_lo(o_s32(a) / o_s32(b)); }
static inline uint32_t o_rem(uint32_t a, uint32_t b)  { return o_lo(o_s32(a) % o_s32(b)); }
static inline uint32_t o_divu(uint32_t a, uint32_t b) { return (uint32_t)((uint64_t)a / b); }
static inline uint32_t o_remu(uint32_t a, uint32_t b) { return (uint32_t)((uint64_t)a % b); }

/* Deslocamentos, um bit por vez. kind: 0 SHL, 1 SHR, 2 SAR, 3 ROL, 4 ROR.
 * Contagem & 31; contagem 0 = valor intacto e C = 0 (alu.h). V preservado. */
enum { O_SHL, O_SHR, O_SAR, O_ROL, O_ROR };

static inline o_res o_shift(int kind, uint32_t a, uint32_t count, uint32_t fin)
{
    uint32_t n = count % 32u, i, c = 0u;
    uint64_t x = a;
    o_res r;
    for (i = 0; i < n; i++) {
        uint64_t top = (x >> 31) & 1u, bot = x & 1u;
        switch (kind) {
        case O_SHL: c = (uint32_t)top; x = (x << 1) & UINT64_C(0xFFFFFFFF); break;
        case O_SHR: c = (uint32_t)bot; x >>= 1; break;
        case O_SAR: c = (uint32_t)bot; x = (x >> 1) | (top << 31); break;
        case O_ROL: c = (uint32_t)top; x = ((x << 1) & UINT64_C(0xFFFFFFFF)) | top; break;
        default:    c = (uint32_t)bot; x = (x >> 1) | (bot << 31); break;
        }
    }
    r.v = (uint32_t)x;
    r.flags = o_zn(r.v, fin & O_V);
    if (c) r.flags |= O_C;
    return r;
}

static inline uint32_t o_clz(uint32_t a)
{
    uint32_t n = 0;
    int i;
    for (i = 31; i >= 0 && ((a >> i) & 1u) == 0u; i--) n++;
    return n;
}

static inline uint32_t o_ctz(uint32_t a)
{
    uint32_t n = 0;
    while (n < 32u && ((a >> n) & 1u) == 0u) n++;
    return n;
}

static inline uint32_t o_popcnt(uint32_t a)
{
    uint32_t n = 0, i;
    for (i = 0; i < 32u; i++) n += (a >> i) & 1u;
    return n;
}

/* Extensão de sinal pela definição: valor dos k bits em complemento de dois. */
static inline uint32_t o_sext(uint32_t a, unsigned bits)
{
    uint64_t m = (UINT64_C(1) << bits) - 1u;
    int64_t v = (int64_t)(a & m);
    if (v >= (int64_t)(UINT64_C(1) << (bits - 1u))) v -= (int64_t)(UINT64_C(1) << bits);
    return o_lo(v);
}

/* Tabela literal do doc 04 §4: linha = condição 0..15, coluna = flags
 * 0..15 (bit0 Z, bit1 N, bit2 C, bit3 V), '1' = desvia. Escrita à mão. */
static const char *const o_cond_table[16] = {
    /*        flags: 0123456789ABCDEF */
    /* AL */ "1111111111111111",
    /* EQ */ "0101010101010101",
    /* NE */ "1010101010101010",
    /* HS */ "0000111100001111",
    /* LO */ "1111000011110000",
    /* MI */ "0011001100110011",
    /* PL */ "1100110011001100",
    /* VS */ "0000000011111111",
    /* VC */ "1111111100000000",
    /* HI */ "0000101000001010",
    /* LS */ "1111010111110101",
    /* GE */ "1100110000110011",
    /* LT */ "0011001111001100",
    /* GT */ "1000100000100010",
    /* LE */ "0111011111011101",
    /* NV */ "0000000000000000",
};

#endif /* KSVM_TEST_ORACLE_H */
