/*
 * test_exec_arith.c — aritmética inteira executada pela VM (doc 04 §5.4).
 * A exatidão exaustiva das operações está em test_alu.c; aqui se verifica
 * decodificação (qual campo é qual operando), escrita do destino e flags.
 */
#include "fixture.h"
#include "test.h"

#define Z KSVM_FLAG_Z
#define N KSVM_FLAG_N
#define C KSVM_FLAG_C
#define V KSVM_FLAG_V

TEST(add_rd_ra_rb_fields)
{
    fixture f;
    RUN(&f, LI(2, 40), LI(3, 2), OP_R(ADD, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 42u);
    ASSERT_EQ_U32(REG(&f, 2), 40u);
    ASSERT_EQ_U32(f.vm.cpu.flags, 0u);
}

TEST(add_signed_overflow)
{
    fixture f;
    RUN(&f, LI(2, 0x7FFFFFFFu), LI(3, 1), OP_R(ADD, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x80000000u);
    ASSERT_EQ_U32(f.vm.cpu.flags, N | V);
}

TEST(addi_negative_immediate)
{
    fixture f;
    RUN(&f, LI(2, 1), OP_I(ADDI, 1, 2, -1), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0u);
    ASSERT_EQ_U32(f.vm.cpu.flags, Z | C);
}

TEST(addl_imm32)
{
    fixture f;
    RUN(&f, LI(2, 0x80000000u), OP_L(ADDL, 1, 2, 0x80000000u), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0u);
    ASSERT_EQ_U32(f.vm.cpu.flags, Z | C | V);
}

TEST(sub_borrow_clears_carry)
{
    fixture f;
    RUN(&f, LI(2, 5), LI(3, 7), OP_R(SUB, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0xFFFFFFFEu);
    ASSERT_EQ_U32(f.vm.cpu.flags, N);
}

TEST(subi_equal_sets_zero_and_carry)
{
    fixture f;
    RUN(&f, LI(2, 10), OP_I(SUBI, 1, 2, 10), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0u);
    ASSERT_EQ_U32(f.vm.cpu.flags, Z | C);
}

/* 0x00000001_FFFFFFFF + 0x00000000_00000001 com ADD + ADC. */
TEST(adc_chains_64bit_add)
{
    fixture f;
    RUN(&f, LI(1, 0xFFFFFFFFu), LI(2, 1), LI(3, 1), LI(4, 0),
        OP_R(ADD, 5, 1, 3), OP_R(ADC, 6, 2, 4), HALT);
    ASSERT_EQ_U32(REG(&f, 5), 0u);
    ASSERT_EQ_U32(REG(&f, 6), 2u);
    ASSERT_EQ_U32(f.vm.cpu.flags, 0u);
}

/* 0x00000002_00000000 - 1 com SUB + SBC (C = não-borrow). */
TEST(sbc_chains_64bit_sub)
{
    fixture f;
    RUN(&f, LI(1, 0), LI(2, 2), LI(3, 1), LI(4, 0),
        OP_R(SUB, 5, 1, 3), OP_R(SBC, 6, 2, 4), HALT);
    ASSERT_EQ_U32(REG(&f, 5), 0xFFFFFFFFu);
    ASSERT_EQ_U32(REG(&f, 6), 1u);
    ASSERT_EQ_U32(f.vm.cpu.flags, C);
}

TEST(neg_values)
{
    fixture f;
    RUN(&f, LI(2, 5), OP_R(NEG, 1, 2, 0), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0xFFFFFFFBu);
    ASSERT_EQ_U32(f.vm.cpu.flags, N);
    RUN(&f, OP_R(NEG, 1, 0, 0), HALT);
    ASSERT_EQ_U32(f.vm.cpu.flags, Z | C);
    RUN(&f, LI(2, 0x80000000u), OP_R(NEG, 1, 2, 0), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x80000000u);
    ASSERT_EQ_U32(f.vm.cpu.flags, N | V);
}

TEST(cmp_sets_flags_without_writing)
{
    fixture f;
    RUN(&f, LI(1, 3), LI(2, 3), OP_R(CMP, 1, 2, 0), HALT);
    ASSERT_EQ_U32(f.vm.cpu.flags, Z | C);
    ASSERT_EQ_U32(REG(&f, 1), 3u);
    RUN(&f, LI(1, 0x80000000u), LI(2, 1), OP_R(CMP, 1, 2, 0), HALT);
    ASSERT_EQ_U32(f.vm.cpu.flags, C | V);       /* INT_MIN - 1 */
}

TEST(cmpi_with_negative_immediate)
{
    fixture f;
    RUN(&f, LI(1, 0xFFFFFFFFu), OP_I(CMPI, 1, 0, -1), HALT);
    ASSERT_EQ_U32(f.vm.cpu.flags, Z | C);
    RUN(&f, LI(1, 0), OP_I(CMPI, 1, 0, 1), HALT);
    ASSERT_EQ_U32(f.vm.cpu.flags, N);
}

TEST(cmpl_with_imm32)
{
    fixture f;
    RUN(&f, LI(1, 0x12345678u), OP_L(CMPL, 1, 0, 0x12345679u), HALT);
    ASSERT_EQ_U32(f.vm.cpu.flags, N);
    ASSERT_EQ_U32(f.vm.cpu.pc, 8u + 8u + 4u);
}

#if KSVM_ENABLE_MULDIV
TEST(mul_low_word_and_zn_only)
{
    fixture f;
    RUN(&f, SET_FLAGS(C | V), LI(2, 0x10000u), LI(3, 0x10001u), OP_R(MUL, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x10000u);
    ASSERT_EQ_U32(f.vm.cpu.flags, C | V);       /* C e V preservados */
}

TEST(muli_negative)
{
    fixture f;
    RUN(&f, LI(2, 7), OP_I(MULI, 1, 2, -3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), (uint32_t)-21);
    ASSERT_EQ_U32(f.vm.cpu.flags, N);
}

TEST(mulh_signed_high)
{
    fixture f;
    RUN(&f, LI(2, 0xFFFFFFFFu), LI(3, 0xFFFFFFFFu), OP_R(MULH, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0u);              /* (-1)*(-1) = 1 */
    ASSERT_EQ_U32(f.vm.cpu.flags, Z);
    RUN(&f, LI(2, 0x80000000u), LI(3, 2), OP_R(MULH, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0xFFFFFFFFu);     /* -2^32 */
}

TEST(mulhu_unsigned_high)
{
    fixture f;
    RUN(&f, LI(2, 0xFFFFFFFFu), LI(3, 0xFFFFFFFFu), OP_R(MULHU, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0xFFFFFFFEu);
    ASSERT_EQ_U32(f.vm.cpu.flags, N);
}

TEST(div_truncates_toward_zero)
{
    fixture f;
    RUN(&f, LI(2, (uint32_t)-7), LI(3, 2), OP_R(DIV, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), (uint32_t)-3);
    ASSERT_EQ_U32(f.vm.cpu.flags, N);
}

TEST(div_int_min_by_minus_one)
{
    fixture f;
    RUN(&f, LI(2, 0x80000000u), LI(3, 0xFFFFFFFFu), OP_R(DIV, 1, 2, 3), OP_R(REM, 4, 2, 3), HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 1), 0x80000000u);
    ASSERT_EQ_U32(REG(&f, 4), 0u);
}

TEST(divu_unsigned)
{
    fixture f;
    RUN(&f, LI(2, 0xFFFFFFFEu), LI(3, 2), OP_R(DIVU, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x7FFFFFFFu);
}

TEST(rem_sign_of_dividend)
{
    fixture f;
    RUN(&f, LI(2, (uint32_t)-7), LI(3, (uint32_t)-2), OP_R(REM, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), (uint32_t)-1);
    RUN(&f, LI(2, 7), LI(3, (uint32_t)-2), OP_R(REM, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 1u);
}

TEST(remu_unsigned)
{
    fixture f;
    RUN(&f, LI(2, 0xFFFFFFFFu), LI(3, 10), OP_R(REMU, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 5u);
    ASSERT_EQ_U32(f.vm.cpu.flags, 0u);
}

/* Critério da fase 1 (doc 10): fatorial por laço. */
TEST(factorial_loop)
{
    fixture f;
    RUN(&f,
        OP_I(MOVI, 1, 0, 10),            /* 0:  n = 10          */
        OP_I(MOVI, 2, 0, 1),             /* 4:  acc = 1         */
        OP_R(MUL, 2, 2, 1),              /* 8:  acc *= n        */
        OP_I(SUBI, 1, 1, 1),             /* 12: n--             */
        OP_B(NE, -12),                   /* 16: if n != 0 -> 8  */
        OP_R(MOV, 1, 2, 0),              /* 20: r1 = acc        */
        HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 1), 3628800u);
    ASSERT_EQ_U32(f.vm.exit_code, 3628800u);
}
#else
/* KSVM_ENABLE_MULDIV=0: MUL..REMU não existem nesta VM (doc 04 §5.4) e
 * respondem BAD_OPCODE sem efeito, mesmo com divisor zero. */
TEST(muldiv_disabled_is_bad_opcode)
{
    unsigned op;
    for (op = KSVM_OP_MUL; op <= KSVM_OP_REMU; op++) {
        fixture f;
        uint32_t w = (op == KSVM_OP_MULI) ? ksvm_enc_i((uint8_t)op, 1, 2, 3)
                                          : ksvm_enc_r((uint8_t)op, 1, 2, 0);
        RUN(&f, LI(1, 5), LI(2, 7), w, HALT);
        ASSERT_EQ(f.vm.status, KSVM_FAULT);
        ASSERT_EQ(f.vm.fault, KSVM_FAULT_BAD_OPCODE);
        ASSERT_EQ_U32(f.vm.fault_addr, w);
        ASSERT_EQ_U32(f.vm.cpu.pc, 16u);
        ASSERT_EQ_U32(REG(&f, 1), 5u);
    }
}
#endif

SUITE(exec_arith)
{
    RUN_TEST(add_rd_ra_rb_fields);
    RUN_TEST(add_signed_overflow);
    RUN_TEST(addi_negative_immediate);
    RUN_TEST(addl_imm32);
    RUN_TEST(sub_borrow_clears_carry);
    RUN_TEST(subi_equal_sets_zero_and_carry);
    RUN_TEST(adc_chains_64bit_add);
    RUN_TEST(sbc_chains_64bit_sub);
    RUN_TEST(neg_values);
    RUN_TEST(cmp_sets_flags_without_writing);
    RUN_TEST(cmpi_with_negative_immediate);
    RUN_TEST(cmpl_with_imm32);
#if KSVM_ENABLE_MULDIV
    RUN_TEST(mul_low_word_and_zn_only);
    RUN_TEST(muli_negative);
    RUN_TEST(mulh_signed_high);
    RUN_TEST(mulhu_unsigned_high);
    RUN_TEST(div_truncates_toward_zero);
    RUN_TEST(div_int_min_by_minus_one);
    RUN_TEST(divu_unsigned);
    RUN_TEST(rem_sign_of_dividend);
    RUN_TEST(remu_unsigned);
    RUN_TEST(factorial_loop);
#else
    RUN_TEST(muldiv_disabled_is_bad_opcode);
#endif
}
