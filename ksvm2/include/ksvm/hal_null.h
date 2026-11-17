/*
 * hal_null.h — HAL determinística para testes.
 *
 * putc guarda a saída num buffer de tamanho fixo (o excedente é descartado,
 * mas contado); getc lê de um buffer de entrada; millis é o número de
 * instruções executadas mais o tempo "dormido" por sleep_ms. Nada depende
 * do relógio ou do SO.
 */
#ifndef KSVM_HAL_NULL_H
#define KSVM_HAL_NULL_H

#include "hal.h"

#ifndef KSVM_HAL_NULL_OUT_MAX
#define KSVM_HAL_NULL_OUT_MAX 256u
#endif

typedef struct ksvm_hal_null {
    uint8_t            out[KSVM_HAL_NULL_OUT_MAX];
    uint32_t           out_len;     /* bytes guardados em out */
    uint32_t           out_total;   /* bytes recebidos, inclusive descartados */
    const uint8_t     *in;
    uint32_t           in_len;
    uint32_t           in_pos;
    bool               in_eof;      /* esgotada a entrada: EOF em vez de NODATA */
    uint32_t           slept_ms;
    bool               halted;
    uint32_t           halt_code;
    const struct ksvm *vm;          /* relógio = vm->insn_count */
} ksvm_hal_null;

/* Zera nh e preenche hal com as funções da porta (hal->ctx = nh). */
void ksvm_hal_null_init(ksvm_hal_null *nh, ksvm_hal *hal, const struct ksvm *vm);

/* Define a entrada lida por getc. */
void ksvm_hal_null_set_input(ksvm_hal_null *nh, const uint8_t *in,
                             uint32_t len, bool eof);

#endif /* KSVM_HAL_NULL_H */
