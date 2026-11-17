/*
 * fixture.c — montagem da VM de teste (ver fixture.h).
 */
#include <string.h>

#include "fixture.h"

void fix_load_hal(fixture *f, const uint32_t *words, size_t count, const ksvm_hal *hal)
{
    size_t i;

    memset(f, 0, sizeof *f);
    memset(f->code, 0xFF, sizeof f->code);
    for (i = 0; i < count && (i + 1u) * 4u <= FIX_CODE_SIZE; i++) {
        ksvm_st32le(&f->code[i * 4u], words[i]);
    }
    ksvm_init(&f->vm, hal);
    (void)ksvm_map_rom(&f->vm, 0u, f->code, FIX_CODE_SIZE, NULL);
    (void)ksvm_map_ram(&f->vm, FIX_RAM_BASE, f->ram, FIX_RAM_SIZE);
    f->vm.stack_limit = FIX_RAM_BASE;
    f->vm.heap_start = FIX_RAM_BASE;
    ksvm_reset(&f->vm, 0u, FIX_RAM_END);
}

void fix_load(fixture *f, const uint32_t *words, size_t count)
{
    fix_load_hal(f, words, count, NULL);
    ksvm_hal_null_init(&f->hn, &f->hal, &f->vm);
    f->vm.hal = &f->hal;
}

ksvm_status fix_run(fixture *f)
{
    return ksvm_run(&f->vm, FIX_MAX_STEPS);
}

ksvm_status fix_steps(fixture *f, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        ksvm_step(&f->vm);
    }
    return f->vm.status;
}

uint32_t fix_ram32(const fixture *f, uint32_t addr)
{
    return ksvm_ld32le(&f->ram[addr - FIX_RAM_BASE]);
}

void fix_set_ram32(fixture *f, uint32_t addr, uint32_t v)
{
    ksvm_st32le(&f->ram[addr - FIX_RAM_BASE], v);
}
