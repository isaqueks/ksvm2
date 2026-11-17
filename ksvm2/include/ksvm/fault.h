/*
 * fault.h — causas de parada anormal da VM.
 *
 * Um fault nunca derruba o host: a instrução que falhou não tem efeito, o PC
 * continua apontando para ela, vm->status vira KSVM_FAULT e vm->fault /
 * vm->fault_addr descrevem o motivo.
 */
#ifndef KSVM_FAULT_H
#define KSVM_FAULT_H

#include "types.h"

typedef enum ksvm_fault {
    KSVM_FAULT_NONE = 0,
    KSVM_FAULT_BAD_OPCODE, /* opcode inválido/desligado, SR inválido; addr = palavra */
    KSVM_FAULT_PC_ALIGN,   /* PC não múltiplo de 4; addr = pc */
    KSVM_FAULT_PC_RANGE,   /* fetch fora de região EXEC; addr = endereço buscado */
    KSVM_FAULT_MEM_RANGE,  /* load/store fora de região; addr = EA */
    KSVM_FAULT_MEM_PERM,   /* load/store sem permissão (ex.: escrita em ROM) */
    KSVM_FAULT_DIV_ZERO,   /* DIV/DIVU/REM/REMU com divisor 0; addr = pc */
    KSVM_FAULT_STACK,      /* PUSH/CALL abaixo de stack_limit; addr = novo sp */
    KSVM_FAULT_TRAP,       /* TRAP imm ou syscall ABORT; addr = código */
    KSVM_FAULT_COUNT
} ksvm_fault;

/* Nome legível ("MEM_RANGE"...). Com KSVM_FAULT_NAMES=0 devolve "?". */
const char *ksvm_fault_name(ksvm_fault f);

#endif /* KSVM_FAULT_H */
