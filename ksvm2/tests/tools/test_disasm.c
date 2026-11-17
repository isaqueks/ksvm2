/*
 * test_disasm.c — ksvm_disasm_one e afins: string exata para cada opcode do
 * isa.def (codificado com ksvm_enc_*), condições, formas não canônicas,
 * truncamento do buffer, resolvedor de símbolos e alvos.
 */
#include <string.h>

#include "ksvm/disasm.h"
#include "ksvm/encode.h"
#include "ksvm/isa.h"
#include "test_tools.h"

#define PC UINT32_C(0x100)

#define R(op, a, b, c)   ksvm_enc_r(KSVM_OP_##op, (a), (b), (c))
#define I(op, a, b, imm) ksvm_enc_i(KSVM_OP_##op, (a), (b), (imm))
#define L(op, a, b)      ksvm_enc_l(KSVM_OP_##op, (a), (b))
#define J(op, off)       ksvm_enc_j(KSVM_OP_##op, (off))
#define B(cond, off)     ksvm_enc_b(KSVM_COND_##cond, (off))

typedef struct dcase {
    uint32_t    w;
    uint32_t    imm32;
    const char *text;
} dcase;

/* Desmonta em PC e compara texto e tamanho; nota o caso em falha. */
static void check_case(test_ctx *t_, const dcase *c)
{
    char buf[KSVM_DISASM_BUF];
    unsigned len = ksvm_disasm_one(PC, c->w, c->imm32, buf, sizeof buf);
    ksvm_insn in;
    unsigned want = (ksvm_decode(c->w, 0u, &in) && in.len == 8u) ? 8u : 4u;

    if (strcmp(buf, c->text) != 0) {
        test_note(t_, "w=0x%08lX: \"%s\", esperado \"%s\"", (unsigned long)c->w, buf, c->text);
    }
    ASSERT_TRUE(strcmp(buf, c->text) == 0);
    ASSERT_EQ(len, want);
}

TEST(every_opcode)
{
    const dcase cases[] = {
        { R(NOP, 0, 0, 0), 0, "nop" },
        { R(MOV, 1, 2, 0), 0, "mov r1, r2" },
        { I(MOVI, 3, 0, -5), 0, "movi r3, -5" },
        { L(MOVIL, 4, 0), UINT32_C(0x12345678), "movil r4, 0x12345678" },
        { I(ADR, 6, 0, 16), 0, "adr r6, +0x10" },
        { L(ADRL, 7, 0), UINT32_C(0xFFFFFFF0), "adrl r7, -0x10" },
        { I(MFSR, 1, 0, 3), 0, "mfsr r1, 3" },
        { I(MTSR, 0, 2, 1), 0, "mtsr 1, r2" },
        { I(LDB, 1, 2, 0), 0, "ldb r1, [r2]" },
        { I(LDBS, 1, 2, -1), 0, "ldbs r1, [r2 - 1]" },
        { I(LDH, 1, 31, 8), 0, "ldh r1, [sp + 8]" },
        { I(LDHS, 1, 30, -8), 0, "ldhs r1, [fp - 8]" },
        { I(LDW, 3, 2, 8), 0, "ldw r3, [r2 + 8]" },
        { I(STB, 1, 2, 0), 0, "stb [r2], r1" },
        { I(STH, 1, 2, 2), 0, "sth [r2 + 2], r1" },
        { I(STW, 1, 31, -4), 0, "stw [sp - 4], r1" },
        { R(LDBX, 1, 2, 3), 0, "ldbx r1, [r2 + r3]" },
        { R(LDBSX, 4, 5, 6), 0, "ldbsx r4, [r5 + r6]" },
        { R(LDHX, 1, 2, 3), 0, "ldhx r1, [r2 + r3]" },
        { R(LDHSX, 1, 2, 3), 0, "ldhsx r1, [r2 + r3]" },
        { R(LDWX, 1, 0, 3), 0, "ldwx r1, [zr + r3]" },
        { R(STBX, 1, 2, 3), 0, "stbx [r2 + r3], r1" },
        { R(STHX, 1, 2, 3), 0, "sthx [r2 + r3], r1" },
        { R(STWX, 9, 10, 11), 0, "stwx [r10 + r11], r9" },
        { R(PUSH, 30, 0, 0), 0, "push fp" },
        { R(POP, 29, 0, 0), 0, "pop r29" },
        { I(PUSHM, 0, 0, -1), 0, "pushm 0x3FFF" },
        { I(POPM, 0, 0, 5), 0, "popm 0x5" },
        { R(ADD, 1, 2, 3), 0, "add r1, r2, r3" },
        { I(ADDI, 2, 1, 1), 0, "addi r2, r1, 1" },
        { L(ADDL, 1, 2), UINT32_C(100000), "addl r1, r2, 0x186A0" },
        { R(SUB, 1, 2, 3), 0, "sub r1, r2, r3" },
        { I(SUBI, 1, 2, -8192), 0, "subi r1, r2, -8192" },
        { R(ADC, 1, 2, 3), 0, "adc r1, r2, r3" },
        { R(SBC, 1, 2, 3), 0, "sbc r1, r2, r3" },
        { R(NEG, 1, 2, 0), 0, "neg r1, r2" },
        { R(CMP, 1, 2, 0), 0, "cmp r1, r2" },
        { I(CMPI, 1, 0, 8191), 0, "cmpi r1, 8191" },
        { L(CMPL, 1, 0), UINT32_C(0xFFFFFFFF), "cmpl r1, 0xFFFFFFFF" },
        { R(MUL, 1, 2, 3), 0, "mul r1, r2, r3" },
        { I(MULI, 1, 2, -3), 0, "muli r1, r2, -3" },
        { R(MULH, 1, 2, 3), 0, "mulh r1, r2, r3" },
        { R(MULHU, 1, 2, 3), 0, "mulhu r1, r2, r3" },
        { R(DIV, 1, 2, 3), 0, "div r1, r2, r3" },
        { R(DIVU, 1, 2, 3), 0, "divu r1, r2, r3" },
        { R(REM, 1, 2, 3), 0, "rem r1, r2, r3" },
        { R(REMU, 1, 2, 3), 0, "remu r1, r2, r3" },
        { R(AND, 1, 2, 3), 0, "and r1, r2, r3" },
        { I(ANDI, 1, 2, 255), 0, "andi r1, r2, 255" },
        { L(ANDL, 1, 2), UINT32_C(0xFF00FF00), "andl r1, r2, 0xFF00FF00" },
        { R(OR, 1, 2, 3), 0, "or r1, r2, r3" },
        { I(ORI, 1, 2, 1), 0, "ori r1, r2, 1" },
        { L(ORL, 1, 2), UINT32_C(0x80000000), "orl r1, r2, 0x80000000" },
        { R(XOR, 1, 2, 3), 0, "xor r1, r2, r3" },
        { I(XORI, 1, 2, -1), 0, "xori r1, r2, -1" },
        { L(XORL, 1, 2), 0, "xorl r1, r2, 0x0" },
        { R(NOT, 1, 2, 0), 0, "not r1, r2" },
        { R(SHL, 1, 2, 3), 0, "shl r1, r2, r3" },
        { I(SHLI, 1, 2, 31), 0, "shli r1, r2, 31" },
        { R(SHR, 1, 2, 3), 0, "shr r1, r2, r3" },
        { I(SHRI, 1, 2, 4), 0, "shri r1, r2, 4" },
        { R(SAR, 1, 2, 3), 0, "sar r1, r2, r3" },
        { I(SARI, 1, 2, 1), 0, "sari r1, r2, 1" },
        { R(ROL, 1, 2, 3), 0, "rol r1, r2, r3" },
        { R(ROR, 1, 2, 3), 0, "ror r1, r2, r3" },
        { R(TST, 1, 2, 0), 0, "tst r1, r2" },
        { I(TSTI, 1, 0, 255), 0, "tsti r1, 255" },
        { R(SXB, 1, 2, 0), 0, "sxb r1, r2" },
        { R(SXH, 1, 2, 0), 0, "sxh r1, r2" },
        { R(ZXB, 1, 2, 0), 0, "zxb r1, r2" },
        { R(ZXH, 1, 2, 0), 0, "zxh r1, r2" },
        { R(CLZ, 1, 2, 0), 0, "clz r1, r2" },
        { R(CTZ, 1, 2, 0), 0, "ctz r1, r2" },
        { R(POPCNT, 1, 2, 0), 0, "popcnt r1, r2" },
        { J(JMP, 0x40), 0, "jmp +0x40" },
        { R(JMPR, 0, 5, 0), 0, "jmpr r5" },
        { B(EQ, 0x40), 0, "beq +0x40" },
        { J(CALL, -8), 0, "call -0x8" },
        { R(CALLR, 0, 7, 0), 0, "callr r7" },
        { R(RET, 0, 0, 0), 0, "ret" },
        { R(HALT, 0, 0, 0), 0, "halt" },
        { I(TRAP, 0, 0, -1), 0, "trap -1" },
        { I(SYSCALL, 0, 0, 0x10), 0, "syscall 0x10" },
        { R(BRK, 0, 0, 0), 0, "brk" },
        { R(WFI, 0, 0, 0), 0, "wfi" },
    };
    bool seen[256];
    size_t i;

    memset(seen, 0, sizeof seen);
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        check_case(t_, &cases[i]);
        if (TEST_FAILED()) {
            return;
        }
        seen[cases[i].w & 0xFFu] = true;
    }
    /* Todo opcode do isa.def tem caso acima. */
#define KSVM_OP(name, code, fmt, fl, desc)                                \
    if (!seen[code]) {                                                    \
        test_note(t_, "opcode %s sem caso de teste", #name);              \
    }                                                                     \
    ASSERT_TRUE(seen[code]);
#include "ksvm/isa.def"
#undef KSVM_OP
}

TEST(register_names)
{
    const dcase cases[] = {
        { R(ADD, 0, 0, 0), 0, "add zr, zr, zr" },
        { R(ADD, 29, 30, 31), 0, "add r29, fp, sp" },
        { I(ADDI, 31, 31, -16), 0, "addi sp, sp, -16" },
        { I(LDW, 1, 0, 100), 0, "ldw r1, [zr + 100]" },
        { I(LDW, 1, 2, -8192), 0, "ldw r1, [r2 - 8192]" },
        { I(SYSCALL, 0, 0, 0x3FFF), 0, "syscall 0x3FFF" },
        { I(SYSCALL, 0, 0, 0), 0, "syscall 0x0" },
        { L(MOVIL, 2, 0), 0x30, "movil r2, 0x30" },
        /* Número de SR: 14 bits sem sinal, como o ksas exige (0..16383). */
        { I(MFSR, 1, 0, -1), 0, "mfsr r1, 16383" },
        { I(MFSR, 1, 0, 9000), 0, "mfsr r1, 9000" },
        { I(MTSR, 0, 2, -8192), 0, "mtsr 8192, r2" },
        { I(SARI, 1, 2, 31), 0, "sari r1, r2, 31" },
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0] && !TEST_FAILED(); i++) {
        check_case(t_, &cases[i]);
    }
}

TEST(all_conditions)
{
    static const char *const names[15] = {
        "bal", "beq", "bne", "bhs", "blo", "bmi", "bpl", "bvs",
        "bvc", "bhi", "bls", "bge", "blt", "bgt", "ble"
    };
    char buf[KSVM_DISASM_BUF], want[32];
    uint8_t c;

    for (c = 0; c < 15u; c++) {
        (void)ksvm_disasm_one(PC, ksvm_enc_b(c, -4), 0, buf, sizeof buf);
        strcpy(want, names[c]);
        strcat(want, " -0x4");
        ASSERT_TRUE(strcmp(buf, want) == 0);
    }
    /* NV não tem mnemônico no ksas: sai como .word. */
    (void)ksvm_disasm_one(PC, ksvm_enc_b(KSVM_COND_NV, 8), 0, buf, sizeof buf);
    ASSERT_TRUE(strcmp(buf, ".word 0x00002F72") == 0);
}

TEST(branch_extremes)
{
    const dcase cases[] = {
        { B(AL, -(INT32_C(1) << 21)), 0, "bal -0x200000" },
        { B(NE, (INT32_C(1) << 21) - 4), 0, "bne +0x1FFFFC" },
        { J(JMP, -(INT32_C(1) << 25)), 0, "jmp -0x2000000" },
        { J(CALL, 0), 0, "call +0x0" },
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0] && !TEST_FAILED(); i++) {
        check_case(t_, &cases[i]);
    }
}

TEST(non_canonical_as_word)
{
    const dcase cases[] = {
        { 0xFFu, 0, ".word 0x000000FF" },                         /* opcode inválido */
        { UINT32_C(0xFFFFFFFF), 0, ".word 0xFFFFFFFF" },
        { 0x09u, 0, ".word 0x00000009" },
        { R(NOP, 1, 0, 0), 0, ".word 0x00000100" },               /* campo não usado */
        { R(ADD, 1, 2, 3) | (UINT32_C(1) << 23), 0, ".word 0x008C4130" },  /* func9 */
        { R(MOV, 1, 2, 3), 0, ".word 0x000C4101" },               /* rc em MOV */
        { R(PUSH, 1, 1, 0), 0, ".word 0x00002120" },
        { I(MOVI, 1, 1, 0), 0, ".word 0x00002102" },              /* rb em MOVI */
        { I(SYSCALL, 1, 0, 0x10), 0, ".word 0x00400178" },
        /* Contagem fora de 0..31: o ksas recusaria "shli r1, r2, 40". */
        { I(SHLI, 1, 2, 40), 0, ".word 0x00A0415B" },
        { I(SHRI, 1, 2, -1), 0, ".word 0xFFFC415D" },
        { I(SARI, 1, 2, 32), 0, ".word 0x0080415F" },
    };
    char buf[KSVM_DISASM_BUF];
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0] && !TEST_FAILED(); i++) {
        check_case(t_, &cases[i]);
    }
    /* Formato L com bits reservados: as duas palavras, tamanho 8. */
    ASSERT_EQ(ksvm_disasm_one(PC, L(MOVIL, 1, 0) | (UINT32_C(1) << 18), 5, buf, sizeof buf), 8);
    ASSERT_TRUE(strcmp(buf, ".word 0x00040103, 0x00000005") == 0);
}

TEST(buffer_truncation)
{
    char buf[16];
    uint32_t w = I(ADDI, 2, 1, 1);

    memset(buf, 'x', sizeof buf);
    ASSERT_EQ(ksvm_disasm_one(PC, w, 0, buf, 0), 4);
    ASSERT_EQ(buf[0], 'x');                                       /* n = 0: intocado */
    ASSERT_EQ(ksvm_disasm_one(PC, w, 0, buf, 1), 4);
    ASSERT_EQ(buf[0], '\0');
    ASSERT_EQ(ksvm_disasm_one(PC, w, 0, buf, 5), 4);
    ASSERT_TRUE(strcmp(buf, "addi") == 0);
    ASSERT_EQ(buf[5], 'x');
    ASSERT_EQ(ksvm_disasm_one(PC, w, 0, NULL, 10), 4);            /* NULL: só o tamanho */
}

static const char *sym_print(void *ctx, uint32_t addr)
{
    (void)ctx;
    return (addr == UINT32_C(0x140)) ? "print" : NULL;
}

TEST(symbol_resolver)
{
    char buf[KSVM_DISASM_BUF];

    (void)ksvm_disasm_one_ex(PC, J(CALL, 0x3C), 0, sym_print, NULL, buf, sizeof buf);
    ASSERT_TRUE(strcmp(buf, "call print") == 0);
    (void)ksvm_disasm_one_ex(PC, B(LT, 0x3C), 0, sym_print, NULL, buf, sizeof buf);
    ASSERT_TRUE(strcmp(buf, "blt print") == 0);
    (void)ksvm_disasm_one_ex(PC, I(ADR, 3, 0, 0x3C), 0, sym_print, NULL, buf, sizeof buf);
    ASSERT_TRUE(strcmp(buf, "adr r3, print") == 0);
    (void)ksvm_disasm_one_ex(PC, L(ADRL, 3, 0), 0x38, sym_print, NULL, buf, sizeof buf);
    ASSERT_TRUE(strcmp(buf, "adrl r3, print") == 0);
    (void)ksvm_disasm_one_ex(PC, J(JMP, 0x40), 0, sym_print, NULL, buf, sizeof buf);
    ASSERT_TRUE(strcmp(buf, "jmp +0x40") == 0);                  /* sem símbolo */
    /* Imediatos absolutos não passam pelo resolvedor. */
    (void)ksvm_disasm_one_ex(PC, L(MOVIL, 1, 0), 0x140, sym_print, NULL, buf, sizeof buf);
    ASSERT_TRUE(strcmp(buf, "movil r1, 0x140") == 0);
}

TEST(targets_and_len)
{
    uint32_t t = 0;

    ASSERT_TRUE(ksvm_disasm_target(PC, B(EQ, 0x40), 0, &t));
    ASSERT_EQ_U32(t, 0x144);
    ASSERT_TRUE(ksvm_disasm_target(PC, J(CALL, -8), 0, &t));
    ASSERT_EQ_U32(t, 0xFC);
    ASSERT_TRUE(ksvm_disasm_target(PC, I(ADR, 1, 0, 8), 0, &t));
    ASSERT_EQ_U32(t, 0x10C);
    ASSERT_TRUE(ksvm_disasm_target(PC, L(ADRL, 1, 0), 0x10, &t));
    ASSERT_EQ_U32(t, 0x118);
    ASSERT_TRUE(ksvm_disasm_target(0, J(JMP, -4), 0, &t));
    ASSERT_EQ_U32(t, 0);
    ASSERT_TRUE(ksvm_disasm_target(PC, J(JMP, 0), 0, NULL));
    ASSERT_FALSE(ksvm_disasm_target(PC, R(ADD, 1, 2, 3), 0, &t));
    ASSERT_FALSE(ksvm_disasm_target(PC, ksvm_enc_b(KSVM_COND_NV, 8), 0, &t));
    ASSERT_FALSE(ksvm_disasm_target(PC, 0xFFu, 0, &t));
    ASSERT_EQ(ksvm_disasm_len(L(MOVIL, 1, 0)), 8);
    ASSERT_EQ(ksvm_disasm_len(R(ADD, 1, 2, 3)), 4);
    ASSERT_EQ(ksvm_disasm_len(0xFFu), 4);
}

SUITE(disasm)
{
    RUN_TEST(every_opcode);
    RUN_TEST(register_names);
    RUN_TEST(all_conditions);
    RUN_TEST(branch_extremes);
    RUN_TEST(non_canonical_as_word);
    RUN_TEST(buffer_truncation);
    RUN_TEST(symbol_resolver);
    RUN_TEST(targets_and_len);
}
