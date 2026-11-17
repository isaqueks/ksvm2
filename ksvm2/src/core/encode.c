/*
 * encode.c — montagem de palavras de instrução (doc 04 §3, doc 07 §3).
 *
 * Imediatos com sinal são convertidos para uint32_t (conversão definida:
 * módulo 2^32) e depois mascarados à largura do campo.
 */
#include "ksvm/encode.h"
#include "ksvm/isa.h"

static uint32_t reg5(uint8_t r)
{
    return (uint32_t)r & 0x1Fu;
}

uint32_t ksvm_enc_r(uint8_t op, uint8_t ra, uint8_t rb, uint8_t rc)
{
    return (uint32_t)op | (reg5(ra) << 8) | (reg5(rb) << 13) | (reg5(rc) << 18);
}

uint32_t ksvm_enc_i(uint8_t op, uint8_t ra, uint8_t rb, int32_t imm14)
{
    uint32_t imm = (uint32_t)imm14 & UINT32_C(0x3FFF);
    return (uint32_t)op | (reg5(ra) << 8) | (reg5(rb) << 13) | (imm << 18);
}

/* Bits 18..31 reservados (zero) na primeira palavra do formato L. */
uint32_t ksvm_enc_l(uint8_t op, uint8_t ra, uint8_t rb)
{
    return ksvm_enc_r(op, ra, rb, 0u);
}

/* Deslocamentos são armazenados em palavras: off_bytes / 4 em complemento
 * de dois, obtido pelo shift lógico do padrão de bits. */
uint32_t ksvm_enc_b(uint8_t cond, int32_t off_bytes)
{
    uint32_t words = ((uint32_t)off_bytes >> 2) & UINT32_C(0xFFFFF);
    return (uint32_t)KSVM_OP_BCOND | (((uint32_t)cond & 0x0Fu) << 8) | (words << 12);
}

uint32_t ksvm_enc_j(uint8_t op, int32_t off_bytes)
{
    uint32_t words = ((uint32_t)off_bytes >> 2) & UINT32_C(0xFFFFFF);
    return (uint32_t)op | (words << 8);
}

bool ksvm_imm14_fits(int32_t imm)
{
    return imm >= KSVM_IMM14_MIN && imm <= KSVM_IMM14_MAX;
}

static bool aligned4(int32_t off)
{
    return ((uint32_t)off & 3u) == 0u;
}

/* off20: -2^19..2^19-1 palavras = -2^21..2^21-4 bytes. */
bool ksvm_off20_fits(int32_t off_bytes)
{
    return aligned4(off_bytes) &&
           off_bytes >= -(INT32_C(1) << 21) && off_bytes <= (INT32_C(1) << 21) - 4;
}

/* off24: -2^23..2^23-1 palavras = -2^25..2^25-4 bytes. */
bool ksvm_off24_fits(int32_t off_bytes)
{
    return aligned4(off_bytes) &&
           off_bytes >= -(INT32_C(1) << 25) && off_bytes <= (INT32_C(1) << 25) - 4;
}
