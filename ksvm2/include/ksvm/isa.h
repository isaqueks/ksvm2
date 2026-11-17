/*
 * isa.h — opcodes, formatos, campos de bits, condições e flags da ISA 2.0.
 *
 * Tudo que descreve instruções vem de isa.def; este header só define os
 * tipos e os extratores de campo (máscaras e shifts, sem UB).
 */
#ifndef KSVM_ISA_H
#define KSVM_ISA_H

#include "bytes.h"
#include "types.h"

/* Opcodes: KSVM_OP_NOP = 0x00, KSVM_OP_ADD = 0x30, ... */
typedef enum ksvm_op {
#define KSVM_OP(name, code, fmt, fl, desc) KSVM_OP_##name = code,
#include "isa.def"
#undef KSVM_OP
    KSVM_OP_INVALID = 0xFF   /* 0xF0..0xFF nunca serão opcodes válidos */
} ksvm_op;

typedef enum ksvm_format {
    KSVM_FMT_INVALID = 0,    /* zero: entradas não listadas da tabela */
    KSVM_FMT_R,
    KSVM_FMT_I,
    KSVM_FMT_L,
    KSVM_FMT_B,
    KSVM_FMT_J
} ksvm_format;

/* Bits de FLAGS (doc 04 §2). Bits 4..31 reservados, lidos como 0. */
#define KSVM_FLAG_Z     UINT32_C(0x1)
#define KSVM_FLAG_N     UINT32_C(0x2)
#define KSVM_FLAG_C     UINT32_C(0x4)
#define KSVM_FLAG_V     UINT32_C(0x8)
#define KSVM_FLAGS_MASK UINT32_C(0xF)

/* Valores da coluna "flags" do isa.def. */
#define KSVM_AFFECTS_NONE 0u
#define KSVM_AFFECTS_ZN   (KSVM_FLAG_Z | KSVM_FLAG_N)
#define KSVM_AFFECTS_ZNC  (KSVM_FLAG_Z | KSVM_FLAG_N | KSVM_FLAG_C)
#define KSVM_AFFECTS_ZNCV KSVM_FLAGS_MASK

/* Condições de BCOND (doc 04 §4). */
typedef enum ksvm_cond {
    KSVM_COND_AL = 0, KSVM_COND_EQ, KSVM_COND_NE, KSVM_COND_HS,
    KSVM_COND_LO,     KSVM_COND_MI, KSVM_COND_PL, KSVM_COND_VS,
    KSVM_COND_VC,     KSVM_COND_HI, KSVM_COND_LS, KSVM_COND_GE,
    KSVM_COND_LT,     KSVM_COND_GT, KSVM_COND_LE, KSVM_COND_NV
} ksvm_cond;

/* Registradores com papel fixo. */
#define KSVM_NUM_REGS 32u
#define KSVM_REG_ZR   0u
#define KSVM_REG_FP   30u
#define KSVM_REG_SP   31u

/* Registradores especiais (MFSR/MTSR). */
#define KSVM_SR_PC          0u
#define KSVM_SR_FLAGS       1u
#define KSVM_SR_ISA_VERSION 2u
#define KSVM_SR_INSN_COUNT  3u

/* Limites dos imediatos com sinal. */
#define KSVM_IMM14_MIN (-8192L)
#define KSVM_IMM14_MAX 8191L

/* Informação estática de cada opcode, gerada de isa.def. */
typedef struct ksvm_isa_info {
    uint8_t format;          /* ksvm_format */
    uint8_t len;             /* 4, ou 8 no formato L; 0 se inválido */
    uint8_t flags;           /* máscara KSVM_FLAG_* que a instrução altera */
#if KSVM_ISA_NAMES
    const char *name;
#endif
} ksvm_isa_info;

/*
 * Leitura da tabela: o núcleo só usa ksvm_isa_get, ksvm_isa_format e
 * ksvm_isa_len, que leem por KSVM_TABLE_READ_* (acesso direto no desktop,
 * LPM na flash do AVR com KSVM_TABLE_PROGMEM). Os campos isolados existem
 * para o caminho quente (fetch/decode): no AVR cada leitura da flash é um
 * LPM volátil que o compilador não elimina.
 */
#if KSVM_ISA_COMPACT

/* Um byte por opcode: bits 0..2 = formato, bits 4..7 = flags afetadas. */
#define KSVM_ISA_COMPACT_FMT_MASK 0x07u
#define KSVM_ISA_COMPACT_FL_SHIFT 4u
extern const uint8_t ksvm_isa_compact[256] KSVM_TABLE_ATTR;
#if KSVM_ISA_NAMES
extern const char *const ksvm_isa_names[256] KSVM_TABLE_ATTR;
#endif

static inline uint8_t ksvm_isa_format(uint8_t op)
{
    return (uint8_t)(KSVM_TABLE_READ_U8(&ksvm_isa_compact[op]) & KSVM_ISA_COMPACT_FMT_MASK);
}

static inline uint8_t ksvm_isa_len_of(uint8_t format)
{
    if (format == KSVM_FMT_INVALID) {
        return 0u;
    }
    return (format == KSVM_FMT_L) ? 8u : 4u;
}

static inline uint8_t ksvm_isa_len(uint8_t op)
{
    return ksvm_isa_len_of(ksvm_isa_format(op));
}

static inline ksvm_isa_info ksvm_isa_get(uint8_t op)
{
    ksvm_isa_info e;
    uint8_t b = KSVM_TABLE_READ_U8(&ksvm_isa_compact[op]);

    e.format = (uint8_t)(b & KSVM_ISA_COMPACT_FMT_MASK);
    e.len    = ksvm_isa_len_of(e.format);
    e.flags  = (uint8_t)(b >> KSVM_ISA_COMPACT_FL_SHIFT);
#if KSVM_ISA_NAMES
    e.name   = (const char *)KSVM_TABLE_READ_PTR(&ksvm_isa_names[op]);
#endif
    return e;
}

#else /* !KSVM_ISA_COMPACT */

/* Tabela indexada pelo opcode. Com KSVM_TABLE_PROGMEM=1 ela fica na flash
 * e só pode ser lida pelas funções abaixo (ver config.h). */
extern const ksvm_isa_info ksvm_isa[256] KSVM_TABLE_ATTR;

/* Copia a entrada por valor. */
static inline ksvm_isa_info ksvm_isa_get(uint8_t op)
{
    ksvm_isa_info e;
    const ksvm_isa_info *p = &ksvm_isa[op];

    e.format = KSVM_TABLE_READ_U8(&p->format);
    e.len    = KSVM_TABLE_READ_U8(&p->len);
    e.flags  = KSVM_TABLE_READ_U8(&p->flags);
#if KSVM_ISA_NAMES
    e.name   = (const char *)KSVM_TABLE_READ_PTR(&p->name);
#endif
    return e;
}

static inline uint8_t ksvm_isa_format(uint8_t op)
{
    return KSVM_TABLE_READ_U8(&ksvm_isa[op].format);
}

static inline uint8_t ksvm_isa_len(uint8_t op)
{
    return KSVM_TABLE_READ_U8(&ksvm_isa[op].len);
}

#endif /* KSVM_ISA_COMPACT */

/* Nome do opcode ("ADD"), NULL se inválido; "?" com KSVM_ISA_NAMES=0. */
const char *ksvm_op_name(uint8_t op);
/* Sufixo da condição ("EQ"); "?" com KSVM_ISA_NAMES=0. */
const char *ksvm_cond_name(uint8_t cond);

/* Instrução decodificada: campos brutos + imediato já estendido. */
typedef struct ksvm_insn {
    uint32_t word;
    uint32_t imm;   /* I: sext(imm14); L: imm32; B/J: deslocamento em bytes */
    uint8_t  op;
    uint8_t  ra, rb, rc;
    uint8_t  cond;
    uint8_t  format; /* ksvm_format */
    uint8_t  len;
} ksvm_insn;

/* Decodifica w (imm32 só é usado no formato L). Devolve false, e deixa
 * format = KSVM_FMT_INVALID, se o opcode não existe. */
bool ksvm_decode(uint32_t w, uint32_t imm32, ksvm_insn *out);

/* Extratores de campo (doc 04 §3). */
static inline uint8_t ksvm_field_op(uint32_t w)   { return (uint8_t)(w & 0xFFu); }
static inline uint8_t ksvm_field_ra(uint32_t w)   { return (uint8_t)((w >> 8) & 0x1Fu); }
static inline uint8_t ksvm_field_rb(uint32_t w)   { return (uint8_t)((w >> 13) & 0x1Fu); }
static inline uint8_t ksvm_field_rc(uint32_t w)   { return (uint8_t)((w >> 18) & 0x1Fu); }
static inline uint8_t ksvm_field_cond(uint32_t w) { return (uint8_t)((w >> 8) & 0x0Fu); }

/* imm14 como 14 bits sem sinal (número de SYSCALL, 0..0x3FFF). */
static inline uint32_t ksvm_field_uimm14(uint32_t w) { return w >> 18; }
static inline uint32_t ksvm_field_imm14(uint32_t w)  { return ksvm_sext14(w >> 18); }

/* Deslocamentos de desvio já em bytes (complemento de dois em uint32_t). */
static inline uint32_t ksvm_field_off20(uint32_t w)  { return ksvm_sext20(w >> 12) << 2; }
static inline uint32_t ksvm_field_off24(uint32_t w)  { return ksvm_sext24(w >> 8) << 2; }

#endif /* KSVM_ISA_H */
