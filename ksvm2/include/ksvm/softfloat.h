/*
 * softfloat.h — IEEE 754 binary32 em software, só com inteiros (extensão F,
 * doc 04 §5.7).
 *
 * Um float é o seu padrão de bits num uint32_t (sinal no bit 31, expoente
 * nos bits 30..23, fração nos 22..0), exatamente como fica num registrador
 * da VM. Nada aqui usa float/double do host: o resultado é bit a bit o
 * mesmo em qualquer plataforma.
 *
 * Regras (as da ISA, doc 04 §5.7):
 *   - arredondamento sempre para o mais próximo, empate para o par (RNE);
 *     sem registrador de controle, sem exceções, sem flags sticky;
 *   - subnormais suportados nas entradas e nas saídas (sem flush-to-zero);
 *   - divisão por zero dá ±inf; inf - inf, 0 * inf, 0 / 0, inf / inf e a
 *     raiz de negativo dão NaN;
 *   - todo NaN produzido ou propagado é o NaN canônico KSVM_F32_NAN
 *     (0x7FC00000), qualquer que seja o NaN de entrada. Exceções: neg/abs
 *     (só mexem no bit de sinal) e min/max com um único NaN (devolvem o
 *     outro operando).
 *
 * Com KSVM_ENABLE_FLOAT=0 este header não declara nada e softfloat.c
 * compila vazio.
 */
#ifndef KSVM_SOFTFLOAT_H
#define KSVM_SOFTFLOAT_H

#include "types.h"

#if KSVM_ENABLE_FLOAT

#define KSVM_F32_NAN      UINT32_C(0x7FC00000)   /* NaN canônico */
#define KSVM_F32_INF      UINT32_C(0x7F800000)
#define KSVM_F32_SIGN     UINT32_C(0x80000000)

/* Direção de ksvm_f32_round_int (FRINT, FFLOOR, FCEIL, FTRUNC). */
typedef enum ksvm_f32_rounding {
    KSVM_F32_RNE = 0,     /* mais próximo, empate para o par */
    KSVM_F32_FLOOR,       /* para -inf */
    KSVM_F32_CEIL,        /* para +inf */
    KSVM_F32_TRUNC        /* para zero */
} ksvm_f32_rounding;

uint32_t ksvm_f32_add(uint32_t a, uint32_t b);
uint32_t ksvm_f32_sub(uint32_t a, uint32_t b);
uint32_t ksvm_f32_mul(uint32_t a, uint32_t b);
uint32_t ksvm_f32_div(uint32_t a, uint32_t b);
uint32_t ksvm_f32_sqrt(uint32_t a);

/* min/max: um NaN só -> o outro operando; dois NaN -> canônico;
 * min(-0, +0) = -0 e max(-0, +0) = +0 (em qualquer ordem). */
uint32_t ksvm_f32_min(uint32_t a, uint32_t b);
uint32_t ksvm_f32_max(uint32_t a, uint32_t b);

/* Só o bit de sinal, inclusive em NaN (o payload é preservado). */
uint32_t ksvm_f32_neg(uint32_t a);
uint32_t ksvm_f32_abs(uint32_t a);

/* Inteiro mais próximo na direção pedida, ainda como float. Preserva o
 * sinal de zero (ceil(-0.5) = -0, rint(-0.4) = -0); inf volta igual. */
uint32_t ksvm_f32_round_int(uint32_t a, ksvm_f32_rounding mode);

/* float -> inteiro truncando para zero, com saturação: f32_to_i32 em
 * [INT32_MIN, INT32_MAX] (resultado em complemento de dois), f32_to_u32 em
 * [0, UINT32_MAX] (todo negativo dá 0). NaN dá 0. */
uint32_t ksvm_f32_to_i32(uint32_t a);
uint32_t ksvm_f32_to_u32(uint32_t a);

/* inteiro -> float com arredondamento RNE (a em complemento de dois em
 * i32_to_f32). */
uint32_t ksvm_i32_to_f32(uint32_t a);
uint32_t ksvm_u32_to_f32(uint32_t a);

/* Flags ZNCV de FCMP a, b (convenção ARM VFP, doc 04 §5.7):
 *   a < b      N        a == b  Z C     a > b  C     unordered (NaN)  C V
 * +0 e -0 são iguais. */
uint32_t ksvm_f32_cmp(uint32_t a, uint32_t b);

#endif /* KSVM_ENABLE_FLOAT */

#endif /* KSVM_SOFTFLOAT_H */
