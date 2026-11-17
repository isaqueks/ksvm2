/*
 * encode.h — codificadores de instrução (doc 07 §3), compartilhados pelos
 * testes e, futuramente, pelo assembler.
 *
 * Campos acima da largura do formato são truncados (mascarados); use as
 * funções *_fits para validar antes. Deslocamentos de desvio são em BYTES,
 * relativos a PC_next, e devem ser múltiplos de 4.
 */
#ifndef KSVM_ENCODE_H
#define KSVM_ENCODE_H

#include "types.h"

uint32_t ksvm_enc_r(uint8_t op, uint8_t ra, uint8_t rb, uint8_t rc);
uint32_t ksvm_enc_i(uint8_t op, uint8_t ra, uint8_t rb, int32_t imm14);
/* Primeira palavra do formato L; o imm32 vai na palavra seguinte. */
uint32_t ksvm_enc_l(uint8_t op, uint8_t ra, uint8_t rb);
/* BCOND com condição "cond". */
uint32_t ksvm_enc_b(uint8_t cond, int32_t off_bytes);
uint32_t ksvm_enc_j(uint8_t op, int32_t off_bytes);

bool ksvm_imm14_fits(int32_t imm);
bool ksvm_off20_fits(int32_t off_bytes);
bool ksvm_off24_fits(int32_t off_bytes);

#endif /* KSVM_ENCODE_H */
