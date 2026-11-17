/*
 * hal_avr.h — HAL para AVR (ATmega328P e afins), sem o core do Arduino.
 *
 *   putc/getc  UART0 por registradores, 8N1, KSVM_HAL_AVR_BAUD (U2X)
 *   millis     Timer0 em CTC, interrupção a cada 1 ms (F_CPU / 64 / 1000)
 *   sleep_ms   espera ativa em modo IDLE (acorda a cada tick do Timer0)
 *   syscall    grupo 4 (doc 06 §3): PIN_MODE, PIN_WRITE, PIN_READ,
 *              ANALOG_READ e, com KSVM_HAL_AVR_PWM, PWM_WRITE
 *
 * Pinos na numeração do Arduino Uno: 0..7 = PD0..PD7, 8..13 = PB0..PB5,
 * 14..19 = PC0..PC5 (A0..A5). 0 e 1 são RX/TX da UART0: reconfigurá-los
 * desliga o console. ANALOG_READ: canal 0..7, referência AVcc, 0..1023.
 * PWM_WRITE (0..255): pinos 3 e 11 (Timer2) e 9 e 10 (Timer1, 8 bits);
 * o Timer0 fica reservado para millis.
 *
 * O Timer0 e a UART0 são recursos globais: uma HAL por programa. O arquivo
 * .c só tem conteúdo quando compilado com __AVR__.
 */
#ifndef KSVM_HAL_AVR_H
#define KSVM_HAL_AVR_H

#include "bus.h"
#include "hal.h"

#ifndef KSVM_HAL_AVR_BAUD
#define KSVM_HAL_AVR_BAUD 115200UL
#endif

#ifndef KSVM_HAL_AVR_PWM
#define KSVM_HAL_AVR_PWM 1
#endif

typedef struct ksvm_hal_avr {
    bool     halted;
    uint32_t halt_code;
} ksvm_hal_avr;

/* Configura UART0, Timer0 e o ADC, liga as interrupções e preenche hal
 * (hal->ctx = ah). */
void ksvm_hal_avr_init(ksvm_hal_avr *ah, ksvm_hal *hal);

/* read8 para ksvm_load_image_ex: lê a imagem (e o código) da flash. Só os
 * primeiros 64 KB de flash (pgm_read_byte "near"). */
uint8_t ksvm_hal_avr_flash_read8(ksvm_region *r, uint32_t off);

/* Milissegundos desde ksvm_hal_avr_init (wrap em 2^32). */
uint32_t ksvm_hal_avr_millis(void);

/* Saída direta na UART0 (mensagens do host). Bloqueia até ter espaço. */
void ksvm_hal_avr_putc(uint8_t c);

/* Espera a UART esvaziar, desliga as interrupções e dorme em power-down
 * para sempre (o simavr encerra a simulação nesse ponto). */
void ksvm_hal_avr_stop(void);

#endif /* KSVM_HAL_AVR_H */
