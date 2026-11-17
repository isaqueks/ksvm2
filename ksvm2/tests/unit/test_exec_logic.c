/*
 * test_exec_logic.c — lógica, deslocamentos, rotações, extensões e contagem
 * de bits executados pela VM (doc 04 §5.5).
 */
#include "fixture.h"
#include "test.h"

#define Z KSVM_FLAG_Z
#define N KSVM_FLAG_N
#define C KSVM_FLAG_C
#define V KSVM_FLAG_V

TEST(and_forms)
{
    fixture f;
    RUN(&f, SET_FLAGS(C | V), LI(2, 0xF0F0F0F0u), LI(3, 0xFF00FF00u),
        OP_R(AND, 1, 2, 3), OP_I(ANDI, 4, 2, 0x0FF0), OP_L(ANDL, 5, 2, 0x80000000u), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0xF000F000u);
    ASSERT_EQ_U32(REG(&f, 4), 0x000000F0u);
    ASSERT_EQ_U32(REG(&f, 5), 0x80000000u);
    ASSERT_EQ_U32(f.vm.cpu.flags, N | C | V);   /* C e V preservados */
}

TEST(andi_immediate_is_sign_extended)
{
    fixture f;
    RUN(&f, LI(2, 0x12345678u), OP_I(ANDI, 1, 2, -16), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x12345670u);
}

TEST(or_forms)
{
    fixture f;
    RUN(&f, LI(2, 0x0F000000u), LI(3, 0x000000F0u),
        OP_R(OR, 1, 2, 3), OP_I(ORI, 4, 2, 1), OP_L(ORL, 5, 0, 0), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x0F0000F0u);
    ASSERT_EQ_U32(REG(&f, 4), 0x0F000001u);
    ASSERT_EQ_U32(REG(&f, 5), 0u);
    ASSERT_EQ_U32(f.vm.cpu.flags, Z);
}

TEST(xor_forms)
{
    fixture f;
    RUN(&f, LI(2, 0xAAAAAAAAu), LI(3, 0xFFFFFFFFu),
        OP_R(XOR, 1, 2, 3), OP_I(XORI, 4, 2, -1), OP_L(XORL, 5, 2, 0xAAAAAAAAu), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x55555555u);
    ASSERT_EQ_U32(REG(&f, 4), 0x55555555u);
    ASSERT_EQ_U32(REG(&f, 5), 0u);
    ASSERT_EQ_U32(f.vm.cpu.flags, Z);
}

TEST(not_inverts_rb)
{
    fixture f;
    RUN(&f, LI(2, 0x0000FFFFu), OP_R(NOT, 1, 2, 0), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0xFFFF0000u);
    ASSERT_EQ_U32(f.vm.cpu.flags, N);
}

TEST(shl_and_shli)
{
    fixture f;
    RUN(&f, LI(2, 0xC0000001u), LI(3, 33), OP_R(SHL, 1, 2, 3), OP_I(SHLI, 4, 2, 2), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x80000002u);     /* 33 & 31 = 1; C = bit 31 */
    ASSERT_EQ_U32(REG(&f, 4), 0x00000004u);
    ASSERT_EQ_U32(f.vm.cpu.flags, C);           /* último bit a sair (bit 30) = 1 */
}

TEST(shr_and_shri)
{
    fixture f;
    RUN(&f, LI(2, 0x80000003u), LI(3, 1), OP_R(SHR, 1, 2, 3), OP_I(SHRI, 4, 2, 31), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x40000001u);
    ASSERT_EQ_U32(REG(&f, 4), 1u);
    ASSERT_EQ_U32(f.vm.cpu.flags, 0u);          /* bit 30 = 0 saiu por último */
}

TEST(sar_and_sari)
{
    fixture f;
    RUN(&f, LI(2, 0x80000010u), LI(3, 4), OP_R(SAR, 1, 2, 3), OP_I(SARI, 4, 2, 5), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0xF8000001u);
    ASSERT_EQ_U32(REG(&f, 4), 0xFC000000u);
    ASSERT_EQ_U32(f.vm.cpu.flags, N | C);       /* bit 4 saiu por último */
}

TEST(shift_by_zero_clears_carry_keeps_v)
{
    fixture f;
    RUN(&f, SET_FLAGS(C | V), LI(2, 5), OP_I(SHLI, 1, 2, 32), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 5u);
    ASSERT_EQ_U32(f.vm.cpu.flags, V);
}

TEST(rol_rotates_left)
{
    fixture f;
    RUN(&f, LI(2, 0x80000001u), LI(3, 4), OP_R(ROL, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x00000018u);
    ASSERT_EQ_U32(f.vm.cpu.flags, 0u);
    RUN(&f, LI(2, 0x80000000u), LI(3, 1), OP_R(ROL, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 1u);
    ASSERT_EQ_U32(f.vm.cpu.flags, C);
}

TEST(ror_rotates_right)
{
    fixture f;
    RUN(&f, LI(2, 0x00000011u), LI(3, 4), OP_R(ROR, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x10000001u);
    ASSERT_EQ_U32(f.vm.cpu.flags, 0u);
    RUN(&f, LI(2, 1), LI(3, 1), OP_R(ROR, 1, 2, 3), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x80000000u);
    ASSERT_EQ_U32(f.vm.cpu.flags, N | C);
}

TEST(tst_and_tsti_only_set_flags)
{
    fixture f;
    RUN(&f, SET_FLAGS(V), LI(1, 0xF0u), LI(2, 0x0Fu), OP_R(TST, 1, 2, 0), HALT);
    ASSERT_EQ_U32(f.vm.cpu.flags, Z | V);
    ASSERT_EQ_U32(REG(&f, 1), 0xF0u);
    RUN(&f, LI(1, 0x80000000u), OP_I(TSTI, 1, 0, -1), HALT);
    ASSERT_EQ_U32(f.vm.cpu.flags, N);
}

TEST(sxb_sxh)
{
    fixture f;
    RUN(&f, LI(2, 0x12345680u), OP_R(SXB, 1, 2, 0), OP_R(SXH, 3, 2, 0), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0xFFFFFF80u);
    ASSERT_EQ_U32(REG(&f, 3), 0x00005680u);
    ASSERT_EQ_U32(f.vm.cpu.flags, 0u);
}

TEST(zxb_zxh)
{
    fixture f;
    RUN(&f, LI(2, 0xFFFF8080u), OP_R(ZXB, 1, 2, 0), OP_R(ZXH, 3, 2, 0), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x80u);
    ASSERT_EQ_U32(REG(&f, 3), 0x8080u);
}

#if KSVM_ENABLE_BITOPS
TEST(clz)
{
    fixture f;
    RUN(&f, LI(2, 0x00010000u), OP_R(CLZ, 1, 2, 0), OP_R(CLZ, 3, 0, 0), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 15u);
    ASSERT_EQ_U32(REG(&f, 3), 32u);
}

TEST(ctz)
{
    fixture f;
    RUN(&f, LI(2, 0x00010000u), OP_R(CTZ, 1, 2, 0), OP_R(CTZ, 3, 0, 0), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 16u);
    ASSERT_EQ_U32(REG(&f, 3), 32u);
}

TEST(popcnt)
{
    fixture f;
    RUN(&f, LI(2, 0xF00F0001u), OP_R(POPCNT, 1, 2, 0), OP_R(POPCNT, 3, 0, 0), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 9u);
    ASSERT_EQ_U32(REG(&f, 3), 0u);
    ASSERT_EQ_U32(f.vm.cpu.flags, Z);
}
#endif

SUITE(exec_logic)
{
    RUN_TEST(and_forms);
    RUN_TEST(andi_immediate_is_sign_extended);
    RUN_TEST(or_forms);
    RUN_TEST(xor_forms);
    RUN_TEST(not_inverts_rb);
    RUN_TEST(shl_and_shli);
    RUN_TEST(shr_and_shri);
    RUN_TEST(sar_and_sari);
    RUN_TEST(shift_by_zero_clears_carry_keeps_v);
    RUN_TEST(rol_rotates_left);
    RUN_TEST(ror_rotates_right);
    RUN_TEST(tst_and_tsti_only_set_flags);
    RUN_TEST(sxb_sxh);
    RUN_TEST(zxb_zxh);
#if KSVM_ENABLE_BITOPS
    RUN_TEST(clz);
    RUN_TEST(ctz);
    RUN_TEST(popcnt);
#endif
}
