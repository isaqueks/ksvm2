/*
 * syscall.h — números de syscall padrão (doc 06 §3), códigos de erro do
 * guest e helpers para o host ler argumentos e acessar a memória do guest.
 *
 * Convenção: número = imm14 de SYSCALL lido como 14 bits SEM sinal
 * (0..0x3FFF); argumentos em r1..r6; retorno em r1 (-1 em erro) e código de
 * erro em r2 (KSVM_ERR_*, 0 se ok). Os grupos 0, 1, 2 e 5 são tratados aqui;
 * qualquer outro número (inclusive MICROS) é encaminhado a hal->syscall.
 */
#ifndef KSVM_SYSCALL_H
#define KSVM_SYSCALL_H

#include "vm.h"

enum ksvm_syscall_num {
    /* Grupo 0 — núcleo */
    KSVM_SYSCALL_EXIT      = 0x00,
    KSVM_SYSCALL_CAPS      = 0x01,
    KSVM_SYSCALL_VERSION   = 0x02,
    KSVM_SYSCALL_YIELD     = 0x03,
    KSVM_SYSCALL_ABORT     = 0x04,
    /* Grupo 1 — console */
    KSVM_SYSCALL_PUTC      = 0x10,
    KSVM_SYSCALL_GETC      = 0x11,
    KSVM_SYSCALL_PUTS      = 0x12,
    KSVM_SYSCALL_GETS      = 0x13,
    /* Grupo 2 — tempo */
    KSVM_SYSCALL_MILLIS    = 0x20,
    KSVM_SYSCALL_SLEEP_MS  = 0x21,
    KSVM_SYSCALL_MICROS    = 0x22,   /* opcional: só via hal->syscall */
    /* Grupo 3 — arquivos (sempre encaminhado ao HAL) */
    KSVM_SYSCALL_OPEN      = 0x30,
    KSVM_SYSCALL_CLOSE     = 0x31,
    KSVM_SYSCALL_READ      = 0x32,
    KSVM_SYSCALL_WRITE     = 0x33,
    KSVM_SYSCALL_SEEK      = 0x34,
    /* Grupo 4 — GPIO (sempre encaminhado ao HAL) */
    KSVM_SYSCALL_PIN_MODE    = 0x40,
    KSVM_SYSCALL_PIN_WRITE   = 0x41,
    KSVM_SYSCALL_PIN_READ    = 0x42,
    KSVM_SYSCALL_ANALOG_READ = 0x43,
    KSVM_SYSCALL_PWM_WRITE   = 0x44,
    /* Grupo 5 — memória */
    KSVM_SYSCALL_MEM_INFO  = 0x50,
    KSVM_SYSCALL_RANDOM    = 0x51,
    /* 0x100..0x3FFF: definidos pelo host */
    KSVM_SYSCALL_USER_BASE = 0x100
};

/* Códigos de erro devolvidos ao guest em r2. */
enum ksvm_guest_err {
    KSVM_ERR_OK    = 0,
    KSVM_ERR_NOSYS = 1,   /* syscall desconhecida ou serviço ausente no HAL */
    KSVM_ERR_FAULT = 2,   /* ponteiro/faixa do guest inválida */
    KSVM_ERR_INVAL = 3,   /* argumento inválido */
    KSVM_ERR_IO    = 4,   /* falha de E/S no host */
    KSVM_ERR_AGAIN = 5,   /* nada disponível agora (GETC sem dado) */
    KSVM_ERR_EOF   = 6    /* fim da entrada */
};

#define KSVM_SYS_RET_ERROR UINT32_C(0xFFFFFFFF)   /* -1 em r1 */

/* CAPS (doc 06 §3): r1 = número de syscall (0..0x3FFF) -> 1/0. Acima dessa
 * faixa, que nunca é número de syscall, ficam consultas de capacidade:
 * CAPS(KSVM_CAPS_ISA) devolve em r1 a máscara das extensões opcionais da
 * ISA presentes nesta VM. */
#define KSVM_CAPS_ISA   UINT32_C(0x4000)
#define KSVM_CAPS_ISA_M UINT32_C(0x1)   /* MUL..REMU (KSVM_ENABLE_MULDIV) */
#define KSVM_CAPS_ISA_B UINT32_C(0x2)   /* CLZ/CTZ/POPCNT (KSVM_ENABLE_BITOPS) */
#define KSVM_CAPS_ISA_F UINT32_C(0x4)   /* extensão F (KSVM_ENABLE_FLOAT) */

/* Máscara desta compilação. */
#define KSVM_CAPS_ISA_BITS                                  \
    ((KSVM_ENABLE_MULDIV ? KSVM_CAPS_ISA_M : UINT32_C(0)) | \
     (KSVM_ENABLE_BITOPS ? KSVM_CAPS_ISA_B : UINT32_C(0)) | \
     (KSVM_ENABLE_FLOAT ? KSVM_CAPS_ISA_F : UINT32_C(0)))

/* Executa a syscall "num" (chamado por exec.c em SYSCALL). Pode mudar
 * vm->status (HALTED, YIELD, FAULT). */
void ksvm_syscall(ksvm *vm, uint32_t num);

/* r[n] para n = 1..6; 0 fora dessa faixa. */
uint32_t ksvm_arg(const ksvm *vm, unsigned n);
/* r1 = value, r2 = err. */
void ksvm_ret(ksvm *vm, uint32_t value, uint32_t err);

/* Copiam len bytes validando a faixa inteira antes (leitura/escrita numa só
 * região). Devolvem KSVM_OK (inclusive com len 0, sem olhar os ponteiros),
 * KSVM_E_ARG (buffer do host NULL com len > 0) ou KSVM_E_RANGE (faixa do
 * guest fora de região ou sem permissão); em erro nada é copiado. */
int ksvm_copy_from_guest(ksvm *vm, void *dst, uint32_t gaddr, uint32_t len);
int ksvm_copy_to_guest(ksvm *vm, uint32_t gaddr, const void *src, uint32_t len);

/* Ponteiro direto para [gaddr, gaddr + len) se a faixa estiver numa região
 * RAM de acesso direto com "perm"; NULL caso contrário (inclusive len 0). */
uint8_t *ksvm_guest_ptr(ksvm *vm, uint32_t gaddr, uint32_t len, uint8_t perm);

#endif /* KSVM_SYSCALL_H */
