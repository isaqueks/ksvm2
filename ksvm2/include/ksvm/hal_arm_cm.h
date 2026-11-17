/*
 * hal_arm_cm.h — HAL para Cortex-M (ARMv6-M/ARMv7-M), sem CMSIS.
 *
 *   putc/getc  UART CMSDK APB (MPS2/ARM IP), KSVM_ARM_CM_UART_BASE
 *   millis     SysTick a 1 ms (KSVM_ARM_CM_CPU_HZ); chame
 *              ksvm_hal_arm_cm_systick() do vetor SysTick
 *   sleep_ms   WFI até o próximo tick, repetido
 *   syscall    grupo 4: PIN_MODE (0 entrada, 1 saída; 2 = pull-up não
 *              existe no GPIO CMSDK: INVAL), PIN_WRITE, PIN_READ nos pinos
 *              0..63 dos GPIO CMSDK AHB em KSVM_ARM_CM_GPIO_BASE (16 pinos
 *              por bloco, blocos a cada 0x1000). ANALOG_READ e PWM_WRITE:
 *              não suportados (NOSYS).
 *   saída      ksvm_hal_arm_cm_exit: SYS_EXIT por semihosting se
 *              KSVM_ARM_CM_SEMIHOSTING (QEMU -semihosting), senão WFI para
 *              sempre.
 *
 * Os padrões de endereço e clock são os do QEMU mps2-an385. O arquivo .c só
 * tem conteúdo quando compilado para um Cortex-M (__ARM_ARCH_PROFILE 'M').
 */
#ifndef KSVM_HAL_ARM_CM_H
#define KSVM_HAL_ARM_CM_H

#include "hal.h"

#ifndef KSVM_ARM_CM_CPU_HZ
#define KSVM_ARM_CM_CPU_HZ 25000000UL
#endif
#ifndef KSVM_ARM_CM_UART_BASE
#define KSVM_ARM_CM_UART_BASE 0x40004000UL
#endif
#ifndef KSVM_ARM_CM_UART_BAUD
#define KSVM_ARM_CM_UART_BAUD 115200UL
#endif
#ifndef KSVM_ARM_CM_GPIO_BASE
#define KSVM_ARM_CM_GPIO_BASE 0x40010000UL
#endif
#ifndef KSVM_ARM_CM_SEMIHOSTING
#define KSVM_ARM_CM_SEMIHOSTING 0
#endif

typedef struct ksvm_hal_arm_cm {
    bool     halted;
    uint32_t halt_code;
} ksvm_hal_arm_cm;

/* Configura UART e SysTick e preenche hal (hal->ctx = ch). */
void ksvm_hal_arm_cm_init(ksvm_hal_arm_cm *ch, ksvm_hal *hal);

/* Corpo do handler SysTick (a tabela de vetores da porta aponta para cá). */
void ksvm_hal_arm_cm_systick(void);

/* Milissegundos desde ksvm_hal_arm_cm_init (wrap em 2^32). */
uint32_t ksvm_hal_arm_cm_millis(void);

/* Saída direta na UART (mensagens do host). Bloqueia até ter espaço. */
void ksvm_hal_arm_cm_putc(uint8_t c);

/* Encerra: semihosting SYS_EXIT (código 0 = sucesso para o QEMU, demais =
 * falha) ou, sem semihosting, dorme para sempre. Não retorna. */
void ksvm_hal_arm_cm_exit(uint32_t code);

#endif /* KSVM_HAL_ARM_CM_H */
