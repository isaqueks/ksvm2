/*
 * bytes.h — acesso little-endian e extensão de sinal sem comportamento
 * indefinido nem dependência do endianness/alinhamento do host.
 *
 * Por que shifts byte a byte: casts (uint32_t *) sobre uint8_t * são UB
 * (alinhamento, strict aliasing) e quebram em Cortex-M0 e big-endian. Cada
 * byte é convertido para um tipo sem sinal ANTES do shift: em AVR "int" tem
 * 16 bits e (uint8_t << 8) promovido a int já pode estourar.
 */
#ifndef KSVM_BYTES_H
#define KSVM_BYTES_H

#include <stdint.h>

static inline uint16_t ksvm_ld16le(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static inline uint32_t ksvm_ld32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline void ksvm_st16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static inline void ksvm_st32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

/*
 * Estende o sinal dos "bits" bits baixos de x (1..32) para 32 bits.
 * (x ^ sinal) - sinal em uint32_t: aritmética modular, sem shift de
 * negativos (implementation-defined em C99).
 */
static inline uint32_t ksvm_sext(uint32_t x, unsigned bits)
{
    uint32_t sign = (uint32_t)1 << (bits - 1u);
    uint32_t mask = (sign << 1) - 1u;   /* bits = 32: sign<<1 = 0, mask = ~0 */
    return ((x & mask) ^ sign) - sign;
}

static inline uint32_t ksvm_sext8(uint32_t x)  { return ksvm_sext(x, 8); }
static inline uint32_t ksvm_sext14(uint32_t x) { return ksvm_sext(x, 14); }
static inline uint32_t ksvm_sext16(uint32_t x) { return ksvm_sext(x, 16); }
static inline uint32_t ksvm_sext20(uint32_t x) { return ksvm_sext(x, 20); }
static inline uint32_t ksvm_sext24(uint32_t x) { return ksvm_sext(x, 24); }

/*
 * Converte o padrão de bits para int32_t sem a conversão implementation-
 * defined de C99 6.3.1.3 (valor > INT32_MAX): -(~x) - 1 cabe sempre.
 */
static inline int32_t ksvm_u32_to_i32(uint32_t x)
{
    if ((x & UINT32_C(0x80000000)) == 0u) {
        return (int32_t)x;
    }
    return -(int32_t)(~x) - 1;
}

#endif /* KSVM_BYTES_H */
