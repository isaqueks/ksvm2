/*
 * disasm.c — disassembler de uma instrução (sintaxe em disasm.h).
 *
 * Formato, tamanho e campos vêm de ksvm_decode (tabela ksvm_isa[]); os nomes
 * vêm de isa.def por X-macro num switch, independentes de KSVM_ISA_NAMES e
 * de a tabela estar em PROGMEM. O que cada opcode mostra como operandos
 * (o "shape") segue o mapeamento operando -> campo descrito no topo de
 * isa.def; opcodes novos caem no shape padrão do formato.
 *
 * Sem stdio: números são formatados à mão num buffer com limite.
 */
#include "ksvm/disasm.h"
#include "ksvm/isa.h"

#if KSVM_ENABLE_DISASM

/* ---- Saída com limite ------------------------------------------------- */

typedef struct out_buf {
    char  *buf;
    size_t cap;
    size_t len;
} out_buf;

static void out_init(out_buf *o, char *buf, size_t cap)
{
    o->buf = buf;
    o->cap = (buf != NULL) ? cap : 0u;
    o->len = 0u;
    if (o->cap != 0u) {
        buf[0] = '\0';
    }
}

/* Acrescenta c se ainda couber com o '\0'; o excedente é descartado. */
static void put_c(out_buf *o, char c)
{
    if (o->cap != 0u && o->len + 1u < o->cap) {
        o->buf[o->len] = c;
        o->len++;
        o->buf[o->len] = '\0';
    }
}

static void put_s(out_buf *o, const char *s)
{
    while (*s != '\0') {
        put_c(o, *s);
        s++;
    }
}

/* Copia em minúsculas (mnemônicos do isa.def são maiúsculos). */
static void put_lower(out_buf *o, const char *s)
{
    while (*s != '\0') {
        char c = *s;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        put_c(o, c);
        s++;
    }
}

/* "0x" + hex maiúsculo, pelo menos min_digits dígitos (1..8). */
static void put_hex(out_buf *o, uint32_t v, unsigned min_digits)
{
    static const char digits[] = "0123456789ABCDEF";
    char tmp[8];
    unsigned n = 0u;

    do {
        tmp[n] = digits[v & 0xFu];
        n++;
        v >>= 4;
    } while (v != 0u);
    while (n < min_digits && n < 8u) {
        tmp[n] = '0';
        n++;
    }
    put_s(o, "0x");
    while (n > 0u) {
        n--;
        put_c(o, tmp[n]);
    }
}

static void put_udec(out_buf *o, uint32_t v)
{
    static const char digits[] = "0123456789";
    char tmp[10];
    unsigned n = 0u;

    do {
        tmp[n] = digits[v % 10u];
        n++;
        v /= 10u;
    } while (v != 0u);
    while (n > 0u) {
        n--;
        put_c(o, tmp[n]);
    }
}

static bool negative(uint32_t v)
{
    return (v & UINT32_C(0x80000000)) != 0u;
}

/* v interpretado em complemento de dois. */
static void put_sdec(out_buf *o, uint32_t v)
{
    if (negative(v)) {
        put_c(o, '-');
        v = 0u - v;
    }
    put_udec(o, v);
}

/* Deslocamento com sinal explícito: "+0x40", "-0x8". */
static void put_rel(out_buf *o, uint32_t off)
{
    if (negative(off)) {
        put_c(o, '-');
        off = 0u - off;
    } else {
        put_c(o, '+');
    }
    put_hex(o, off, 1u);
}

static void put_reg(out_buf *o, uint8_t r)
{
    if (r == KSVM_REG_ZR) {
        put_s(o, "zr");
    } else if (r == KSVM_REG_FP) {
        put_s(o, "fp");
    } else if (r == KSVM_REG_SP) {
        put_s(o, "sp");
    } else {
        put_c(o, 'r');
        put_udec(o, r);
    }
}

static void put_sep(out_buf *o)
{
    put_s(o, ", ");
}

/* ---- Nomes e shapes --------------------------------------------------- */

static const char *op_name(uint8_t op)
{
    switch (op) {
#define KSVM_OP(name, code, fmt, fl, desc) case code: return #name;
#include "ksvm/isa.def"
#undef KSVM_OP
    default: return NULL;
    }
}

static const char *cond_mnemonic(uint8_t cond)
{
    static const char *const names[16] = {
        "bal", "beq", "bne", "bhs", "blo", "bmi", "bpl", "bvs",
        "bvc", "bhi", "bls", "bge", "blt", "bgt", "ble", NULL   /* NV */
    };
    return names[cond & 0x0Fu];
}

/* Como os campos viram operandos (ver isa.def). */
typedef enum shape {
    SH_NONE,        /* ret */
    SH_D_A_B,       /* add rd, ra, rb        campos ra, rb, rc */
    SH_D_B,         /* mov rd, rb            ra, rb */
    SH_A_B,         /* cmp ra, rb            ra, rb */
    SH_A,           /* push ra               ra */
    SH_B,           /* jmpr rb               rb */
    SH_LDX,         /* ldwx rd, [rb + rc]    ra, rb, rc */
    SH_STX,         /* stwx [rb + rc], ra    ra, rb, rc */
    SH_D_A_IMM,     /* addi rd, ra, imm      ra, rb, imm14 */
    SH_LD,          /* ldw rd, [rb + imm]    ra, rb, imm14 */
    SH_ST,          /* stw [rb + imm], ra    ra, rb, imm14 */
    SH_D_IMM,       /* movi rd, imm          ra, imm14 */
    SH_D_UIMM,      /* mfsr rd, sr           ra, imm14 sem sinal (o ksas exige 0..16383) */
    SH_A_IMM,       /* cmpi ra, imm          ra, imm14 */
    SH_D_PCREL,     /* adr rd, alvo          ra, imm14 */
    SH_SR_B,        /* mtsr sr, rb           rb, imm14 */
    SH_IMM,         /* trap imm              imm14 com sinal */
    SH_UIMM,        /* syscall 0x10          imm14 sem sinal */
    SH_D_A_IMM32,   /* addl rd, ra, imm32    ra, rb + imm32 */
    SH_D_IMM32,     /* movil rd, imm32       ra + imm32 */
    SH_A_IMM32,     /* cmpl ra, imm32        ra + imm32 */
    SH_D_PCREL32,   /* adrl rd, alvo         ra + imm32 */
    SH_BRANCH,      /* beq alvo              cond, off20 */
    SH_JUMP         /* call alvo             off24 */
} shape;

static shape default_shape(uint8_t format)
{
    switch (format) {
    case KSVM_FMT_I: return SH_D_A_IMM;
    case KSVM_FMT_L: return SH_D_A_IMM32;
    case KSVM_FMT_B: return SH_BRANCH;
    case KSVM_FMT_J: return SH_JUMP;
    default:         return SH_D_A_B;
    }
}

static shape op_shape(uint8_t op, uint8_t format)
{
    switch (op) {
    case KSVM_OP_NOP: case KSVM_OP_RET: case KSVM_OP_HALT:
    case KSVM_OP_BRK: case KSVM_OP_WFI:
        return SH_NONE;
    case KSVM_OP_MOV: case KSVM_OP_NEG: case KSVM_OP_NOT:
    case KSVM_OP_SXB: case KSVM_OP_SXH: case KSVM_OP_ZXB: case KSVM_OP_ZXH:
    case KSVM_OP_CLZ: case KSVM_OP_CTZ: case KSVM_OP_POPCNT:
        return SH_D_B;
    case KSVM_OP_CMP: case KSVM_OP_TST:     return SH_A_B;
    case KSVM_OP_PUSH: case KSVM_OP_POP:    return SH_A;
    case KSVM_OP_JMPR: case KSVM_OP_CALLR:  return SH_B;
    case KSVM_OP_LDBX: case KSVM_OP_LDBSX: case KSVM_OP_LDHX:
    case KSVM_OP_LDHSX: case KSVM_OP_LDWX:
        return SH_LDX;
    case KSVM_OP_STBX: case KSVM_OP_STHX: case KSVM_OP_STWX: return SH_STX;
    case KSVM_OP_LDB: case KSVM_OP_LDBS: case KSVM_OP_LDH:
    case KSVM_OP_LDHS: case KSVM_OP_LDW:
        return SH_LD;
    case KSVM_OP_STB: case KSVM_OP_STH: case KSVM_OP_STW: return SH_ST;
    case KSVM_OP_MOVI:                      return SH_D_IMM;
    case KSVM_OP_MFSR:                      return SH_D_UIMM;
    case KSVM_OP_CMPI: case KSVM_OP_TSTI:   return SH_A_IMM;
    case KSVM_OP_ADR:                       return SH_D_PCREL;
    case KSVM_OP_MTSR:                      return SH_SR_B;
    case KSVM_OP_TRAP:                      return SH_IMM;
    case KSVM_OP_SYSCALL: case KSVM_OP_PUSHM: case KSVM_OP_POPM: return SH_UIMM;
    case KSVM_OP_MOVIL:                     return SH_D_IMM32;
    case KSVM_OP_CMPL:                      return SH_A_IMM32;
    case KSVM_OP_ADRL:                      return SH_D_PCREL32;
    default:                                return default_shape(format);
    }
}

/* Bits da primeira palavra que o shape usa; os demais devem ser zero. */
#define M_OP   UINT32_C(0x000000FF)
#define M_RA   UINT32_C(0x00001F00)
#define M_RB   UINT32_C(0x0003E000)
#define M_RC   UINT32_C(0x007C0000)
#define M_IMM  UINT32_C(0xFFFC0000)
#define M_ALL  UINT32_C(0xFFFFFFFF)

static uint32_t shape_mask(shape s)
{
    switch (s) {
    case SH_NONE:                                   return M_OP;
    case SH_D_A_B: case SH_LDX: case SH_STX:        return M_OP | M_RA | M_RB | M_RC;
    case SH_D_B: case SH_A_B: case SH_D_A_IMM32:    return M_OP | M_RA | M_RB;
    case SH_A: case SH_D_IMM32: case SH_A_IMM32:
    case SH_D_PCREL32:
        return M_OP | M_RA;
    case SH_B:                                      return M_OP | M_RB;
    case SH_D_A_IMM: case SH_LD: case SH_ST:        return M_OP | M_RA | M_RB | M_IMM;
    case SH_D_IMM: case SH_D_UIMM: case SH_A_IMM:
    case SH_D_PCREL:
        return M_OP | M_RA | M_IMM;
    case SH_SR_B:                                   return M_OP | M_RB | M_IMM;
    case SH_IMM: case SH_UIMM:                      return M_OP | M_IMM;
    default:                                        return M_ALL;  /* B, J */
    }
}

/* Instrução decodificada + como mostrá-la. */
typedef struct view {
    ksvm_insn in;
    shape     sh;
    bool      canonical;   /* false: mostrar como .word */
} view;

static void make_view(uint32_t w, uint32_t imm32, view *v)
{
    bool valid = ksvm_decode(w, imm32, &v->in) && op_name(v->in.op) != NULL;

    v->sh = op_shape(v->in.op, v->in.format);
    v->canonical = valid && (w & ~shape_mask(v->sh)) == 0u;
    if (v->sh == SH_BRANCH && cond_mnemonic(v->in.cond) == NULL) {
        v->canonical = false;
    }
    /* O ksas só aceita contagem 0..31 em SHLI/SHRI/SARI; as demais (a VM
     * usa imm & 31) saem como .word para o round-trip continuar exato. */
    if ((v->in.op == KSVM_OP_SHLI || v->in.op == KSVM_OP_SHRI || v->in.op == KSVM_OP_SARI) &&
        v->in.imm > 31u) {
        v->canonical = false;
    }
}

static bool is_pcrel(shape s)
{
    return s == SH_BRANCH || s == SH_JUMP || s == SH_D_PCREL || s == SH_D_PCREL32;
}

/* ---- Operandos -------------------------------------------------------- */

/* "[rb]", "[rb + 8]" ou "[rb - 8]". */
static void put_mem_imm(out_buf *o, uint8_t rb, uint32_t imm)
{
    put_c(o, '[');
    put_reg(o, rb);
    if (imm != 0u) {
        put_s(o, negative(imm) ? " - " : " + ");
        put_udec(o, negative(imm) ? 0u - imm : imm);
    }
    put_c(o, ']');
}

static void put_mem_reg(out_buf *o, uint8_t rb, uint8_t rc)
{
    put_c(o, '[');
    put_reg(o, rb);
    put_s(o, " + ");
    put_reg(o, rc);
    put_c(o, ']');
}

/* Alvo PC-relativo: nome do símbolo ou deslocamento com sinal. */
static void put_target(out_buf *o, uint32_t pc, const view *v,
                       ksvm_disasm_sym_fn sym, void *ctx)
{
    const char *name = NULL;

    if (sym != NULL) {
        name = sym(ctx, pc + v->in.len + v->in.imm);
    }
    if (name != NULL) {
        put_s(o, name);
    } else {
        put_rel(o, v->in.imm);
    }
}

static void put_reg_list(out_buf *o, uint8_t a, uint8_t b)
{
    put_reg(o, a);
    put_sep(o);
    put_reg(o, b);
}

static void put_operands_r(out_buf *o, const view *v)
{
    const ksvm_insn *in = &v->in;

    switch (v->sh) {
    case SH_D_A_B: put_reg_list(o, in->ra, in->rb); put_sep(o); put_reg(o, in->rc); break;
    case SH_D_B:
    case SH_A_B:   put_reg_list(o, in->ra, in->rb); break;
    case SH_A:     put_reg(o, in->ra); break;
    case SH_B:     put_reg(o, in->rb); break;
    case SH_LDX:   put_reg(o, in->ra); put_sep(o); put_mem_reg(o, in->rb, in->rc); break;
    case SH_STX:   put_mem_reg(o, in->rb, in->rc); put_sep(o); put_reg(o, in->ra); break;
    default:       break;                                   /* SH_NONE */
    }
}

static void put_operands(out_buf *o, const view *v)
{
    const ksvm_insn *in = &v->in;

    switch (v->sh) {
    case SH_D_A_IMM:   put_reg_list(o, in->ra, in->rb); put_sep(o); put_sdec(o, in->imm); break;
    case SH_LD:        put_reg(o, in->ra); put_sep(o); put_mem_imm(o, in->rb, in->imm); break;
    case SH_ST:        put_mem_imm(o, in->rb, in->imm); put_sep(o); put_reg(o, in->ra); break;
    case SH_D_IMM:
    case SH_A_IMM:     put_reg(o, in->ra); put_sep(o); put_sdec(o, in->imm); break;
    case SH_D_UIMM:    put_reg(o, in->ra); put_sep(o); put_udec(o, ksvm_field_uimm14(in->word)); break;
    case SH_SR_B:      put_udec(o, ksvm_field_uimm14(in->word)); put_sep(o); put_reg(o, in->rb); break;
    case SH_IMM:       put_sdec(o, in->imm); break;
    case SH_UIMM:      put_hex(o, ksvm_field_uimm14(in->word), 1u); break;
    case SH_D_A_IMM32: put_reg_list(o, in->ra, in->rb); put_sep(o); put_hex(o, in->imm, 1u); break;
    case SH_D_IMM32:
    case SH_A_IMM32:   put_reg(o, in->ra); put_sep(o); put_hex(o, in->imm, 1u); break;
    default:           put_operands_r(o, v); break;
    }
}

static void put_word_directive(out_buf *o, const view *v, uint32_t imm32)
{
    put_s(o, ".word ");
    put_hex(o, v->in.word, 8u);
    if (v->in.format == KSVM_FMT_L) {
        put_sep(o);
        put_hex(o, imm32, 8u);
    }
}

static void put_mnemonic(out_buf *o, const view *v)
{
    if (v->sh == SH_BRANCH) {
        put_s(o, cond_mnemonic(v->in.cond));
    } else {
        put_lower(o, op_name(v->in.op));
    }
}

/* ---- API -------------------------------------------------------------- */

unsigned ksvm_disasm_len(uint32_t w)
{
    ksvm_insn in;

    return (ksvm_decode(w, 0u, &in) && in.len == 8u) ? 8u : 4u;
}

unsigned ksvm_disasm_one_ex(uint32_t pc, uint32_t w, uint32_t imm32,
                            ksvm_disasm_sym_fn sym, void *ctx, char *buf, size_t n)
{
    out_buf o;
    view v;

    out_init(&o, buf, n);
    make_view(w, imm32, &v);
    if (!v.canonical) {
        put_word_directive(&o, &v, imm32);
        return (v.in.format == KSVM_FMT_L) ? 8u : 4u;
    }
    put_mnemonic(&o, &v);
    if (v.sh != SH_NONE) {
        put_c(&o, ' ');
    }
    if (v.sh == SH_D_PCREL || v.sh == SH_D_PCREL32) {
        put_reg(&o, v.in.ra);
        put_sep(&o);
    }
    if (is_pcrel(v.sh)) {
        put_target(&o, pc, &v, sym, ctx);
    } else {
        put_operands(&o, &v);
    }
    return v.in.len;
}

unsigned ksvm_disasm_one(uint32_t pc, uint32_t w, uint32_t imm32, char *buf, size_t n)
{
    return ksvm_disasm_one_ex(pc, w, imm32, NULL, NULL, buf, n);
}

bool ksvm_disasm_target(uint32_t pc, uint32_t w, uint32_t imm32, uint32_t *target)
{
    view v;

    make_view(w, imm32, &v);
    if (!v.canonical || !is_pcrel(v.sh)) {
        return false;
    }
    if (target != NULL) {
        *target = pc + v.in.len + v.in.imm;
    }
    return true;
}

#else  /* !KSVM_ENABLE_DISASM */

/* ISO C proíbe unidade de tradução vazia. */
typedef int ksvm_disasm_disabled;

#endif /* KSVM_ENABLE_DISASM */
