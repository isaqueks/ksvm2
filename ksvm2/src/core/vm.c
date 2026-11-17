/*
 * vm.c — ciclo de vida da instância: init, mapeamentos, reset, run.
 * (O passo de execução em si está em exec.c.)
 */
#include <string.h>

#include "ksvm/vm.h"

/* HAL vazia usada quando o host passa NULL: o núcleo só precisa testar os
 * ponteiros de função, nunca o ponteiro da HAL. */
static const ksvm_hal ksvm_hal_none = { .ctx = NULL };

void ksvm_init(ksvm *vm, const ksvm_hal *hal)
{
    KSVM_MEMSET(vm, 0, sizeof *vm);    /* inclui o barramento: = ksvm_bus_init */
    vm->hal = (hal != NULL) ? hal : &ksvm_hal_none;
    vm->status = KSVM_HALTED;          /* nada roda antes de ksvm_reset */
    vm->fault = KSVM_FAULT_NONE;
    vm->rng_state = KSVM_RNG_SEED;
#if KSVM_ENABLE_TRACE
    vm->trace = NULL;
    vm->trace_ctx = NULL;
#endif
}

static void region_init(ksvm_region *r, uint32_t base, uint32_t size,
                        uint8_t perm, uint8_t kind, void *backing)
{
    r->base = base;
    r->size = size;
    r->perm = perm;
    r->kind = kind;
    r->backing = backing;
    r->read8 = NULL;
    r->write8 = NULL;
}

int ksvm_map_ram(ksvm *vm, uint32_t base, uint8_t *mem, uint32_t size)
{
    ksvm_region r;

    if (mem == NULL) {
        return KSVM_E_ARG;
    }
    region_init(&r, base, size, KSVM_MEM_READ | KSVM_MEM_WRITE, KSVM_REGION_RAM, mem);
    return ksvm_bus_map(&vm->bus, &r);
}

/* O cast remove o const só para caber em "backing": a região não recebe
 * KSVM_MEM_WRITE e ksvm_bus_map recusa escrita direta fora de RAM, então o
 * núcleo nunca escreve através dele. */
int ksvm_map_rom(ksvm *vm, uint32_t base, const uint8_t *mem, uint32_t size,
                 ksvm_read8_fn read8)
{
    ksvm_region r;

    if (mem == NULL) {
        return KSVM_E_ARG;
    }
    region_init(&r, base, size, KSVM_MEM_READ | KSVM_MEM_EXEC, KSVM_REGION_ROM,
                (void *)mem);
    r.read8 = read8;
    return ksvm_bus_map(&vm->bus, &r);
}

int ksvm_map_mmio(ksvm *vm, uint32_t base, uint32_t size, void *ctx,
                  ksvm_read8_fn rd, ksvm_write8_fn wr)
{
    ksvm_region r;
    uint8_t perm = 0;

    if (rd != NULL) {
        perm |= KSVM_MEM_READ;
    }
    if (wr != NULL) {
        perm |= KSVM_MEM_WRITE;
    }
    if (perm == 0u) {
        return KSVM_E_ARG;
    }
    region_init(&r, base, size, perm, KSVM_REGION_MMIO, ctx);
    r.read8 = rd;
    r.write8 = wr;
    return ksvm_bus_map(&vm->bus, &r);
}

void ksvm_reset(ksvm *vm, uint32_t entry, uint32_t sp)
{
    ksvm_cpu_reset(&vm->cpu, entry, sp);
    vm->status = KSVM_RUNNING;
    vm->fault = KSVM_FAULT_NONE;
    vm->fault_addr = 0u;
    vm->exit_code = 0u;
    vm->insn_count = 0u;
}

ksvm_status ksvm_run(ksvm *vm, uint32_t max_insns)
{
    uint32_t done = 0u;

    if (vm->status == KSVM_YIELD) {
        vm->status = KSVM_RUNNING;
    }
    while (vm->status == KSVM_RUNNING && (max_insns == 0u || done < max_insns)) {
        ksvm_step(vm);
        done++;
    }
    return vm->status;
}

void ksvm_set_fault(ksvm *vm, ksvm_fault f, uint32_t addr)
{
    vm->status = KSVM_FAULT;
    vm->fault = f;
    vm->fault_addr = addr;
}

void ksvm_halt(ksvm *vm, uint32_t code)
{
    vm->status = KSVM_HALTED;
    vm->exit_code = code;
    if (vm->hal->on_halt != NULL) {
        vm->hal->on_halt(vm->hal->ctx, code);
    }
}

#if KSVM_ENABLE_TRACE
void ksvm_set_trace(ksvm *vm, ksvm_trace_fn fn, void *ctx)
{
    vm->trace = fn;
    vm->trace_ctx = ctx;
}
#endif
