/*
 * fault.c — nomes legíveis dos faults (opcionais para poupar flash no AVR).
 *
 * Linhas de tamanho fixo com KSVM_TABLE_ATTR: com KSVM_TABLE_PROGMEM os
 * nomes ficam na flash e o ponteiro devolvido é um endereço de flash.
 */
#include "ksvm/fault.h"

static const char ksvm_fault_unknown[] KSVM_TABLE_ATTR = "?";

const char *ksvm_fault_name(ksvm_fault f)
{
#if KSVM_FAULT_NAMES
    static const char names[KSVM_FAULT_COUNT][11] KSVM_TABLE_ATTR = {
        "NONE", "BAD_OPCODE", "PC_ALIGN", "PC_RANGE", "MEM_RANGE",
        "MEM_PERM", "DIV_ZERO", "STACK", "TRAP"
    };
    if ((unsigned)f >= (unsigned)KSVM_FAULT_COUNT) {
        return ksvm_fault_unknown;
    }
    return names[f];
#else
    (void)f;
    return ksvm_fault_unknown;
#endif
}
