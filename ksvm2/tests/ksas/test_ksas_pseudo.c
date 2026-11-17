/*
 * test_ksas_pseudo.c — pseudo-instruções (doc 04 §7, doc 07) e a escolha
 * entre forma curta (I, 4 bytes) e longa (L, 8 bytes).
 */
#include "ksas_test.h"

#define R(op, a, b, c)   ksvm_enc_r(KSVM_OP_##op, (a), (b), (c))
#define I(op, a, b, imm) ksvm_enc_i(KSVM_OP_##op, (a), (b), (imm))
#define LW(op, a, b)     ksvm_enc_l(KSVM_OP_##op, (a), (b))

/* Confere a palavra em addr e (opcional) o tamanho do código. */
#define WORD_AT(o, addr, w) ASSERT_EQ_U32(ta_code32((o), (addr)), (w))

TEST(li_picks_short_or_long)
{
    asm_out o;

    ASM_OK(&o, "li r1, 0\n"            /* 0: MOVI */
               "li r2, -8192\n"        /* 4: MOVI */
               "li r3, 8191\n"         /* 8: MOVI */
               "li r4, 8192\n"         /* 12: MOVIL (8) */
               "li r5, 0xFFFFFFFF\n"   /* 20: -1 cabe: MOVI */
               "li r6, -8193\n"        /* 24: MOVIL */
               "halt\n");              /* 32 */
    WORD_AT(&o, 0, I(MOVI, 1, 0, 0));
    WORD_AT(&o, 4, I(MOVI, 2, 0, -8192));
    WORD_AT(&o, 8, I(MOVI, 3, 0, 8191));
    WORD_AT(&o, 12, LW(MOVIL, 4, 0));
    WORD_AT(&o, 16, 8192);
    WORD_AT(&o, 20, I(MOVI, 5, 0, -1));
    WORD_AT(&o, 24, LW(MOVIL, 6, 0));
    WORD_AT(&o, 28, (uint32_t)-8193);
    WORD_AT(&o, 32, R(HALT, 0, 0, 0));
    ta_free(&o);
}

TEST(li_with_labels)
{
    asm_out o;

    /* Label de .text já visto: valor conhecido no passo 1 -> MOVI.
     * Label à frente ou de .data: desconhecido -> MOVIL, mesmo pequeno. */
    ASM_OK(&o, "back: nop\n"            /* 0 */
               "li r1, back\n"          /* 4: MOVI 0 */
               "li r2, fwd\n"           /* 8: MOVIL */
               "li r3, var\n"           /* 16: MOVIL */
               "li r4, K\n"             /* 24: constante: MOVI */
               "fwd: halt\n"            /* 28 */
               "K = 3\n"
               ".data\n"
               "var: .word 0\n");
    WORD_AT(&o, 4, I(MOVI, 1, 0, 0));
    WORD_AT(&o, 8, LW(MOVIL, 2, 0));
    WORD_AT(&o, 12, 28);
    WORD_AT(&o, 16, LW(MOVIL, 3, 0));
    WORD_AT(&o, 20, o.res.ram_base);
    WORD_AT(&o, 24, I(MOVI, 4, 0, 3));
    ta_free(&o);
}

TEST(la_absolute_and_pic)
{
    asm_out o;

    ASM_OK(&o, "la r1, msg\nhalt\n.data\nmsg: .byte 1\n");
    WORD_AT(&o, 0, LW(MOVIL, 1, 0));
    WORD_AT(&o, 4, o.res.ram_base);
    ASSERT_EQ(o.hdr.flags & KSVM_KSX_FLAG_PIC, 0);
    ta_free(&o);

    /* .pic: ADRL rd, alvo - PC_next (PC_next = 8). */
    ASM_OK(&o, ".pic\nla r1, msg\nhalt\n.data\nmsg: .byte 1\n");
    WORD_AT(&o, 0, LW(ADRL, 1, 0));
    WORD_AT(&o, 4, o.res.ram_base - 8u);
    ASSERT_EQ(o.hdr.flags & KSVM_KSX_FLAG_PIC, KSVM_KSX_FLAG_PIC);
    ta_free(&o);
}

TEST(simple_pseudos)
{
    asm_out o;

    ASM_OK(&o, "clr r5\n"
               "inc r3\n"
               "dec r4\n"
               "not r6\n"
               "neg r7\n"
               "x: b x\n"
               "j x\n"
               "mov r1, 5\n");
    WORD_AT(&o, 0, R(MOV, 5, 0, 0));
    WORD_AT(&o, 4, I(ADDI, 3, 3, 1));
    WORD_AT(&o, 8, I(SUBI, 4, 4, 1));
    WORD_AT(&o, 12, R(NOT, 6, 6, 0));
    WORD_AT(&o, 16, R(NEG, 7, 7, 0));
    WORD_AT(&o, 20, ksvm_enc_b(KSVM_COND_AL, -4));
    WORD_AT(&o, 24, ksvm_enc_j(KSVM_OP_JMP, -8));
    WORD_AT(&o, 28, I(MOVI, 1, 0, 5));
    ta_free(&o);
}

TEST(cmp_with_immediate)
{
    asm_out o;

    ASM_OK(&o, "cmp r1, 5\n"            /* 0: CMPI */
               "cmp r2, 100000\n"       /* 4: CMPL */
               "cmp r3, fwd\n"          /* 12: desconhecido -> CMPL */
               "cmpi r4, 9000\n"        /* 20: CMPI explícito que não cabe -> CMPL */
               "fwd: tst r5, 0x10\n");  /* 28: TSTI */
    WORD_AT(&o, 0, I(CMPI, 1, 0, 5));
    WORD_AT(&o, 4, LW(CMPL, 2, 0));
    WORD_AT(&o, 8, 100000);
    WORD_AT(&o, 12, LW(CMPL, 3, 0));
    WORD_AT(&o, 16, 28);
    WORD_AT(&o, 20, LW(CMPL, 4, 0));
    WORD_AT(&o, 24, 9000);
    WORD_AT(&o, 28, I(TSTI, 5, 0, 0x10));
    ta_free(&o);
}

TEST(arith_immediates_grow_to_long)
{
    asm_out o;

    ASM_OK(&o, "add r1, r2, 5\n"         /* 0: ADDI */
               "add r1, r2, 70000\n"     /* 4: ADDL */
               "sub r1, r2, 5\n"         /* 12: SUBI */
               "sub r1, r2, 70000\n"     /* 16: ADDL -70000 */
               "subi r1, r2, -9000\n"    /* 24: ADDL 9000 (doc 04 §7) */
               "addi r1, r2, 10000\n"    /* 32: ADDL */
               "andi r1, r2, 0xFFFF\n"   /* 40: ANDL */
               "or r1, r2, 0xF\n"        /* 48: ORI */
               "xor r1, r2, -1\n"        /* 52: XORI */
               "shl r1, r2, 3\n"         /* 56: SHLI */
               "mul r1, r2, 3\n");       /* 60: MULI */
    WORD_AT(&o, 0, I(ADDI, 1, 2, 5));
    WORD_AT(&o, 4, LW(ADDL, 1, 2));
    WORD_AT(&o, 8, 70000);
    WORD_AT(&o, 12, I(SUBI, 1, 2, 5));
    WORD_AT(&o, 16, LW(ADDL, 1, 2));
    WORD_AT(&o, 20, (uint32_t)-70000);
    WORD_AT(&o, 24, LW(ADDL, 1, 2));
    WORD_AT(&o, 28, 9000);
    WORD_AT(&o, 32, LW(ADDL, 1, 2));
    WORD_AT(&o, 36, 10000);
    WORD_AT(&o, 40, LW(ANDL, 1, 2));
    WORD_AT(&o, 44, 0xFFFF);
    WORD_AT(&o, 48, I(ORI, 1, 2, 0xF));
    WORD_AT(&o, 52, I(XORI, 1, 2, -1));
    WORD_AT(&o, 56, I(SHLI, 1, 2, 3));
    WORD_AT(&o, 60, I(MULI, 1, 2, 3));
    ta_free(&o);
}

TEST(explicit_short_form_with_forward_value)
{
    asm_out o;

    /* "addi" com valor desconhecido no passo 1 fica curto (4 bytes). */
    ASM_OK(&o, "addi r1, r1, K\nhalt\nK = 12\n");
    WORD_AT(&o, 0, I(ADDI, 1, 1, 12));
    ASSERT_EQ(o.hdr.code_size, 8);
    ta_free(&o);
}

TEST(push_pop_lists)
{
    asm_out o;

    ASM_OK(&o, "push r1, r2, fp\npop fp, r2, r1\n");
    WORD_AT(&o, 0, R(PUSH, 1, 0, 0));
    WORD_AT(&o, 4, R(PUSH, 2, 0, 0));
    WORD_AT(&o, 8, R(PUSH, 30, 0, 0));
    WORD_AT(&o, 12, R(POP, 30, 0, 0));
    WORD_AT(&o, 16, R(POP, 2, 0, 0));
    WORD_AT(&o, 20, R(POP, 1, 0, 0));
    ta_free(&o);
}

TEST(named_syscalls_and_special_registers)
{
    asm_out o;

    ASM_OK(&o, "syscall PUTS\n"
               "syscall putc\n"
               "syscall EXIT\n"
               "syscall MEM_INFO\n"
               "mfsr r1, flags\n"
               "mfsr r2, INSN_COUNT\n"
               "mtsr FLAGS, r3\n");
    WORD_AT(&o, 0, I(SYSCALL, 0, 0, KSVM_SYSCALL_PUTS));
    WORD_AT(&o, 4, I(SYSCALL, 0, 0, KSVM_SYSCALL_PUTC));
    WORD_AT(&o, 8, I(SYSCALL, 0, 0, KSVM_SYSCALL_EXIT));
    WORD_AT(&o, 12, I(SYSCALL, 0, 0, KSVM_SYSCALL_MEM_INFO));
    WORD_AT(&o, 16, I(MFSR, 1, 0, KSVM_SR_FLAGS));
    WORD_AT(&o, 20, I(MFSR, 2, 0, KSVM_SR_INSN_COUNT));
    WORD_AT(&o, 24, I(MTSR, 0, 3, KSVM_SR_FLAGS));
    ta_free(&o);
}

TEST(case_insensitive_mnemonics_and_registers)
{
    asm_out o;

    ASM_OK(&o, "LI R1, 1\nAdD Sp, FP, zR\nB.EQ x\nx: HALT\n");
    WORD_AT(&o, 0, I(MOVI, 1, 0, 1));
    WORD_AT(&o, 4, R(ADD, 31, 30, 0));
    WORD_AT(&o, 8, ksvm_enc_b(KSVM_COND_EQ, 0));
    ta_free(&o);
}

SUITE(ksas_pseudo)
{
    RUN_TEST(li_picks_short_or_long);
    RUN_TEST(li_with_labels);
    RUN_TEST(la_absolute_and_pic);
    RUN_TEST(simple_pseudos);
    RUN_TEST(cmp_with_immediate);
    RUN_TEST(arith_immediates_grow_to_long);
    RUN_TEST(explicit_short_form_with_forward_value);
    RUN_TEST(push_pop_lists);
    RUN_TEST(named_syscalls_and_special_registers);
    RUN_TEST(case_insensitive_mnemonics_and_registers);
}
