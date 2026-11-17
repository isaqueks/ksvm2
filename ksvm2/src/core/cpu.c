/*
 * cpu.c — reset do estado arquitetural e avaliação de condições.
 */
#include "ksvm/cpu.h"
#include "ksvm/isa.h"

void ksvm_cpu_reset(ksvm_cpu *cpu, uint32_t pc, uint32_t sp)
{
    unsigned i;

    for (i = 0; i < KSVM_NUM_REGS; i++) {
        cpu->r[i] = 0u;
    }
    cpu->r[KSVM_REG_SP] = sp;
    cpu->pc = pc;
    cpu->flags = 0u;
}

/*
 * Tabela do doc 04 §4. C segue a convenção ARM (C = 1: sem borrow), por isso
 * HS/LO/HI/LS saem direto das flags.
 *
 * As condições vêm em pares (2k - 1, 2k), a segunda negando a primeira:
 * EQ/NE, HS/LO, MI/PL, VS/VC, HI/LS, GE/LT, GT/LE; e AL (0) é a negação de
 * NV (15). Avaliar só a primeira de cada par e negar nas pares economiza
 * metade dos casos (flash no AVR). Só os 4 bits baixos importam: copiá-los
 * para um uint8_t evita aritmética de 32 bits no AVR (238 -> 124 bytes).
 */
bool ksvm_cond_holds(uint32_t flags, uint8_t cond)
{
    uint8_t f = (uint8_t)(flags & KSVM_FLAGS_MASK);
    bool z = (f & KSVM_FLAG_Z) != 0u;
    bool n = (f & KSVM_FLAG_N) != 0u;
    bool c = (f & KSVM_FLAG_C) != 0u;
    bool v = (f & KSVM_FLAG_V) != 0u;
    bool r;

    cond &= 0x0Fu;
    switch ((cond + 1u) >> 1) {
    case 1:  r = z;               break;   /* EQ (NE) */
    case 2:  r = c;               break;   /* HS (LO) */
    case 3:  r = n;               break;   /* MI (PL) */
    case 4:  r = v;               break;   /* VS (VC) */
    case 5:  r = c && !z;         break;   /* HI (LS) */
    case 6:  r = n == v;          break;   /* GE (LT) */
    case 7:  r = !z && (n == v);  break;   /* GT (LE) */
    default: r = false;           break;   /* NV (AL = 0 nega) */
    }
    return ((cond & 1u) != 0u) ? r : !r;
}
