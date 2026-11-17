/*
 * mnemonic.c — tabelas do assembler derivadas da ISA (ver mnemonic.h).
 */
#include <ctype.h>
#include <string.h>

#include "ksvm/isa.h"
#include "ksvm/syscall.h"
#include "ir.h"
#include "lexer.h"
#include "mnemonic.h"

#if !KSVM_ISA_NAMES
#error "ksas usa ksvm_cond_name(): compile com KSVM_ISA_NAMES=1"
#endif

/* Nomes de instrução gerados de isa.def: nunca uma lista manual. */
typedef struct isa_name {
    const char *name;
    uint8_t     op;
} isa_name;

static const isa_name isa_names[] = {
#define KSVM_OP(name, code, fmt, fl, desc) { #name, code },
#include "ksvm/isa.def"
#undef KSVM_OP
};

#define NUM_ISA_NAMES (sizeof isa_names / sizeof isa_names[0])

bool ksas_isa_lookup(const char *s, uint32_t n, uint8_t *op)
{
    size_t i;

    for (i = 0; i < NUM_ISA_NAMES; i++) {
        if (ksas_name_eq(s, n, isa_names[i].name)) {
            *op = isa_names[i].op;
            return true;
        }
    }
    return false;
}

static const char *op_name(uint8_t op)
{
    size_t i;

    for (i = 0; i < NUM_ISA_NAMES; i++) {
        if (isa_names[i].op == op) {
            return isa_names[i].name;
        }
    }
    return "?";
}

const char *ksas_op_lower(uint8_t op, char *buf, uint32_t cap)
{
    const char *name = op_name(op);
    uint32_t i;

    for (i = 0; i + 1u < cap && name[i] != '\0'; i++) {
        buf[i] = (char)tolower((unsigned char)name[i]);
    }
    buf[i] = '\0';
    return buf;
}

ksas_shape ksas_op_shape(uint8_t op)
{
    switch (op) {
    case KSVM_OP_NOP: case KSVM_OP_RET: case KSVM_OP_HALT: case KSVM_OP_BRK: case KSVM_OP_WFI:
        return KSAS_SH_NONE;
    case KSVM_OP_MOV: case KSVM_OP_NEG: case KSVM_OP_NOT: case KSVM_OP_CMP: case KSVM_OP_TST:
    case KSVM_OP_SXB: case KSVM_OP_SXH: case KSVM_OP_ZXB: case KSVM_OP_ZXH:
    case KSVM_OP_CLZ: case KSVM_OP_CTZ: case KSVM_OP_POPCNT:
        return KSAS_SH_R2;
    case KSVM_OP_PUSH: case KSVM_OP_POP:
        return KSAS_SH_RA;
    case KSVM_OP_JMPR: case KSVM_OP_CALLR:
        return KSAS_SH_RB;
    case KSVM_OP_LDB: case KSVM_OP_LDBS: case KSVM_OP_LDH: case KSVM_OP_LDHS: case KSVM_OP_LDW:
    case KSVM_OP_LDBX: case KSVM_OP_LDBSX: case KSVM_OP_LDHX: case KSVM_OP_LDHSX:
    case KSVM_OP_LDWX:
        return KSAS_SH_LOAD;
    case KSVM_OP_STB: case KSVM_OP_STH: case KSVM_OP_STW:
    case KSVM_OP_STBX: case KSVM_OP_STHX: case KSVM_OP_STWX:
        return KSAS_SH_STORE;
    case KSVM_OP_MOVI: case KSVM_OP_ADR: case KSVM_OP_MFSR:
    case KSVM_OP_CMPI: case KSVM_OP_TSTI: case KSVM_OP_MOVIL: case KSVM_OP_ADRL:
    case KSVM_OP_CMPL:
        return KSAS_SH_RI;
    case KSVM_OP_MTSR:
        return KSAS_SH_SR_RB;
    case KSVM_OP_TRAP: case KSVM_OP_SYSCALL: case KSVM_OP_PUSHM: case KSVM_OP_POPM:
        return KSAS_SH_IMM;
    default:
        break;
    }
    switch (ksvm_isa[op].format) {
    case KSVM_FMT_R: return KSAS_SH_R3;
    case KSVM_FMT_B: return KSAS_SH_BRANCH;
    case KSVM_FMT_J: return KSAS_SH_JUMP;
    default:         return KSAS_SH_RRI;
    }
}

uint8_t ksas_op_imm_kind(uint8_t op)
{
    switch (op) {
    case KSVM_OP_ADR:   return KSAS_IMM_PCREL14;
    case KSVM_OP_ADRL:  return KSAS_IMM_PCREL32;
    case KSVM_OP_SYSCALL: case KSVM_OP_MFSR: case KSVM_OP_MTSR:
    case KSVM_OP_PUSHM: case KSVM_OP_POPM:
        return KSAS_IMM_U14;
    case KSVM_OP_SHLI: case KSVM_OP_SHRI: case KSVM_OP_SARI:
        return KSAS_IMM_SHIFT;
    default:
        break;
    }
    switch (ksvm_isa[op].format) {
    case KSVM_FMT_I: return KSAS_IMM_S14;
    case KSVM_FMT_L: return KSAS_IMM_32;
    case KSVM_FMT_B: return KSAS_IMM_BR20;
    case KSVM_FMT_J: return KSAS_IMM_J24;
    default:         return KSAS_IMM_NONE;
    }
}

/* Procura prefix[0..plen) + suffix na tabela. */
static bool lookup_concat(const char *prefix, size_t plen, const char *suffix, uint8_t *out)
{
    char buf[32];
    size_t slen = strlen(suffix);

    if (plen + slen >= sizeof buf) {
        return false;
    }
    memcpy(buf, prefix, plen);
    memcpy(buf + plen, suffix, slen);
    return ksas_isa_lookup(buf, (uint32_t)(plen + slen), out);
}

bool ksas_op_suffixed(uint8_t op, const char *suffix, uint8_t *out)
{
    const char *name = op_name(op);
    return lookup_concat(name, strlen(name), suffix, out);
}

/* Só imediatos S14 aritméticos/lógicos têm forma longa (MOVI, ADDI, ANDI,
 * ORI, XORI, CMPI); o nome dela é NOME+"L" ou NOME-sem-"I"+"L". SUBI não
 * tem SUBL: o parser usa ADDL com o imediato negado (doc 04 §7). */
uint8_t ksas_op_long_form(uint8_t op)
{
    const char *name = op_name(op);
    size_t len = strlen(name);
    uint8_t out;

    if (ksvm_isa[op].format != KSVM_FMT_I || ksas_op_imm_kind(op) != KSAS_IMM_S14) {
        return KSAS_NO_OP;
    }
    if (lookup_concat(name, len, "L", &out)) {
        return out;
    }
    if (len > 1u && name[len - 1u] == 'I' && lookup_concat(name, len - 1u, "L", &out)) {
        return out;
    }
    return KSAS_NO_OP;
}

bool ksas_cond_lookup(const char *s, uint32_t n, uint8_t *cond)
{
    uint8_t i;

    for (i = 0; i < 16u; i++) {
        if (ksas_name_eq(s, n, ksvm_cond_name(i))) {
            *cond = i;
            return true;
        }
    }
    /* Aliases do doc 07: CS = HS, CC = LO. */
    if (ksas_name_eq(s, n, "cs")) { *cond = KSVM_COND_HS; return true; }
    if (ksas_name_eq(s, n, "cc")) { *cond = KSVM_COND_LO; return true; }
    return false;
}

typedef struct named_num {
    const char *name;
    uint32_t    num;
} named_num;

static bool lookup_named(const named_num *t, size_t count, const char *s, uint32_t n,
                         uint32_t *num)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (ksas_name_eq(s, n, t[i].name)) {
            *num = t[i].num;
            return true;
        }
    }
    return false;
}

bool ksas_syscall_lookup(const char *s, uint32_t n, uint32_t *num)
{
    static const named_num names[] = {
        { "EXIT", KSVM_SYSCALL_EXIT },         { "CAPS", KSVM_SYSCALL_CAPS },
        { "VERSION", KSVM_SYSCALL_VERSION },   { "YIELD", KSVM_SYSCALL_YIELD },
        { "ABORT", KSVM_SYSCALL_ABORT },       { "PUTC", KSVM_SYSCALL_PUTC },
        { "GETC", KSVM_SYSCALL_GETC },         { "PUTS", KSVM_SYSCALL_PUTS },
        { "GETS", KSVM_SYSCALL_GETS },         { "MILLIS", KSVM_SYSCALL_MILLIS },
        { "SLEEP_MS", KSVM_SYSCALL_SLEEP_MS }, { "MICROS", KSVM_SYSCALL_MICROS },
        { "OPEN", KSVM_SYSCALL_OPEN },         { "CLOSE", KSVM_SYSCALL_CLOSE },
        { "READ", KSVM_SYSCALL_READ },         { "WRITE", KSVM_SYSCALL_WRITE },
        { "SEEK", KSVM_SYSCALL_SEEK },         { "PIN_MODE", KSVM_SYSCALL_PIN_MODE },
        { "PIN_WRITE", KSVM_SYSCALL_PIN_WRITE }, { "PIN_READ", KSVM_SYSCALL_PIN_READ },
        { "ANALOG_READ", KSVM_SYSCALL_ANALOG_READ },
        { "PWM_WRITE", KSVM_SYSCALL_PWM_WRITE },
        { "MEM_INFO", KSVM_SYSCALL_MEM_INFO }, { "RANDOM", KSVM_SYSCALL_RANDOM }
    };
    return lookup_named(names, sizeof names / sizeof names[0], s, n, num);
}

bool ksas_sr_lookup(const char *s, uint32_t n, uint32_t *num)
{
    static const named_num names[] = {
        { "PC", KSVM_SR_PC }, { "FLAGS", KSVM_SR_FLAGS },
        { "ISA_VERSION", KSVM_SR_ISA_VERSION }, { "INSN_COUNT", KSVM_SR_INSN_COUNT }
    };
    return lookup_named(names, sizeof names / sizeof names[0], s, n, num);
}
