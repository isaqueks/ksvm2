/*
 * test_exec_mov.c — movimentação e constantes (doc 04 §5.1).
 */
#include "fixture.h"
#include "test.h"

TEST(nop_only_advances_pc)
{
    fixture f;
    LOAD(&f, OP_R(NOP, 0, 0, 0), HALT);
    ASSERT_EQ(fix_steps(&f, 1), KSVM_RUNNING);
    ASSERT_EQ_U32(f.vm.cpu.pc, 4u);
    ASSERT_EQ_U32(f.vm.cpu.flags, 0u);
    ASSERT_EQ_U32(f.vm.insn_count, 1u);
}

TEST(mov_copies_rb_into_rd)
{
    fixture f;
    RUN(&f, LI(2, 0xCAFEF00Du), OP_R(MOV, 1, 2, 0), HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 1), 0xCAFEF00Du);
}

TEST(movi_sign_extends_imm14)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 1, 0, -8192), OP_I(MOVI, 2, 0, 8191), OP_I(MOVI, 3, 0, -1), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0xFFFFE000u);
    ASSERT_EQ_U32(REG(&f, 2), 8191u);
    ASSERT_EQ_U32(REG(&f, 3), 0xFFFFFFFFu);
}

TEST(movil_loads_imm32_and_is_8_bytes)
{
    fixture f;
    LOAD(&f, LI(5, 0x80000001u), HALT);
    ASSERT_EQ(fix_steps(&f, 1), KSVM_RUNNING);
    ASSERT_EQ_U32(REG(&f, 5), 0x80000001u);
    ASSERT_EQ_U32(f.vm.cpu.pc, 8u);
}

/* 0x04 era MOVHI, removido da ISA (doc 04 §5.1): opcode inválido, sem efeito. */
TEST(opcode_04_removed_is_bad_opcode)
{
    fixture f;
    uint32_t w = 0x04u | (1u << 8) | (0x123u << 18);
    RUN(&f, LI(1, 0xAAAA1234u), w, HALT);
    ASSERT_EQ(f.vm.status, KSVM_FAULT);
    ASSERT_EQ(f.vm.fault, KSVM_FAULT_BAD_OPCODE);
    ASSERT_EQ_U32(f.vm.fault_addr, w);
    ASSERT_EQ_U32(f.vm.cpu.pc, 8u);
    ASSERT_EQ_U32(REG(&f, 1), 0xAAAA1234u);
    ASSERT_EQ(ksvm_isa_format(0x04u), KSVM_FMT_INVALID);
}

TEST(adr_is_pc_relative_to_next)
{
    fixture f;
    RUN(&f, OP_R(NOP, 0, 0, 0), OP_I(ADR, 1, 0, 16), OP_I(ADR, 2, 0, -8), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 8u + 16u);         /* ADR em 4: PC_next = 8 */
    ASSERT_EQ_U32(REG(&f, 2), 12u - 8u);
}

TEST(adrl_is_pc_relative_long)
{
    fixture f;
    RUN(&f, OP_R(NOP, 0, 0, 0), OP_L(ADRL, 3, 0, 0x10000), OP_L(ADRL, 4, 0, -20), HALT);
    ASSERT_EQ_U32(REG(&f, 3), 12u + 0x10000u);   /* ADRL em 4, 8 bytes */
    ASSERT_EQ_U32(REG(&f, 4), 20u - 20u);
}

TEST(mfsr_reads_special_registers)
{
    fixture f;
    RUN(&f,
        SET_FLAGS(KSVM_FLAG_C | KSVM_FLAG_N),
        OP_I(MFSR, 1, 0, KSVM_SR_FLAGS),
        OP_I(MFSR, 2, 0, KSVM_SR_PC),
        OP_I(MFSR, 3, 0, KSVM_SR_ISA_VERSION),
        OP_I(MFSR, 4, 0, KSVM_SR_INSN_COUNT),
        HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 1), KSVM_FLAG_C | KSVM_FLAG_N);
    ASSERT_EQ_U32(REG(&f, 2), 16u);              /* endereço da própria MFSR */
    ASSERT_EQ_U32(REG(&f, 3), 0x0200u);
    ASSERT_EQ_U32(REG(&f, 4), 5u);               /* 5 instruções antes dela */
}

TEST(mtsr_writes_flags_masking_reserved_bits)
{
    fixture f;
    RUN(&f, LI(7, 0xFFFFFFF5u), OP_I(MTSR, 0, 7, KSVM_SR_FLAGS), HALT);
    ASSERT_EQ_U32(f.vm.cpu.flags, 0x5u);
}

TEST(writes_to_zr_are_discarded)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 0, 0, 123), OP_R(MOV, 1, 0, 0), LI(0, 7), OP_I(ADDI, 2, 0, 5), HALT);
    ASSERT_EQ_U32(REG(&f, 0), 0u);
    ASSERT_EQ_U32(REG(&f, 1), 0u);
    ASSERT_EQ_U32(REG(&f, 2), 5u);
}

/* Exemplo literal do doc 09 §1. */
TEST(add_sets_carry_and_zero)
{
    fixture f;
    RUN(&f, LI(1, 0xFFFFFFFFu), OP_I(ADDI, 2, 1, 1), HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 2), 0u);
    ASSERT_EQ_U32(f.vm.cpu.flags, KSVM_FLAG_Z | KSVM_FLAG_C);
}

SUITE(exec_mov)
{
    RUN_TEST(nop_only_advances_pc);
    RUN_TEST(mov_copies_rb_into_rd);
    RUN_TEST(movi_sign_extends_imm14);
    RUN_TEST(movil_loads_imm32_and_is_8_bytes);
    RUN_TEST(opcode_04_removed_is_bad_opcode);
    RUN_TEST(adr_is_pc_relative_to_next);
    RUN_TEST(adrl_is_pc_relative_long);
    RUN_TEST(mfsr_reads_special_registers);
    RUN_TEST(mtsr_writes_flags_masking_reserved_bits);
    RUN_TEST(writes_to_zr_are_discarded);
    RUN_TEST(add_sets_carry_and_zero);
}
