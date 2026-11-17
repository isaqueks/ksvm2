/*
 * alu.h — operações aritméticas e lógicas de 32 bits com cálculo de flags.
 *
 * Convenção: funções que alteram flags recebem "flags" por ponteiro, leem o
 * que precisam (C de entrada em ADC/SBC) e escrevem apenas as flags que a
 * instrução afeta (doc 04 §5); as demais são preservadas. Toda aritmética é
 * feita em uint32_t (wrap definido); nenhum resultado depende de shift de
 * negativo, overflow com sinal ou divisão indefinida.
 */
#ifndef KSVM_ALU_H
#define KSVM_ALU_H

#include "types.h"

/* Escreve Z e N conforme res; preserva C e V. Devolve res. */
uint32_t ksvm_alu_zn(uint32_t res, uint32_t *flags);

/* ZNCV */
uint32_t ksvm_alu_add(uint32_t a, uint32_t b, uint32_t *flags);
uint32_t ksvm_alu_sub(uint32_t a, uint32_t b, uint32_t *flags);
uint32_t ksvm_alu_adc(uint32_t a, uint32_t b, uint32_t *flags);
uint32_t ksvm_alu_sbc(uint32_t a, uint32_t b, uint32_t *flags);

/* ZNC: C = último bit deslocado/rotacionado para fora; 0 se contagem & 31 = 0. */
uint32_t ksvm_alu_shl(uint32_t a, uint32_t n, uint32_t *flags);
uint32_t ksvm_alu_shr(uint32_t a, uint32_t n, uint32_t *flags);
uint32_t ksvm_alu_sar(uint32_t a, uint32_t n, uint32_t *flags);
uint32_t ksvm_alu_rol(uint32_t a, uint32_t n, uint32_t *flags);
uint32_t ksvm_alu_ror(uint32_t a, uint32_t n, uint32_t *flags);

/* Sem flags (o chamador aplica ksvm_alu_zn). */
uint32_t ksvm_alu_mul(uint32_t a, uint32_t b);
uint32_t ksvm_alu_mulhu(uint32_t a, uint32_t b);
uint32_t ksvm_alu_mulh(uint32_t a, uint32_t b);

/* Divisões: devolvem false (sem escrever *out) se b == 0.
 * DIV de INT32_MIN por -1 dá INT32_MIN; REM correspondente dá 0. */
bool ksvm_alu_div(uint32_t a, uint32_t b, uint32_t *out);
bool ksvm_alu_divu(uint32_t a, uint32_t b, uint32_t *out);
bool ksvm_alu_rem(uint32_t a, uint32_t b, uint32_t *out);
bool ksvm_alu_remu(uint32_t a, uint32_t b, uint32_t *out);

uint32_t ksvm_alu_clz(uint32_t a);
uint32_t ksvm_alu_ctz(uint32_t a);
uint32_t ksvm_alu_popcnt(uint32_t a);

#endif /* KSVM_ALU_H */
