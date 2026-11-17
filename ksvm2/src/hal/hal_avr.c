/*
 * hal_avr.c — HAL do ATmega328P por registradores (ver hal_avr.h).
 *
 * Único arquivo da árvore que usa <avr/io.h>, <avr/interrupt.h> e
 * <avr/pgmspace.h> diretamente; os atributos de ISR vêm das macros da
 * avr-libc. Fora do AVR o arquivo fica vazio (só um typedef, porque C99
 * não aceita unidade de tradução vazia).
 */
#ifdef __AVR__

#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/sleep.h>

#include "ksvm/hal_avr.h"
#include "ksvm/syscall.h"

#ifndef F_CPU
#error "defina F_CPU (ex.: -DF_CPU=16000000UL)"
#endif

/* Timer0 em CTC com prescaler 64: OCR0A + 1 ticks por milissegundo. */
#define T0_TOP ((F_CPU / 64UL / 1000UL) - 1UL)
#if T0_TOP < 1UL || T0_TOP > 255UL
#error "F_CPU fora da faixa do Timer0 (prescaler 64, 1 ms)"
#endif

/* UART com U2X0: UBRR = F_CPU / (8 * baud) - 1, arredondado. */
#define UART_UBRR (((F_CPU + 4UL * KSVM_HAL_AVR_BAUD) / (8UL * KSVM_HAL_AVR_BAUD)) - 1UL)

static volatile uint32_t ms_ticks;   /* escrito só pela ISR */
static volatile bool     uart_used;  /* algo já foi transmitido */

ISR(TIMER0_COMPA_vect)
{
    ms_ticks++;
}

uint32_t ksvm_hal_avr_millis(void)
{
    uint8_t sreg = SREG;
    uint32_t v;

    cli();                  /* leitura de 32 bits não é atômica no AVR */
    v = ms_ticks;
    SREG = sreg;
    return v;
}

uint8_t ksvm_hal_avr_flash_read8(ksvm_region *r, uint32_t off)
{
    const uint8_t *p = (const uint8_t *)r->backing + (uint16_t)off;
    return pgm_read_byte(p);
}

/* ---- UART0 ------------------------------------------------------------ */

void ksvm_hal_avr_putc(uint8_t c)
{
    while ((UCSR0A & (1u << UDRE0)) == 0u) {
    }
    UCSR0A = (uint8_t)(UCSR0A | (1u << TXC0));   /* escrever 1 limpa TXC0 */
    UDR0 = c;
    uart_used = true;
}

static int avr_putc(void *ctx, uint8_t c)
{
    (void)ctx;
    ksvm_hal_avr_putc(c);
    return 0;
}

static int avr_getc(void *ctx)
{
    (void)ctx;
    if ((UCSR0A & (1u << RXC0)) == 0u) {
        return KSVM_HAL_NODATA;
    }
    return UDR0;
}

/* ---- Tempo ------------------------------------------------------------ */

static uint32_t avr_millis(void *ctx)
{
    (void)ctx;
    return ksvm_hal_avr_millis();
}

/* IDLE mantém o Timer0 rodando; a ISR acorda a CPU a cada milissegundo. */
static void avr_sleep_ms(void *ctx, uint32_t ms)
{
    uint32_t start = ksvm_hal_avr_millis();

    (void)ctx;
    SMCR = (uint8_t)SLEEP_MODE_IDLE;     /* set_sleep_mode() fere -Wconversion */
    while (ksvm_hal_avr_millis() - start < ms) {
        sleep_mode();
    }
}

static void avr_on_halt(void *ctx, uint32_t code)
{
    ksvm_hal_avr *ah = (ksvm_hal_avr *)ctx;

    ah->halted = true;
    ah->halt_code = code;
}

/* ---- Grupo 4: GPIO ---------------------------------------------------- */

/* No ATmega48/88/168/328, PINx, DDRx e PORTx são consecutivos e as portas
 * B, C e D vêm em sequência (PINB = 0x23, PINC = 0x26, PIND = 0x29): o
 * bloco de um pino é &PINB + 3 * porta, sem tabela. */
#define REG_PIN  0u
#define REG_DDR  1u
#define REG_PORT 2u

/* Numeração do Arduino Uno (hal_avr.h); NULL se o pino não existe. */
static volatile uint8_t *pin_regs(uint32_t pin, uint8_t *mask)
{
    uint8_t n, port;                    /* port: 0 = B, 1 = C, 2 = D */

    if (pin >= 20u) {
        return NULL;
    }
    n = (uint8_t)pin;                   /* daqui em diante, aritmética de 8 bits */
    if (n < 8u) {
        port = 2u;
    } else if (n < 14u) {
        port = 0u;
        n = (uint8_t)(n - 8u);
    } else {
        port = 1u;
        n = (uint8_t)(n - 14u);
    }
    *mask = (uint8_t)(1u << n);
    return &PINB + (uint8_t)(3u * port);
}

static void set_bits(volatile uint8_t *reg, uint8_t mask, bool on)
{
    *reg = on ? (uint8_t)(*reg | mask) : (uint8_t)(*reg & (uint8_t)~mask);
}

static uint16_t adc_read(uint8_t channel)
{
    ADMUX = (uint8_t)((1u << REFS0) | channel);          /* referência AVcc */
    ADCSRA = (uint8_t)(ADCSRA | (1u << ADSC));
    while ((ADCSRA & (1u << ADSC)) != 0u) {
    }
    return ADC;                                           /* lê ADCL e ADCH */
}

#if KSVM_HAL_AVR_PWM
/* Fast PWM 8 bits, prescaler 64, saída não invertida no canal do pino. */
static bool pwm_start(uint32_t n, uint8_t duty)
{
    if (n == 3u || n == 11u) {                  /* OC2B = PD3, OC2A = PB3 */
        TCCR2A = (uint8_t)(TCCR2A | (1u << WGM21) | (1u << WGM20) |
                           ((n == 3u) ? (1u << COM2B1) : (1u << COM2A1)));
        TCCR2B = (uint8_t)(1u << CS22);
        if (n == 3u) {
            OCR2B = duty;
        } else {
            OCR2A = duty;
        }
    } else if (n == 9u || n == 10u) {           /* OC1A = PB1, OC1B = PB2 */
        TCCR1A = (uint8_t)(TCCR1A | (1u << WGM10) |
                           ((n == 9u) ? (1u << COM1A1) : (1u << COM1B1)));
        TCCR1B = (uint8_t)((1u << WGM12) | (1u << CS11) | (1u << CS10));
        if (n == 9u) {
            OCR1A = duty;
        } else {
            OCR1B = duty;
        }
    } else {
        return false;
    }
    return true;
}
#define GPIO_LAST KSVM_SYSCALL_PWM_WRITE
#else
#define GPIO_LAST KSVM_SYSCALL_ANALOG_READ
#endif

/* Devolve false se o argumento é inválido (o chamador responde INVAL).
 * num já está em PIN_MODE..GPIO_LAST. */
static bool gpio(ksvm *vm, uint8_t num, uint32_t a1, uint32_t a2)
{
    uint8_t mask = 0u;
    volatile uint8_t *r = pin_regs(a1, &mask);
    /* a2 saturado em 8 bits: preserva "> 2" e "!= 0" com comparações de
     * 8 bits (cada comparação de 32 bits custa ~10 bytes no AVR). */
    uint8_t arg = (a2 > 0xFFu) ? 0xFFu : (uint8_t)a2;
    uint32_t value = 0u;

    if (num == KSVM_SYSCALL_ANALOG_READ) {
        if (a1 > 7u) {
            return false;
        }
        value = adc_read((uint8_t)a1);
    } else if (r == NULL) {
        return false;
    } else if (num == KSVM_SYSCALL_PIN_MODE) {  /* 0 entrada, 1 saída, 2 pull-up */
        if (arg > 2u) {
            return false;
        }
        set_bits(r + REG_DDR, mask, arg == 1u);
        if (arg != 1u) {
            set_bits(r + REG_PORT, mask, arg == 2u);
        }
    } else if (num == KSVM_SYSCALL_PIN_WRITE) {
        set_bits(r + REG_PORT, mask, arg != 0u);
    } else if (num == KSVM_SYSCALL_PIN_READ) {
        value = ((r[REG_PIN] & mask) != 0u) ? 1u : 0u;
    } else {
#if KSVM_HAL_AVR_PWM
        if (a2 > 0xFFu || !pwm_start(a1, arg)) {
            return false;
        }
        set_bits(r + REG_DDR, mask, true);
#endif
    }
    ksvm_ret(vm, value, KSVM_ERR_OK);
    return true;
}

static int avr_syscall(ksvm *vm, uint32_t num)
{
    if (num < KSVM_SYSCALL_PIN_MODE || num > GPIO_LAST) {
        return KSVM_SYS_UNKNOWN;
    }
    if (!gpio(vm, (uint8_t)num, ksvm_arg(vm, 1u), ksvm_arg(vm, 2u))) {
        ksvm_ret(vm, KSVM_SYS_RET_ERROR, KSVM_ERR_INVAL);
    }
    return KSVM_SYS_OK;
}

/* ---- Inicialização e parada ------------------------------------------- */

void ksvm_hal_avr_init(ksvm_hal_avr *ah, ksvm_hal *hal)
{
    ah->halted = false;
    ah->halt_code = 0u;

    UBRR0 = (uint16_t)UART_UBRR;
    UCSR0A = (uint8_t)(1u << U2X0);
    UCSR0C = (uint8_t)((1u << UCSZ01) | (1u << UCSZ00));          /* 8N1 */
    UCSR0B = (uint8_t)((1u << RXEN0) | (1u << TXEN0));

    TCCR0A = (uint8_t)(1u << WGM01);                               /* CTC */
    OCR0A = (uint8_t)T0_TOP;
    TCCR0B = (uint8_t)((1u << CS01) | (1u << CS00));               /* /64 */
    TIMSK0 = (uint8_t)(1u << OCIE0A);

    ADCSRA = (uint8_t)((1u << ADEN) | (1u << ADPS2) | (1u << ADPS1) | (1u << ADPS0));

    hal->ctx = ah;
    hal->putc = avr_putc;
    hal->getc = avr_getc;
    hal->millis = avr_millis;
    hal->sleep_ms = avr_sleep_ms;
    hal->syscall = avr_syscall;
    hal->on_halt = avr_on_halt;
    sei();
}

void ksvm_hal_avr_stop(void)
{
    if (uart_used) {
        while ((UCSR0A & (1u << TXC0)) == 0u) {    /* último byte saiu */
        }
    }
    cli();
    SMCR = (uint8_t)SLEEP_MODE_PWR_DOWN;
    sleep_enable();
    for (;;) {
        sleep_cpu();
    }
}

#else
/* C99 6.9: uma unidade de tradução precisa de ao menos uma declaração. */
typedef int ksvm_hal_avr_not_avr;
#endif /* __AVR__ */
