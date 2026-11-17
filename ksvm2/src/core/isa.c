/*
 * isa.c — tabela da ISA gerada de isa.def e decodificação de instruções.
 *
 * A tabela e os nomes podem morar na flash (KSVM_TABLE_PROGMEM, AVR): por
 * isso tudo aqui é lido por ksvm_isa_get/ksvm_isa_* e KSVM_TABLE_READ_*,
 * e os nomes são arrays com KSVM_TABLE_ATTR em vez de literais soltos
 * (um literal iria para a RAM no AVR).
 */
#include "ksvm/isa.h"

/* Tamanho em bytes por formato: só L carrega a palavra extra de imm32. */
#define KSVM_LEN_R 4u
#define KSVM_LEN_I 4u
#define KSVM_LEN_L 8u
#define KSVM_LEN_B 4u
#define KSVM_LEN_J 4u

#if KSVM_ISA_NAMES
/* Um array por nome ("ADD"), para que o atributo valha também para ele. */
#define KSVM_OP(name, code, fmt, fl, desc) \
    static const char ksvm_isa_name_##name[] KSVM_TABLE_ATTR = #name;
#include "ksvm/isa.def"
#undef KSVM_OP
#endif

#if KSVM_ISA_COMPACT

/* Entradas não listadas ficam zeradas = KSVM_FMT_INVALID. */
const uint8_t ksvm_isa_compact[256] KSVM_TABLE_ATTR = {
#define KSVM_OP(name, code, fmt, fl, desc) \
    [code] = (uint8_t)(KSVM_FMT_##fmt | (KSVM_AFFECTS_##fl << KSVM_ISA_COMPACT_FL_SHIFT)),
#include "ksvm/isa.def"
#undef KSVM_OP
};

#if KSVM_ISA_NAMES
const char *const ksvm_isa_names[256] KSVM_TABLE_ATTR = {
#define KSVM_OP(name, code, fmt, fl, desc) [code] = ksvm_isa_name_##name,
#include "ksvm/isa.def"
#undef KSVM_OP
};
#endif

#else /* !KSVM_ISA_COMPACT */

/* Os nomes vão direto para ## dentro de KSVM_OP, sem passar por outra
 * macro: um argumento repassado a outra macro é expandido antes, e alguns
 * nomes de opcode são macros em headers de MCU (ADC é o registrador do
 * conversor A/D em <avr/io.h>, que <avr/pgmspace.h> inclui). */
/* Entradas não listadas ficam zeradas = KSVM_FMT_INVALID, len 0. */
const ksvm_isa_info ksvm_isa[256] KSVM_TABLE_ATTR = {
#if KSVM_ISA_NAMES
#define KSVM_OP(name, code, fmt, fl, desc) \
    [code] = { KSVM_FMT_##fmt, KSVM_LEN_##fmt, KSVM_AFFECTS_##fl, ksvm_isa_name_##name },
#else
#define KSVM_OP(name, code, fmt, fl, desc) \
    [code] = { KSVM_FMT_##fmt, KSVM_LEN_##fmt, KSVM_AFFECTS_##fl },
#endif
#include "ksvm/isa.def"
#undef KSVM_OP
};

#endif /* KSVM_ISA_COMPACT */

#if !KSVM_ISA_NAMES
static const char ksvm_isa_unknown[] KSVM_TABLE_ATTR = "?";
#endif

const char *ksvm_op_name(uint8_t op)
{
#if KSVM_ISA_NAMES
    return ksvm_isa_get(op).name;
#else
    (void)op;
    return ksvm_isa_unknown;
#endif
}

const char *ksvm_cond_name(uint8_t cond)
{
#if KSVM_ISA_NAMES
    /* Linhas de tamanho fixo: sem tabela de ponteiros para ler da flash. */
    static const char names[16][3] KSVM_TABLE_ATTR = {
        "AL", "EQ", "NE", "HS", "LO", "MI", "PL", "VS",
        "VC", "HI", "LS", "GE", "LT", "GT", "LE", "NV"
    };
    return names[cond & 0x0Fu];
#else
    (void)cond;
    return ksvm_isa_unknown;
#endif
}

/* Imediato conforme o formato; R não tem imediato. */
static uint32_t decode_imm(uint8_t format, uint32_t w, uint32_t imm32)
{
    switch (format) {
    case KSVM_FMT_I: return ksvm_field_imm14(w);
    case KSVM_FMT_L: return imm32;
    case KSVM_FMT_B: return ksvm_field_off20(w);
    case KSVM_FMT_J: return ksvm_field_off24(w);
    default:         return 0u;
    }
}

bool ksvm_decode(uint32_t w, uint32_t imm32, ksvm_insn *out)
{
    uint8_t op = ksvm_field_op(w);
    uint8_t format = ksvm_isa_format(op);

    out->word   = w;
    out->op     = op;
    out->ra     = ksvm_field_ra(w);
    out->rb     = ksvm_field_rb(w);
    out->rc     = ksvm_field_rc(w);
    out->cond   = ksvm_field_cond(w);
    out->format = format;
    out->len    = ksvm_isa_len(op);
    out->imm    = decode_imm(format, w, imm32);
    return format != KSVM_FMT_INVALID;
}
