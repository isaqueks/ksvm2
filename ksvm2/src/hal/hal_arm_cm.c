/*
 * hal_arm_cm.c — HAL Cortex-M por registradores (ver hal_arm_cm.h).
 *
 * Periféricos acessados por endereço fixo (volatile uint32_t); o único
 * assembly é WFI e o BKPT 0xAB do semihosting. Fora de um Cortex-M o
 * arquivo fica vazio (só um typedef, porque C99 não aceita unidade de
 * tradução vazia).
 */
#if defined(__arm__) && defined(__ARM_ARCH_PROFILE) && __ARM_ARCH_PROFILE == 'M'

#include "ksvm/hal_arm_cm.h"
#include "ksvm/syscall.h"

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* UART CMSDK APB */
#define UART_DATA    REG32(KSVM_ARM_CM_UART_BASE + 0x000u)
#define UART_STATE   REG32(KSVM_ARM_CM_UART_BASE + 0x004u)
#define UART_CTRL    REG32(KSVM_ARM_CM_UART_BASE + 0x008u)
#define UART_BAUDDIV REG32(KSVM_ARM_CM_UART_BASE + 0x010u)
#define UART_STATE_TXFULL 0x1u
#define UART_STATE_RXFULL 0x2u
#define UART_CTRL_TXEN    0x1u
#define UART_CTRL_RXEN    0x2u

/* SysTick (núcleo ARMv6-M/ARMv7-M) */
#define SYST_CSR REG32(0xE000E010u)
#define SYST_RVR REG32(0xE000E014u)
#define SYST_CVR REG32(0xE000E018u)
#define SYST_CSR_ENABLE    0x1u
#define SYST_CSR_TICKINT   0x2u
#define SYST_CSR_CLKSOURCE 0x4u

/* GPIO CMSDK AHB: um bloco de 16 pinos a cada 0x1000 */
#define GPIO_DATA     0x000u
#define GPIO_DATAOUT  0x004u
#define GPIO_OUTENSET 0x010u
#define GPIO_OUTENCLR 0x014u
#define GPIO_PINS     64u

#define SYSTICK_RELOAD ((KSVM_ARM_CM_CPU_HZ / 1000UL) - 1UL)
#if SYSTICK_RELOAD > 0xFFFFFFUL
#error "KSVM_ARM_CM_CPU_HZ alto demais para o SysTick de 24 bits a 1 ms"
#endif

static volatile uint32_t ms_ticks;   /* escrito só pelo SysTick */

void ksvm_hal_arm_cm_systick(void)
{
    ms_ticks++;
}

uint32_t ksvm_hal_arm_cm_millis(void)
{
    return ms_ticks;                 /* LDR de 32 bits: atômico */
}

/* ---- UART ------------------------------------------------------------- */

void ksvm_hal_arm_cm_putc(uint8_t c)
{
    while ((UART_STATE & UART_STATE_TXFULL) != 0u) {
    }
    UART_DATA = c;
}

static int cm_putc(void *ctx, uint8_t c)
{
    (void)ctx;
    ksvm_hal_arm_cm_putc(c);
    return 0;
}

static int cm_getc(void *ctx)
{
    (void)ctx;
    if ((UART_STATE & UART_STATE_RXFULL) == 0u) {
        return KSVM_HAL_NODATA;
    }
    return (int)(UART_DATA & 0xFFu);
}

/* ---- Tempo ------------------------------------------------------------ */

static uint32_t cm_millis(void *ctx)
{
    (void)ctx;
    return ms_ticks;
}

static void cm_sleep_ms(void *ctx, uint32_t ms)
{
    uint32_t start = ms_ticks;

    (void)ctx;
    while (ms_ticks - start < ms) {
        __asm__ volatile ("wfi");    /* acorda no próximo SysTick */
    }
}

static void cm_on_halt(void *ctx, uint32_t code)
{
    ksvm_hal_arm_cm *ch = (ksvm_hal_arm_cm *)ctx;

    ch->halted = true;
    ch->halt_code = code;
}

/* ---- Grupo 4: GPIO ---------------------------------------------------- */

static uintptr_t gpio_block(uint32_t pin)
{
    return (uintptr_t)(KSVM_ARM_CM_GPIO_BASE + (pin / 16u) * 0x1000u);
}

static int cm_syscall(ksvm *vm, uint32_t num)
{
    uint32_t pin = ksvm_arg(vm, 1u), arg = ksvm_arg(vm, 2u), value = 0u;
    uintptr_t g = gpio_block(pin);
    uint32_t bit = (uint32_t)1u << (pin % 16u);

    if (num < KSVM_SYSCALL_PIN_MODE || num > KSVM_SYSCALL_PIN_READ) {
        return KSVM_SYS_UNKNOWN;          /* ANALOG_READ/PWM_WRITE: NOSYS */
    }
    if (pin >= GPIO_PINS || (num == KSVM_SYSCALL_PIN_MODE && arg > 1u)) {
        ksvm_ret(vm, KSVM_SYS_RET_ERROR, KSVM_ERR_INVAL);
        return KSVM_SYS_OK;
    }
    if (num == KSVM_SYSCALL_PIN_MODE) {
        REG32(g + (arg == 1u ? GPIO_OUTENSET : GPIO_OUTENCLR)) = bit;
    } else if (num == KSVM_SYSCALL_PIN_WRITE) {
        uint32_t out = REG32(g + GPIO_DATAOUT);
        REG32(g + GPIO_DATAOUT) = (arg != 0u) ? (out | bit) : (out & ~bit);
    } else {
        value = ((REG32(g + GPIO_DATA) & bit) != 0u) ? 1u : 0u;
    }
    ksvm_ret(vm, value, KSVM_ERR_OK);
    return KSVM_SYS_OK;
}

/* ---- Inicialização e saída -------------------------------------------- */

void ksvm_hal_arm_cm_init(ksvm_hal_arm_cm *ch, ksvm_hal *hal)
{
    ch->halted = false;
    ch->halt_code = 0u;

    UART_BAUDDIV = KSVM_ARM_CM_CPU_HZ / KSVM_ARM_CM_UART_BAUD;
    UART_CTRL = UART_CTRL_TXEN | UART_CTRL_RXEN;

    SYST_RVR = SYSTICK_RELOAD;
    SYST_CVR = 0u;
    SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;

    hal->ctx = ch;
    hal->putc = cm_putc;
    hal->getc = cm_getc;
    hal->millis = cm_millis;
    hal->sleep_ms = cm_sleep_ms;
    hal->syscall = cm_syscall;
    hal->on_halt = cm_on_halt;
}

#if KSVM_ARM_CM_SEMIHOSTING
#define SEMI_SYS_EXIT                  0x18u
#define SEMI_ADP_STOPPED_APP_EXIT      0x20026u
#define SEMI_ADP_STOPPED_RUNTIME_ERROR 0x20023u

static void semihost(uint32_t op, uint32_t arg)
{
    register uint32_t r0 __asm__("r0") = op;
    register uint32_t r1 __asm__("r1") = arg;

    __asm__ volatile ("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
}
#endif

void ksvm_hal_arm_cm_exit(uint32_t code)
{
    while ((UART_STATE & UART_STATE_TXFULL) != 0u) {   /* UART esvaziou */
    }
#if KSVM_ARM_CM_SEMIHOSTING
    /* ARM de 32 bits: o QEMU só distingue "ApplicationExit" (status 0) de
     * qualquer outro motivo (status 1). */
    semihost(SEMI_SYS_EXIT, (code == 0u) ? SEMI_ADP_STOPPED_APP_EXIT
                                         : SEMI_ADP_STOPPED_RUNTIME_ERROR);
#else
    (void)code;
#endif
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

#else
/* C99 6.9: uma unidade de tradução precisa de ao menos uma declaração. */
typedef int ksvm_hal_arm_cm_not_cortex_m;
#endif
