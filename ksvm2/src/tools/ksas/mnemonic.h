/*
 * mnemonic.h — vocabulário do assembler derivado da ISA.
 *
 * Os nomes de instrução vêm de isa.def (X-macro); formato e tamanho de
 * ksvm_isa[]. O que isa.def não descreve — a FORMA dos operandos de cada
 * opcode (doc 04 §5) — fica em ksas_op_shape, por opcode (KSVM_OP_*),
 * com padrão por formato. Variantes (ADD -> ADDI -> ADDL, LDB -> LDBX)
 * são encontradas pelo NOME na tabela, não por listas paralelas.
 */
#ifndef KSAS_MNEMONIC_H
#define KSAS_MNEMONIC_H

#include <stdbool.h>
#include <stdint.h>

/* Forma dos operandos no fonte -> campos de bits (topo de isa.def). */
typedef enum ksas_shape {
    KSAS_SH_NONE,      /* nop, ret, halt, brk, wfi */
    KSAS_SH_R3,        /* rd, ra, rb   -> ra, rb, rc (ou rd, ra, imm) */
    KSAS_SH_R2,        /* rd, rb / ra, rb -> ra, rb (mov, neg, cmp, tst...) */
    KSAS_SH_RA,        /* push ra / pop rd -> ra */
    KSAS_SH_RB,        /* jmpr rb, callr rb -> rb */
    KSAS_SH_LOAD,      /* rd, [mem] */
    KSAS_SH_STORE,     /* [mem], ra */
    KSAS_SH_RRI,       /* rd, ra, imm  -> ra, rb, imm */
    KSAS_SH_RI,        /* rd, imm / ra, imm -> ra, imm */
    KSAS_SH_SR_RB,     /* mtsr sr, rb  -> imm, rb */
    KSAS_SH_IMM,       /* trap/syscall/pushm/popm imm */
    KSAS_SH_BRANCH,    /* BCOND: só via b<cond> */
    KSAS_SH_JUMP       /* jmp/call alvo */
} ksas_shape;

/* Mnemônico da ISA (sem diferenciar maiúsculas) -> opcode. */
bool ksas_isa_lookup(const char *s, uint32_t n, uint8_t *op);

/* Nome do opcode em minúsculas ("addi") para mensagens. */
const char *ksas_op_lower(uint8_t op, char *buf, uint32_t cap);

ksas_shape ksas_op_shape(uint8_t op);

/* Tipo de imediato de um opcode I/L (ksas_imm_kind). */
uint8_t ksas_op_imm_kind(uint8_t op);

/* Opcode cujo nome é nome(op) + suffix (ADD+"I" = ADDI, LDB+"X" = LDBX). */
bool ksas_op_suffixed(uint8_t op, const char *suffix, uint8_t *out);

/* Forma longa (L) de um opcode I: MOVI -> MOVIL, ADDI -> ADDL...
 * KSAS_NO_OP se não existe. */
uint8_t ksas_op_long_form(uint8_t op);

/* Sufixo de condição ("eq", "hs", "cs"...) -> código 0..15. */
bool ksas_cond_lookup(const char *s, uint32_t n, uint8_t *cond);

/* Nome de syscall do doc 06 ("PUTC") -> número. */
bool ksas_syscall_lookup(const char *s, uint32_t n, uint32_t *num);

/* Registrador especial ("flags") -> número. */
bool ksas_sr_lookup(const char *s, uint32_t n, uint32_t *num);

#endif /* KSAS_MNEMONIC_H */
