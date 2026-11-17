/*
 * main.c — porta Cortex-M: roda a imagem embutida em program_image.h.
 *
 * A flash é endereçável, então a imagem é um const uint8_t[] comum e o
 * loader a lê diretamente (read8 = NULL; doc 05 §3.3): o código do guest
 * roda da flash, só .data/.bss/pilha do guest vão para ram[].
 *
 * Mesmo protocolo de saída da porta AVR: "KSVM halt <código>" ou
 * "KSVM FAULT <n> pc=<pc> addr=<addr>" (hex) e encerra (semihosting no
 * QEMU: status 0 só para HALT 0).
 */
#include "ksvm/hal_arm_cm.h"
#include "ksvm/loader.h"
#include "ksvm/vm.h"

#include "program_image.h"

#ifndef KSVM_ARM_RAM_SIZE
#define KSVM_ARM_RAM_SIZE 4096u
#endif

#ifndef KSVM_ARM_SLICE
#define KSVM_ARM_SLICE 500u
#endif

static ksvm            vm;
static ksvm_hal        hal;
static ksvm_hal_arm_cm hal_ctx;
static uint8_t         ram[KSVM_ARM_RAM_SIZE];

static void put_s(const char *s)
{
    while (*s != '\0') {
        ksvm_hal_arm_cm_putc((uint8_t)*s);
        s++;
    }
}

static void put_hex(uint32_t v, unsigned digits)
{
    while (digits > 0u) {
        uint32_t d;
        digits--;
        d = (v >> (4u * digits)) & 0xFu;
        ksvm_hal_arm_cm_putc((uint8_t)(d < 10u ? 0x30u + d : 0x37u + d));  /* '0', 'A'-10 */
    }
}

int main(void)
{
    int err;
    ksvm_status st;

    ksvm_hal_arm_cm_init(&hal_ctx, &hal);
    ksvm_init(&vm, &hal);
    err = ksvm_load_image(&vm, ksvm_program_image, KSVM_PROGRAM_IMAGE_SIZE,
                          ram, (uint32_t)sizeof ram);
    if (err != KSVM_OK) {
        put_s("KSVM load error ");
        put_hex((uint32_t)-err, 2u);
        put_s("\n");
        ksvm_hal_arm_cm_exit(0xFDu);
    }
    do {
        st = ksvm_run(&vm, KSVM_ARM_SLICE);
        /* Aqui o firmware atenderia suas próprias tarefas entre fatias. */
    } while (st == KSVM_RUNNING || st == KSVM_YIELD);

    if (st == KSVM_FAULT) {
        put_s("KSVM FAULT ");
        put_hex((uint32_t)vm.fault, 2u);
        put_s(" pc=");
        put_hex(vm.cpu.pc, 8u);
        put_s(" addr=");
        put_hex(vm.fault_addr, 8u);
        put_s("\n");
        ksvm_hal_arm_cm_exit(0xFCu);
    }
    put_s("KSVM halt ");
    put_hex(vm.exit_code, 8u);
    put_s("\n");
    ksvm_hal_arm_cm_exit(vm.exit_code);
    return 0;
}
