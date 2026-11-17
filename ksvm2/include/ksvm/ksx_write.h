/*
 * ksx_write.h — escritor mínimo de imagens .ksx em memória.
 *
 * Usado pelos testes e, futuramente, pelo assembler. Layout gerado:
 * cabeçalho (64 bytes) | código | .data | símbolos, cada seção alinhada a 4
 * com preenchimento zero; checksum Adler-32 calculado por último.
 */
#ifndef KSVM_KSX_WRITE_H
#define KSVM_KSX_WRITE_H

#include "types.h"

typedef struct ksvm_ksx_desc {
    uint32_t       entry;
    uint16_t       flags;       /* KSVM_KSX_FLAG_* (SYMS é ligado se sym_size > 0) */
    const uint8_t *code;
    uint32_t       code_size;
    const uint8_t *data;        /* pode ser NULL se data_size == 0 */
    uint32_t       data_size;
    uint32_t       bss_size;
    uint32_t       ram_base;    /* 0 = loader decide */
    uint32_t       stack_size;
    uint32_t       heap_min;    /* só compõe ram_min (informativo) */
    const uint8_t *sym;
    uint32_t       sym_size;
} ksvm_ksx_desc;

/* Tamanho total do arquivo, ou 0 se não cabe em 32 bits. */
uint32_t ksvm_ksx_size(const ksvm_ksx_desc *d);

/* Escreve em out[0..cap). KSVM_OK, KSVM_E_ARG ou KSVM_E_RANGE (cap pequeno).
 * *written recebe o tamanho em caso de sucesso. */
int ksvm_ksx_write(const ksvm_ksx_desc *d, uint8_t *out, uint32_t cap,
                   uint32_t *written);

#endif /* KSVM_KSX_WRITE_H */
