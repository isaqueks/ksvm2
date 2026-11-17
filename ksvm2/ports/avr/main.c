/*
 * main.c — porta ATmega328P: roda a imagem embutida em program_image.h.
 *
 * A imagem .ksx fica inteira na flash (PROGMEM): o loader lê cabeçalho,
 * checksum e .data por ksvm_hal_avr_flash_read8, e a região de código é
 * uma ROM com o mesmo read8 (nada do código do guest vai para a SRAM).
 * A SRAM guarda só struct ksvm, a RAM do guest (ram[]) e a pilha do AVR.
 *
 * Laço cooperativo: fatias de KSVM_AVR_SLICE instruções; entre fatias o
 * firmware poderia atender outras tarefas. Ao terminar imprime
 * "KSVM halt <código>" ou "KSVM FAULT <n> pc=<pc> addr=<addr>" (hex) e
 * dorme (o simavr encerra a simulação nesse ponto).
 */
#include <avr/pgmspace.h>

#include "ksvm/hal_avr.h"
#include "ksvm/loader.h"
#include "ksvm/vm.h"

#define KSVM_IMAGE_ATTR PROGMEM
#include "program_image.h"

#ifndef KSVM_AVR_RAM_SIZE
#define KSVM_AVR_RAM_SIZE 1024u
#endif

#ifndef KSVM_AVR_SLICE
#define KSVM_AVR_SLICE 500u
#endif

static ksvm         vm;
static ksvm_hal     hal;
static ksvm_hal_avr hal_ctx;
static uint8_t      ram[KSVM_AVR_RAM_SIZE];

static const char msg_halt[]  PROGMEM = "KSVM halt ";
static const char msg_fault[] PROGMEM = "KSVM FAULT ";
static const char msg_pc[]    PROGMEM = " pc=";
static const char msg_addr[]  PROGMEM = " addr=";
static const char msg_load[]  PROGMEM = "KSVM load error ";

static void put_P(const char *s)
{
    char c;

    while ((c = (char)pgm_read_byte(s)) != '\0') {
        ksvm_hal_avr_putc((uint8_t)c);
        s++;
    }
}

/* "digits" dígitos hexadecimais maiúsculos. */
static void put_hex(uint32_t v, uint8_t digits)
{
    while (digits > 0u) {
        uint8_t d;
        digits--;
        d = (uint8_t)((v >> (4u * digits)) & 0xFu);
        ksvm_hal_avr_putc((uint8_t)(d < 10u ? 0x30u + d : 0x37u + d));  /* '0', 'A'-10 */
    }
}

static void report(void)
{
    if (vm.status == KSVM_FAULT) {
        put_P(msg_fault);
        put_hex((uint32_t)vm.fault, 2u);
        put_P(msg_pc);
        put_hex(vm.cpu.pc, 8u);
        put_P(msg_addr);
        put_hex(vm.fault_addr, 8u);
    } else {
        put_P(msg_halt);
        put_hex(vm.exit_code, 8u);
    }
    ksvm_hal_avr_putc('\n');
}

int main(void)
{
    int err;
    ksvm_status st;

    ksvm_hal_avr_init(&hal_ctx, &hal);
    ksvm_init(&vm, &hal);
    err = ksvm_load_image_ex(&vm, ksvm_program_image, KSVM_PROGRAM_IMAGE_SIZE,
                             ram, (uint32_t)sizeof ram, ksvm_hal_avr_flash_read8);
    if (err != KSVM_OK) {
        put_P(msg_load);
        put_hex((uint32_t)-err, 2u);
        ksvm_hal_avr_putc('\n');
        ksvm_hal_avr_stop();
    }
    do {
        st = ksvm_run(&vm, KSVM_AVR_SLICE);
        /* Aqui o firmware atenderia suas próprias tarefas entre fatias. */
    } while (st == KSVM_RUNNING || st == KSVM_YIELD);
    report();
    ksvm_hal_avr_stop();
    return 0;
}
