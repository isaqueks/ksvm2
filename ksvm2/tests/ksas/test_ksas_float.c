/*
 * test_ksas_float.c — extensão F no assembler (doc 07): mnemônicos e
 * formas de operandos de FADD..FCMP, a diretiva .float e a pseudo fli.
 */
#include "ksas_test.h"

#define R(op, a, b, c) ksvm_enc_r(KSVM_OP_##op, (a), (b), (c))
#define LW(op, a, b)   ksvm_enc_l(KSVM_OP_##op, (a), (b))

static uint32_t data32(const asm_out *o, uint32_t off)
{
    return ksvm_ld32le(o->res.image + o->hdr.data_off + off);
}

/* Campos como nas inteiras: "rd, ra, rb" -> ra, rb, rc; "rd, rb" -> ra, rb
 * (e "fneg rd" = "fneg rd, rd", como not); fcmp ra, rb como cmp. */
TEST(float_mnemonics_encode)
{
    asm_out o;

    ASM_OK(&o, "fadd r1, r2, r3\n"      /* 0 */
               "fsub r4, r5, r6\n"
               "fmul r7, r8, r9\n"
               "fdiv r10, r11, r12\n"
               "fmin r13, r14, r15\n"   /* 16 */
               "fmax r16, r17, r18\n"
               "fsqrt r1, r2\n"         /* 24 */
               "FNEG r3, r4\n"
               "fabs r5, r6\n"
               "frint r7, r8\n"
               "ffloor r9, r10\n"       /* 40 */
               "fceil r11, r12\n"
               "ftrunc r13, r14\n"
               "ftoi r15, r16\n"
               "ftou r17, r18\n"        /* 56 */
               "itof r19, r20\n"
               "utof r21, r22\n"
               "fcmp r23, r24\n"        /* 68 */
               "fneg r25\n");           /* 72 */
    ASSERT_EQ_U32(ta_code32(&o, 0), R(FADD, 1, 2, 3));
    ASSERT_EQ_U32(ta_code32(&o, 4), R(FSUB, 4, 5, 6));
    ASSERT_EQ_U32(ta_code32(&o, 8), R(FMUL, 7, 8, 9));
    ASSERT_EQ_U32(ta_code32(&o, 12), R(FDIV, 10, 11, 12));
    ASSERT_EQ_U32(ta_code32(&o, 16), R(FMIN, 13, 14, 15));
    ASSERT_EQ_U32(ta_code32(&o, 20), R(FMAX, 16, 17, 18));
    ASSERT_EQ_U32(ta_code32(&o, 24), R(FSQRT, 1, 2, 0));
    ASSERT_EQ_U32(ta_code32(&o, 28), R(FNEG, 3, 4, 0));
    ASSERT_EQ_U32(ta_code32(&o, 32), R(FABS, 5, 6, 0));
    ASSERT_EQ_U32(ta_code32(&o, 36), R(FRINT, 7, 8, 0));
    ASSERT_EQ_U32(ta_code32(&o, 40), R(FFLOOR, 9, 10, 0));
    ASSERT_EQ_U32(ta_code32(&o, 44), R(FCEIL, 11, 12, 0));
    ASSERT_EQ_U32(ta_code32(&o, 48), R(FTRUNC, 13, 14, 0));
    ASSERT_EQ_U32(ta_code32(&o, 52), R(FTOI, 15, 16, 0));
    ASSERT_EQ_U32(ta_code32(&o, 56), R(FTOU, 17, 18, 0));
    ASSERT_EQ_U32(ta_code32(&o, 60), R(ITOF, 19, 20, 0));
    ASSERT_EQ_U32(ta_code32(&o, 64), R(UTOF, 21, 22, 0));
    ASSERT_EQ_U32(ta_code32(&o, 68), R(FCMP, 23, 24, 0));
    ASSERT_EQ_U32(ta_code32(&o, 72), R(FNEG, 25, 25, 0));
    ASSERT_EQ(o.hdr.code_size, 76);
    ta_free(&o);
}

TEST(float_operand_errors)
{
    ASM_ERR("fadd r1, r2, 5\n", 1, 14, "'fadd' nao aceita imediato");
    ASM_ERR("fsqrt r1, 4\n", 1, 11, "'fsqrt' nao aceita imediato");
    ASM_ERR("fcmp r1\n", 1, 8, "'fcmp' espera dois operandos");
    ASM_ERR("fcmp r1, 0\n", 1, 10, "'fcmp' nao aceita imediato");
    ASM_ERR("fadd r1, r2\n", 1, 12, "esperado ','");
}

/* binary32 little-endian; inteiros viram float; NaN canônico com sinal. */
TEST(float_directive_values)
{
    static const uint32_t want[] = {
        0x3FC00000u, 0xC4FA0000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u,
        0x40400000u, 0x00000000u, 0x80000000u, 0x00000001u, 0x7F7FFFFFu, 0x3DCCCCCDu,
        0x3F800000u, 0x3EAAAAABu, 0x00000000u, 0x4B800000u, 0xC0000000u, 0x7F800000u,
    };
    asm_out o;
    uint32_t i;

    ASM_OK(&o, "halt\n"
               ".data\n"
               "v: .float 1.5, -2e3, inf, -inf, nan, -nan\n"
               "   .float 0x1.8p1, 0, -0.0, 1.40129846e-45, 3.4028235e38, 0.1\n"
               "   .float 1, 0.333333333333, 1e-50, 16777217, -2.0 ; comentario\n"
               "   .float INFINITY\n");
    ASSERT_EQ(o.hdr.data_size, 4u * (sizeof want / sizeof want[0]));
    for (i = 0; i < sizeof want / sizeof want[0]; i++) {
        if (data32(&o, 4u * i) != want[i]) {
            test_note(t_, "valor %lu", (unsigned long)i);
        }
        ASSERT_EQ_U32(data32(&o, 4u * i), want[i]);
    }
    ASSERT_EQ(ta_data8(&o, 0), 0x00);                  /* little-endian */
    ASSERT_EQ(ta_data8(&o, 3), 0x3F);
    ta_free(&o);

    /* Também em .rodata (região de código), sem alinhamento automático. */
    ASM_OK(&o, "halt\n.rodata\n.byte 7\nk: .float -1.5\n");
    ASSERT_EQ(ta_code8(&o, 4), 7);
    ASSERT_EQ(ta_code8(&o, 5), 0x00);
    ASSERT_EQ(ta_code8(&o, 7), 0xC0);
    ASSERT_EQ(ta_code8(&o, 8), 0xBF);
    ta_free(&o);
}

TEST(float_directive_errors)
{
    ASM_ERR(".data\n.float\n", 2, 7, "esperado valor float");
    ASM_ERR(".data\n.float abc\n", 2, 8, "valor float invalido");
    ASM_ERR(".data\n.float 1.5x\n", 2, 8, "valor float invalido");
    ASM_ERR(".data\n.float 1.5.2\n", 2, 8, "valor float invalido");
    ASM_ERR(".data\n.float 1e39\n", 2, 8, "fora do alcance de float");
    ASM_ERR(".data\n.float -1e39\n", 2, 8, "fora do alcance de float");
    ASM_ERR(".data\n.float 1.5 2.5\n", 2, 12, "texto inesperado");
    ASM_ERR(".data\n.float 1.5,\n", 2, 12, "esperado valor float");
    ASM_ERR(".data\n.float r1\n", 2, 8, "valor float invalido");
    ASM_ERR(".bss\n.float 1\n", 2, 1, "nao sao permitidos em .bss");
}

/* fli rd, v = movil rd, bits (sempre a forma longa). */
TEST(fli_pseudo)
{
    asm_out o;

    ASM_OK(&o, "fli r1, 1.5\n"           /* 0 */
               "fli r2, -nan\n"          /* 8 */
               "fli r3, 1\n"             /* 16: 1.0, não o inteiro 1 */
               "fli r4, 0x1p-149\n"      /* 24 */
               "fli sp, -0\n"            /* 32 */
               "halt\n");
    ASSERT_EQ_U32(ta_code32(&o, 0), LW(MOVIL, 1, 0));
    ASSERT_EQ_U32(ta_code32(&o, 4), 0x3FC00000u);
    ASSERT_EQ_U32(ta_code32(&o, 12), 0xFFC00000u);
    ASSERT_EQ_U32(ta_code32(&o, 20), 0x3F800000u);
    ASSERT_EQ_U32(ta_code32(&o, 28), 0x00000001u);
    ASSERT_EQ_U32(ta_code32(&o, 32), LW(MOVIL, 31, 0));
    ASSERT_EQ_U32(ta_code32(&o, 36), 0x80000000u);
    ASSERT_EQ(o.hdr.code_size, 44);
    ta_free(&o);

    ASM_ERR("fli r1 1.5\n", 1, 8, "esperado ','");
    ASM_ERR("fli 2, 1.5\n", 1, 5, "esperado registrador");
    ASM_ERR("fli r1, x\n", 1, 9, "valor float invalido");
    ASM_ERR("fli r1, 2.5 + 1\n", 1, 13, "texto inesperado");
    ASM_ERR(".data\nfli r1, 1\n", 2, 1, "instrucoes so podem aparecer em .text");
}

/* Programa montado e executado: (1.5 + 2.25) * 2 = 7.5; rint -> 8; ftoi. */
TEST(float_program_runs)
{
    vm_box *b = ta_run(t_, "_start: fli  r2, 1.5\n"
                           "        la   r5, val\n"
                           "        ldw  r3, [r5]\n"
                           "        fadd r1, r2, r3\n"
                           "        ldw  r4, [r5 + 4]\n"
                           "        fmul r1, r1, r4\n"
                           "        fcmp r1, r2\n"
                           "        ble  bad\n"         /* 7.5 > 1.5 */
                           "        frint r1, r1\n"
                           "        ftoi r1, r1\n"
                           "        halt\n"
                           "bad:    li   r1, 99\n"
                           "        halt\n"
                           "        .data\n"
                           "val:    .float 2.25, 2\n");
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ(b->status, KSVM_HALTED);
    ASSERT_EQ_U32(b->vm.exit_code, 8u);
    ASSERT_EQ_U32(b->vm.cpu.r[3], 0x40100000u);         /* 2.25 */
    ta_box_free(b);
}

SUITE(ksas_float)
{
    RUN_TEST(float_mnemonics_encode);
    RUN_TEST(float_operand_errors);
    RUN_TEST(float_directive_values);
    RUN_TEST(float_directive_errors);
    RUN_TEST(fli_pseudo);
    RUN_TEST(float_program_runs);
}
