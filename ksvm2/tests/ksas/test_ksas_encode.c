/*
 * test_ksas_encode.c — cada formato (R, I, L, B, J) e cada forma de
 * operando conferidos contra os codificadores do núcleo (ksvm_enc_*).
 */
#include <stdio.h>

#include "ksas_test.h"
#include "mnemonic.h"

/* Monta src e confere as palavras de código a partir do endereço 0. */
static bool code_is(test_ctx *t_, const char *src, const uint32_t *words, size_t n)
{
    asm_out o;
    size_t i;
    bool ok;

    if (ta_assemble(&o, src) != 0) {
        test_note(t_, "\"%s\" nao montou:\n%s", src, o.diags);
        ta_free(&o);
        return false;
    }
    ok = (o.hdr.code_size == n * 4u);
    for (i = 0; ok && i < n; i++) {
        ok = (ta_code32(&o, (uint32_t)(i * 4u)) == words[i]);
    }
    if (!ok) {
        test_note(t_, "\"%s\": code_size %lu, palavra[%lu] = 0x%08lX, esperado 0x%08lX",
                  src, (unsigned long)o.hdr.code_size, (unsigned long)(i - 1u),
                  (unsigned long)ta_code32(&o, (uint32_t)((i - 1u) * 4u)),
                  (unsigned long)words[i - 1u]);
    }
    ta_free(&o);
    return ok;
}

#define CODE_IS(src, ...)                                                       \
    do {                                                                        \
        const uint32_t w_[] = { __VA_ARGS__ };                                  \
        ASSERT_TRUE(code_is(t_, (src), w_, sizeof w_ / sizeof w_[0]));          \
    } while (0)

#define R(op, a, b, c)   ksvm_enc_r(KSVM_OP_##op, (a), (b), (c))
#define I(op, a, b, imm) ksvm_enc_i(KSVM_OP_##op, (a), (b), (imm))
#define L(op, a, b, imm) ksvm_enc_l(KSVM_OP_##op, (a), (b)), (uint32_t)(imm)

/* ---- Casos escritos à mão (mapeamento do topo de isa.def) ------------- */

TEST(format_r)
{
    CODE_IS("add r1, r2, r3", R(ADD, 1, 2, 3));
    CODE_IS("SUB sp, fp, r29", R(SUB, 31, 30, 29));
    CODE_IS("mulhu a0, t0, s0", R(MULHU, 1, 5, 16));
    CODE_IS("mov r1, r2", R(MOV, 1, 2, 0));
    CODE_IS("neg r3, r4", R(NEG, 3, 4, 0));
    CODE_IS("cmp r5, r6", R(CMP, 5, 6, 0));
    CODE_IS("tst r7, r8", R(TST, 7, 8, 0));
    CODE_IS("popcnt r9, r10", R(POPCNT, 9, 10, 0));
    CODE_IS("push r7", R(PUSH, 7, 0, 0));
    CODE_IS("pop r8", R(POP, 8, 0, 0));
    CODE_IS("jmpr r9", R(JMPR, 0, 9, 0));
    CODE_IS("callr r10", R(CALLR, 0, 10, 0));
    CODE_IS("nop\nret\nhalt\nbrk\nwfi", R(NOP, 0, 0, 0), R(RET, 0, 0, 0), R(HALT, 0, 0, 0),
            R(BRK, 0, 0, 0), R(WFI, 0, 0, 0));
}

TEST(format_i)
{
    CODE_IS("addi r1, r2, -5", I(ADDI, 1, 2, -5));
    CODE_IS("movi r3, 8191", I(MOVI, 3, 0, 8191));
    CODE_IS("movi r3, -8192", I(MOVI, 3, 0, -8192));
    CODE_IS("cmpi r4, -1", I(CMPI, 4, 0, -1));
    CODE_IS("tsti r5, 7", I(TSTI, 5, 0, 7));
    CODE_IS("shli r1, r2, 31", I(SHLI, 1, 2, 31));
    CODE_IS("muli r1, r2, -100", I(MULI, 1, 2, -100));
    CODE_IS("trap 99", I(TRAP, 0, 0, 99));
    CODE_IS("trap -1", I(TRAP, 0, 0, -1));
    CODE_IS("syscall 0x3FFF", I(SYSCALL, 0, 0, 0x3FFF));
    CODE_IS("mfsr r1, 2", I(MFSR, 1, 0, 2));
    CODE_IS("mtsr 1, r2", I(MTSR, 0, 2, 1));
    CODE_IS("pushm 0x10\npopm 0x10", I(PUSHM, 0, 0, 0x10), I(POPM, 0, 0, 0x10));
}

TEST(format_l)
{
    CODE_IS("movil r1, 0x12345678", L(MOVIL, 1, 0, 0x12345678));
    CODE_IS("addl r1, r2, -100000", L(ADDL, 1, 2, -100000));
    CODE_IS("cmpl r3, 5", L(CMPL, 3, 0, 5));              /* L explícito fica L */
    CODE_IS("andl r1, r2, 0xFF00FF00", L(ANDL, 1, 2, 0xFF00FF00u));
    CODE_IS("orl r1, r2, 1\nxorl r3, r4, -1", L(ORL, 1, 2, 1), L(XORL, 3, 4, -1));
}

TEST(memory_operands)
{
    CODE_IS("ldb r1, [r2]", I(LDB, 1, 2, 0));
    CODE_IS("ldw r1, [r2 + 8]", I(LDW, 1, 2, 8));
    CODE_IS("ldh r1, [r2 - 8]", I(LDH, 1, 2, -8));
    CODE_IS("ldhs r1, [sp + 4 * 3]", I(LDHS, 1, 31, 12));
    CODE_IS("ldbs r1, [r2 - 4 + 2]", I(LDBS, 1, 2, -2));   /* sinal entra como unário */
    CODE_IS("stb [r2 + 1], r3", I(STB, 3, 2, 1));
    CODE_IS("sth [r2], r3", I(STH, 3, 2, 0));
    CODE_IS("stw r3, [r2 + 4]", I(STW, 3, 2, 4));           /* ordem alternativa */
    CODE_IS("ldb r1, [r2 + r3]", R(LDBX, 1, 2, 3));         /* vira a forma X */
    CODE_IS("ldwx r1, [r2 + r3]", R(LDWX, 1, 2, 3));
    CODE_IS("stw [r2 + r3], r4", R(STWX, 4, 2, 3));
    CODE_IS("sthx r4, [r2 + r3]", R(STHX, 4, 2, 3));
    CODE_IS("ldw r1, [0x100]", I(LDW, 1, 0, 0x100));        /* [abs] = [zr + abs] */
    CODE_IS("ldw r1, [val]\nval = 8191", I(LDW, 1, 0, 8191));
}

TEST(pc_relative_adr)
{
    /* ADR/ADRL recebem o ALVO; o campo guarda alvo - PC_next. */
    CODE_IS("adr r1, tgt\nnop\ntgt: nop", I(ADR, 1, 0, 4), R(NOP, 0, 0, 0), R(NOP, 0, 0, 0));
    CODE_IS("back: nop\nadrl r2, back", R(NOP, 0, 0, 0), L(ADRL, 2, 0, -12));
}

TEST(branches_all_conditions)
{
    static const char *const names[16] = {
        "al", "eq", "ne", "hs", "lo", "mi", "pl", "vs",
        "vc", "hi", "ls", "ge", "lt", "gt", "le", "nv"
    };
    char src[64];
    unsigned i;

    for (i = 0; i < 16u; i++) {
        snprintf(src, sizeof src, "x: b%s x", names[i]);
        CODE_IS(src, ksvm_enc_b((uint8_t)i, -4));
        snprintf(src, sizeof src, "nop\nB.%s y\ny: nop", names[i]);    /* para frente */
        CODE_IS(src, R(NOP, 0, 0, 0), ksvm_enc_b((uint8_t)i, 0), R(NOP, 0, 0, 0));
    }
    CODE_IS("x: bcs x\nbcc x", ksvm_enc_b(KSVM_COND_HS, -4), ksvm_enc_b(KSVM_COND_LO, -8));
    CODE_IS("x: b x", ksvm_enc_b(KSVM_COND_AL, -4));
}

TEST(jumps_and_calls)
{
    CODE_IS("jmp fwd\nnop\nfwd: call back\nback: ret",
            ksvm_enc_j(KSVM_OP_JMP, 4), R(NOP, 0, 0, 0), ksvm_enc_j(KSVM_OP_CALL, 0),
            R(RET, 0, 0, 0));
    CODE_IS("x: nop\nj x", R(NOP, 0, 0, 0), ksvm_enc_j(KSVM_OP_JMP, -8));
}

TEST(branch_limits)
{
    /* off20 = +-2 MiB: o maior desvio para trás ainda cabe. */
    asm_out o;

    ASM_OK(&o, "x: .space 0x200000 - 4\n"
               "   bal x\n");
    ASSERT_EQ_U32(ta_code32(&o, 0x200000 - 4), ksvm_enc_b(KSVM_COND_AL, -0x200000));
    ta_free(&o);
}

/* ---- Varredura de toda a tabela da ISA -------------------------------- */

static const struct { const char *name; uint8_t op; } isa_ops[] = {
#define KSVM_OP(name, code, fmt, fl, desc) { #name, code },
#include "ksvm/isa.def"
#undef KSVM_OP
};

/* Fonte e palavras esperadas para um opcode, pela forma de operandos. */
static size_t sweep_case(uint8_t op, const char *name, char *src, size_t cap, uint32_t *w)
{
    bool l = ksvm_isa[op].format == KSVM_FMT_L;

    switch (ksas_op_shape(op)) {
    case KSAS_SH_NONE:  snprintf(src, cap, "%s", name);              w[0] = ksvm_enc_r(op, 0, 0, 0);   return 1;
    case KSAS_SH_R3:    snprintf(src, cap, "%s r1, r2, r3", name);   w[0] = ksvm_enc_r(op, 1, 2, 3);   return 1;
    case KSAS_SH_R2:    snprintf(src, cap, "%s r4, r5", name);       w[0] = ksvm_enc_r(op, 4, 5, 0);   return 1;
    case KSAS_SH_RA:    snprintf(src, cap, "%s r6", name);           w[0] = ksvm_enc_r(op, 6, 0, 0);   return 1;
    case KSAS_SH_RB:    snprintf(src, cap, "%s r7", name);           w[0] = ksvm_enc_r(op, 0, 7, 0);   return 1;
    case KSAS_SH_SR_RB: snprintf(src, cap, "%s 1, r11", name);       w[0] = ksvm_enc_i(op, 0, 11, 1);  return 1;
    case KSAS_SH_IMM:   snprintf(src, cap, "%s 5", name);            w[0] = ksvm_enc_i(op, 0, 0, 5);   return 1;
    case KSAS_SH_JUMP:  snprintf(src, cap, "x: %s x", name);         w[0] = ksvm_enc_j(op, -4);        return 1;
    case KSAS_SH_LOAD:
        if (ksvm_isa[op].format == KSVM_FMT_R) {
            snprintf(src, cap, "%s r12, [r13 + r14]", name);  w[0] = ksvm_enc_r(op, 12, 13, 14);
        } else {
            snprintf(src, cap, "%s r12, [r13 + 4]", name);    w[0] = ksvm_enc_i(op, 12, 13, 4);
        }
        return 1;
    case KSAS_SH_STORE:
        if (ksvm_isa[op].format == KSVM_FMT_R) {
            snprintf(src, cap, "%s [r13 + r14], r12", name);  w[0] = ksvm_enc_r(op, 12, 13, 14);
        } else {
            snprintf(src, cap, "%s [r13 + 4], r12", name);    w[0] = ksvm_enc_i(op, 12, 13, 4);
        }
        return 1;
    case KSAS_SH_RRI:
        snprintf(src, cap, "%s r8, r9, %s", name, l ? "0x12345678" : "7");
        w[0] = l ? ksvm_enc_l(op, 8, 9) : ksvm_enc_i(op, 8, 9, 7);
        w[1] = 0x12345678u;
        return l ? 2u : 1u;
    case KSAS_SH_RI:
        if (op == KSVM_OP_ADR || op == KSVM_OP_ADRL) {
            return 0;                                      /* pc_relative_adr */
        }
        snprintf(src, cap, "%s r10, %s", name, l ? "0x89ABCDEF" : "3");
        w[0] = l ? ksvm_enc_l(op, 10, 0) : ksvm_enc_i(op, 10, 0, 3);
        w[1] = 0x89ABCDEFu;
        return l ? 2u : 1u;
    default:
        return 0;                                          /* BCOND: branches_all_conditions */
    }
}

TEST(every_isa_mnemonic)
{
    char src[96];
    uint32_t w[2];
    size_t i, n, tested = 0;

    for (i = 0; i < sizeof isa_ops / sizeof isa_ops[0]; i++) {
        n = sweep_case(isa_ops[i].op, isa_ops[i].name, src, sizeof src, w);
        if (n == 0u) {
            continue;
        }
        if (!code_is(t_, src, w, n)) {
            test_fail(t_, __FILE__, __LINE__, isa_ops[i].name);
            return;
        }
        tested++;
    }
    ASSERT_TRUE(tested + 3u == sizeof isa_ops / sizeof isa_ops[0]);   /* ADR, ADRL, BCOND */
}

SUITE(ksas_encode)
{
    RUN_TEST(format_r);
    RUN_TEST(format_i);
    RUN_TEST(format_l);
    RUN_TEST(memory_operands);
    RUN_TEST(pc_relative_adr);
    RUN_TEST(branches_all_conditions);
    RUN_TEST(jumps_and_calls);
    RUN_TEST(branch_limits);
    RUN_TEST(every_isa_mnemonic);
}
