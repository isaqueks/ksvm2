/*
 * vm.h — a instância da VM e sua API de ciclo de vida (doc 03 §3.4, §4).
 *
 * struct ksvm não contém ponteiros para dentro de si mesma: pode ser copiada
 * com memcpy (snapshot). Não há estado global: várias instâncias coexistem.
 */
#ifndef KSVM_VM_H
#define KSVM_VM_H

#include "bus.h"
#include "cpu.h"
#include "fault.h"
#include "hal.h"
#include "trace.h"
#include "types.h"

/* Semente padrão da syscall RANDOM (o host pode trocar vm->rng_state). */
#define KSVM_RNG_SEED UINT32_C(0x2545F491)

typedef struct ksvm {
    ksvm_cpu        cpu;
    ksvm_bus        bus;
    const ksvm_hal *hal;          /* nunca NULL após ksvm_init */
    ksvm_status     status;
    ksvm_fault      fault;        /* detalhe quando status == KSVM_FAULT */
    uint32_t        fault_addr;   /* endereço, código ou palavra envolvida */
    uint32_t        exit_code;
    uint32_t        stack_limit;  /* PUSH/CALL com sp - 4 < limite: KSVM_FAULT_STACK */
    uint32_t        insn_count;   /* instruções concluídas (wrap em 2^32) */
    uint32_t        heap_start;   /* fim de .bss (syscall MEM_INFO); loader define */
    uint32_t        rng_state;    /* estado do xorshift32 da syscall RANDOM (!= 0) */
#if KSVM_ENABLE_TRACE
    ksvm_trace_fn   trace;
    void           *trace_ctx;
#endif
} ksvm;

/* Zera a instância, sem regiões. hal pode ser NULL (nenhum serviço). A VM
 * fica parada (KSVM_HALTED) até ksvm_reset. */
void ksvm_init(ksvm *vm, const ksvm_hal *hal);

/* Mapeamentos (KSVM_OK ou KSVM_E_*; ver ksvm_bus_map).
 * RAM: leitura+escrita. ROM: leitura+execução, read8 NULL = acesso direto.
 * MMIO: leitura se rd != NULL, escrita se wr != NULL. */
int ksvm_map_ram(ksvm *vm, uint32_t base, uint8_t *mem, uint32_t size);
int ksvm_map_rom(ksvm *vm, uint32_t base, const uint8_t *mem, uint32_t size,
                 ksvm_read8_fn read8);
int ksvm_map_mmio(ksvm *vm, uint32_t base, uint32_t size, void *ctx,
                  ksvm_read8_fn rd, ksvm_write8_fn wr);

/* Zera registradores/flags/contadores, pc = entry, sp = sp, status RUNNING.
 * Mantém barramento, hal, stack_limit, heap_start e rng_state. */
void ksvm_reset(ksvm *vm, uint32_t entry, uint32_t sp);

/* Executa uma instrução. Retoma de KSVM_YIELD; não faz nada em HALTED/FAULT. */
ksvm_status ksvm_step(ksvm *vm);

/* Executa até parar, ceder (WFI/YIELD) ou max_insns instruções (0 = sem
 * limite). Devolve o status final (KSVM_RUNNING se esgotou a fatia). */
ksvm_status ksvm_run(ksvm *vm, uint32_t max_insns);

/* Coloca a VM em fault (usado pelo núcleo e por syscalls do host). */
void ksvm_set_fault(ksvm *vm, ksvm_fault f, uint32_t addr);

/* Para a VM normalmente (HALT/EXIT): exit_code e hal->on_halt. */
void ksvm_halt(ksvm *vm, uint32_t code);

#endif /* KSVM_VM_H */
