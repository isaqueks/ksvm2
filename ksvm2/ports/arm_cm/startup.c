/*
 * startup.c — inicialização mínima de Cortex-M para a porta (sem CMSIS).
 *
 * Tabela de vetores em .isr_vector (endereço 0, ver linker.ld), reset que
 * copia .data da flash, zera .bss, liga as armadilhas de acesso desalinhado
 * e divisão por zero (ARMv7-M) e chama main. Qualquer exceção de falha
 * imprime "KSVM HARDFAULT" na UART e encerra com status de erro.
 *
 * Atenção: o QEMU 8.2 NÃO gera falha em LDR/STR desalinhado nem no
 * mps2-an385 com CCR.UNALIGN_TRP nem no microbit (Cortex-M0) — verificado
 * com um LDR desalinhado proposital. Num Cortex-M0 real isso seria hard
 * fault. A garantia contra acesso desalinhado no núcleo vem de bytes.h e do
 * UBSan (-fsanitize=undefined inclui alignment) em make test-asan.
 */
#include <stdint.h>

#include "ksvm/hal_arm_cm.h"

typedef void (*vector_fn)(void);

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

int main(void);
void Reset_Handler(void);
void Fault_Handler(void);

/* SCB->CCR: UNALIGN_TRP (bit 3) e DIV_0_TRP (bit 4). Só ARMv7-M tem esses
 * bits; no ARMv6-M (M0) o acesso desalinhado já é sempre falha. */
#define SCB_CCR (*(volatile uint32_t *)0xE000ED14u)
#define CCR_UNALIGN_TRP 0x8u
#define CCR_DIV_0_TRP   0x10u

__attribute__((section(".isr_vector"), used))
static const vector_fn vectors[16] = {
    (vector_fn)(uintptr_t)&_estack,    /* SP inicial */
    Reset_Handler,
    Fault_Handler,                     /* NMI */
    Fault_Handler,                     /* HardFault */
    Fault_Handler,                     /* MemManage (v7-M) */
    Fault_Handler,                     /* BusFault (v7-M) */
    Fault_Handler,                     /* UsageFault (v7-M) */
    0, 0, 0, 0,
    Fault_Handler,                     /* SVCall */
    Fault_Handler,                     /* DebugMon */
    0,
    Fault_Handler,                     /* PendSV */
    ksvm_hal_arm_cm_systick            /* SysTick */
};

static const char hardfault_msg[] = "\nKSVM HARDFAULT\n";

void Fault_Handler(void)
{
    const char *s;

    for (s = hardfault_msg; *s != '\0'; s++) {
        ksvm_hal_arm_cm_putc((uint8_t)*s);
    }
    ksvm_hal_arm_cm_exit(0xFFu);
}

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst;

    for (dst = &_sdata; dst < &_edata; dst++) {
        *dst = *src++;
    }
    for (dst = &_sbss; dst < &_ebss; dst++) {
        *dst = 0u;
    }
    /* Em ARMv7-M real (M3/M4) passa a falhar como o M0. Num M0 esses bits
     * são fixos (UNALIGN_TRP já vale 1) e a escrita é inócua. (O QEMU
     * ignora UNALIGN_TRP: ver o comentário do topo.) */
    SCB_CCR |= CCR_UNALIGN_TRP | CCR_DIV_0_TRP;
    (void)main();
    ksvm_hal_arm_cm_exit(0xFEu);
}
