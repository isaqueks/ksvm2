/*
 * cpu.h — estado arquitetural do processador (registradores, PC, FLAGS).
 */
#ifndef KSVM_CPU_H
#define KSVM_CPU_H

#include "types.h"

typedef struct ksvm_cpu {
    uint32_t r[32];      /* r[0] lido sempre como 0: o laço zera após cada instrução */
    uint32_t pc;         /* deve ser múltiplo de 4 (verificado no fetch) */
    uint32_t flags;      /* bits Z N C V (KSVM_FLAG_*) */
} ksvm_cpu;

/* Zera registradores e flags; define pc e sp (r31). */
void ksvm_cpu_reset(ksvm_cpu *cpu, uint32_t pc, uint32_t sp);

/* Avalia a condição de BCOND (0..15) sobre as flags (doc 04 §4). */
bool ksvm_cond_holds(uint32_t flags, uint8_t cond);

#endif /* KSVM_CPU_H */
