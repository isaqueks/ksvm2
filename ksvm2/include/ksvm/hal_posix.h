/*
 * hal_posix.h — HAL para Linux, macOS e *BSD (doc 06 §4).
 *
 *   putc      buffer próprio despejado com write(2) em '\n', buffer cheio,
 *             antes de ler a entrada, dormir, parar ou escrever por WRITE
 *   getc      read(2) de 1 byte; não bloqueante (poll com timeout 0) ou
 *             bloqueante, conforme stdin_block
 *   millis    clock_gettime(CLOCK_MONOTONIC) desde o init
 *   sleep_ms  nanosleep
 *   syscall   MICROS (0x22) e, com KSVM_ENABLE_SYSCALL_FS=1 e fs_enabled,
 *             o grupo 3 (OPEN/CLOSE/READ/WRITE/SEEK; fd 0/1/2 pré-abertos;
 *             caminhos relativos ao diretório atual, sem ".." nem absolutos)
 *
 * Uso:
 *     ksvm_hal_posix hp;  ksvm_hal hal;
 *     ksvm_hal_posix_init(&hp, &hal);
 *     hp.stdin_block = true;              (opcional, após o init)
 *     ksvm_init(&vm, &hal); ... ksvm_run ...
 *     ksvm_hal_posix_close_all(&hp);      (despeja a saída, fecha arquivos)
 */
#ifndef KSVM_HAL_POSIX_H
#define KSVM_HAL_POSIX_H

#include "hal.h"

#define KSVM_HAL_POSIX_MAX_FDS 16u
#define KSVM_HAL_POSIX_OUT_BUF 512u

typedef struct ksvm_hal_posix {
    int      in_fd;                          /* entrada do guest (0) */
    int      out_fd;                         /* saída do guest (1) */
    bool     stdin_block;                    /* GETC bloqueia esperando dado */
    bool     fs_enabled;                     /* grupo 3 ligado (padrão: true) */
    bool     in_eof;                         /* entrada terminou */
    bool     out_error;                      /* write(2) da saída falhou */
    uint64_t t0_us;                          /* relógio no init */
    int      fds[KSVM_HAL_POSIX_MAX_FDS];     /* fd do guest -> fd do host, -1 livre */
    uint32_t out_len;
    uint8_t  out[KSVM_HAL_POSIX_OUT_BUF];
} ksvm_hal_posix;

/* Preenche hp com os padrões (stdin 0, stdout 1, não bloqueante, fs ligado)
 * e hal com as funções da porta (hal->ctx = hp). */
void ksvm_hal_posix_init(ksvm_hal_posix *hp, ksvm_hal *hal);

/* Despeja a saída pendente de putc. */
void ksvm_hal_posix_flush(ksvm_hal_posix *hp);

/* Despeja a saída e fecha os arquivos abertos pelo guest (fd >= 3). */
void ksvm_hal_posix_close_all(ksvm_hal_posix *hp);

#endif /* KSVM_HAL_POSIX_H */
