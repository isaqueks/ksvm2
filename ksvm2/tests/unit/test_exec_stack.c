/*
 * test_exec_stack.c — PUSH/POP e os opcionais PUSHM/POPM (doc 04 §5.3).
 */
#include "fixture.h"
#include "test.h"

#define SP KSVM_REG_SP

TEST(push_decrements_sp_then_stores)
{
    fixture f;
    RUN(&f, LI(1, 0xA1B2C3D4u), OP_R(PUSH, 1, 0, 0), HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, SP), FIX_RAM_END - 4u);
    ASSERT_EQ_U32(fix_ram32(&f, FIX_RAM_END - 4u), 0xA1B2C3D4u);
}

TEST(pop_loads_then_increments_sp)
{
    fixture f;
    LOAD(&f, OP_I(ADDI, SP, SP, -4), OP_R(POP, 2, 0, 0), HALT);
    fix_set_ram32(&f, FIX_RAM_END - 4u, 0x0BADF00Du);
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 2), 0x0BADF00Du);
    ASSERT_EQ_U32(REG(&f, SP), FIX_RAM_END);
}

TEST(push_pop_is_lifo)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 1, 0, 1), OP_I(MOVI, 2, 0, 2), OP_I(MOVI, 3, 0, 3),
        OP_R(PUSH, 1, 0, 0), OP_R(PUSH, 2, 0, 0), OP_R(PUSH, 3, 0, 0),
        OP_R(POP, 4, 0, 0), OP_R(POP, 5, 0, 0), OP_R(POP, 6, 0, 0), HALT);
    ASSERT_EQ_U32(REG(&f, 4), 3u);
    ASSERT_EQ_U32(REG(&f, 5), 2u);
    ASSERT_EQ_U32(REG(&f, 6), 1u);
    ASSERT_EQ_U32(REG(&f, SP), FIX_RAM_END);
}

TEST(push_zr_pushes_zero)
{
    fixture f;
    LOAD(&f, OP_R(PUSH, 0, 0, 0), HALT);
    fix_set_ram32(&f, FIX_RAM_END - 4u, 0xFFFFFFFFu);
    (void)fix_run(&f);
    ASSERT_EQ_U32(fix_ram32(&f, FIX_RAM_END - 4u), 0u);
}

TEST(push_sp_stores_previous_sp)
{
    fixture f;
    RUN(&f, OP_R(PUSH, SP, 0, 0), HALT);
    ASSERT_EQ_U32(fix_ram32(&f, FIX_RAM_END - 4u), FIX_RAM_END);
}

TEST(pop_sp_takes_loaded_value)
{
    fixture f;
    LOAD(&f, OP_I(ADDI, SP, SP, -4), OP_R(POP, SP, 0, 0), HALT);
    fix_set_ram32(&f, FIX_RAM_END - 4u, FIX_RAM_BASE + 0x80u);
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, SP), FIX_RAM_BASE + 0x80u);
}

TEST(pushm_popm_are_not_implemented)
{
    fixture f;
    RUN(&f, OP_I(PUSHM, 0, 0, 0x3FFF), HALT);
    ASSERT_EQ(f.vm.status, KSVM_FAULT);
    ASSERT_EQ(f.vm.fault, KSVM_FAULT_BAD_OPCODE);
    ASSERT_EQ_U32(REG(&f, SP), FIX_RAM_END);
    RUN(&f, OP_I(POPM, 0, 0, 1), HALT);
    ASSERT_EQ(f.vm.fault, KSVM_FAULT_BAD_OPCODE);
}

SUITE(exec_stack)
{
    RUN_TEST(push_decrements_sp_then_stores);
    RUN_TEST(pop_loads_then_increments_sp);
    RUN_TEST(push_pop_is_lifo);
    RUN_TEST(push_zr_pushes_zero);
    RUN_TEST(push_sp_stores_previous_sp);
    RUN_TEST(pop_sp_takes_loaded_value);
    RUN_TEST(pushm_popm_are_not_implemented);
}
