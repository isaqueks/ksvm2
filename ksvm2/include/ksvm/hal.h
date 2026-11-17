/*
 * hal.h — funções que o host fornece à VM (doc 03 §3.3, doc 06).
 *
 * Todo ponteiro pode ser NULL: o núcleo trata a ausência como "não
 * suportado" e devolve erro ao guest (r1 = -1, r2 = KSVM_ERR_NOSYS), nunca
 * entra em fault por isso.
 *
 * Nota: os membros se chamam putc/getc como no doc 03. Como algumas libcs
 * (avr-libc) definem putc/getc como macros de função, o núcleo os chama
 * entre parênteses — (hal->putc)(...) — o que impede a expansão da macro.
 */
#ifndef KSVM_HAL_H
#define KSVM_HAL_H

#include "types.h"

struct ksvm;

/* Retornos de getc (além de 0..255). */
#define KSVM_HAL_NODATA (-1)   /* nada disponível agora (não bloqueia) */
#define KSVM_HAL_EOF    (-2)   /* fim da entrada (extensão opcional) */

/* Retornos de hal->syscall. */
enum ksvm_sys_result {
    KSVM_SYS_OK      = 0,      /* tratada; r1/r2 já escritos pelo host */
    KSVM_SYS_UNKNOWN = 1,      /* núcleo devolve r1 = -1, r2 = KSVM_ERR_NOSYS */
    KSVM_SYS_YIELD   = 2       /* tratada; VM vai para KSVM_YIELD */
};

typedef struct ksvm_hal {
    void *ctx;
    /* I/O básico */
    int  (*putc)(void *ctx, uint8_t c);              /* < 0 em erro */
    int  (*getc)(void *ctx);                         /* 0..255, NODATA ou EOF */
    /* Tempo */
    uint32_t (*millis)(void *ctx);
    void     (*sleep_ms)(void *ctx, uint32_t ms);
    /* Syscalls fora dos grupos do núcleo (fd, GPIO, usuário). Usa
     * ksvm_arg/ksvm_ret (syscall.h) e vm->hal->ctx. */
    int  (*syscall)(struct ksvm *vm, uint32_t num);
    /* Término por HALT ou syscall EXIT */
    void (*on_halt)(void *ctx, uint32_t code);
} ksvm_hal;

#endif /* KSVM_HAL_H */
