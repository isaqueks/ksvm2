/*
 * test_exec_mem.c — loads e stores de 8/16/32 bits, formas I e X (doc 04 §5.2).
 */
#include "fixture.h"
#include "test.h"

#define BASE FIX_RAM_BASE

TEST(stw_ldw_roundtrip_little_endian)
{
    fixture f;
    RUN(&f, LI(1, BASE), LI(2, 0x11223344u),
        OP_I(STW, 2, 1, 8), OP_I(LDW, 3, 1, 8), HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 3), 0x11223344u);
    ASSERT_EQ_U32(f.ram[8], 0x44u);
    ASSERT_EQ_U32(f.ram[11], 0x11u);
}

TEST(stb_writes_only_low_byte)
{
    fixture f;
    LOAD(&f, LI(1, BASE), LI(2, 0xABCDEF80u), OP_I(STB, 2, 1, 5), HALT);
    fix_set_ram32(&f, BASE + 4u, 0x11111111u);
    (void)fix_run(&f);
    ASSERT_EQ_U32(fix_ram32(&f, BASE + 4u), 0x11118011u);
}

TEST(ldb_zero_extends)
{
    fixture f;
    LOAD(&f, LI(1, BASE), OP_I(LDB, 3, 1, 2), HALT);
    f.ram[2] = 0x80u;
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 3), 0x80u);
}

TEST(ldbs_sign_extends)
{
    fixture f;
    LOAD(&f, LI(1, BASE), OP_I(LDBS, 3, 1, 2), OP_I(LDBS, 4, 1, 3), HALT);
    f.ram[2] = 0x80u;
    f.ram[3] = 0x7Fu;
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 3), 0xFFFFFF80u);
    ASSERT_EQ_U32(REG(&f, 4), 0x7Fu);
}

TEST(sth_writes_only_low_half)
{
    fixture f;
    LOAD(&f, LI(1, BASE), LI(2, 0x12348001u), OP_I(STH, 2, 1, 1), HALT);
    (void)fix_run(&f);
    ASSERT_EQ_U32(f.ram[0], 0u);
    ASSERT_EQ_U32(f.ram[1], 0x01u);
    ASSERT_EQ_U32(f.ram[2], 0x80u);
    ASSERT_EQ_U32(f.ram[3], 0u);
}

TEST(ldh_zero_extends)
{
    fixture f;
    LOAD(&f, LI(1, BASE), OP_I(LDH, 3, 1, 1), HALT);   /* desalinhado de propósito */
    f.ram[1] = 0x01u;
    f.ram[2] = 0x80u;
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 3), 0x8001u);
}

TEST(ldhs_sign_extends)
{
    fixture f;
    LOAD(&f, LI(1, BASE), OP_I(LDHS, 3, 1, 0), HALT);
    f.ram[0] = 0x01u;
    f.ram[1] = 0x80u;
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 3), 0xFFFF8001u);
}

TEST(ldw_unaligned_and_negative_offset)
{
    fixture f;
    LOAD(&f, LI(1, BASE + 0x100u), OP_I(LDW, 3, 1, -255), OP_I(STW, 3, 1, -0xF9), HALT);
    fix_set_ram32(&f, BASE + 1u, 0xDEADBEEFu);   /* lido em BASE+0x100-255 = BASE+1 */
    (void)fix_run(&f);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 3), 0xDEADBEEFu);
    ASSERT_EQ_U32(fix_ram32(&f, BASE + 7u), 0xDEADBEEFu);   /* escrito em BASE+7 */
}

TEST(indexed_forms_use_rb_plus_rc)
{
    fixture f;
    LOAD(&f, LI(1, BASE), OP_I(MOVI, 2, 0, 16), LI(3, 0xF00DFACEu),
         OP_R(STWX, 3, 1, 2),          /* [r1 + r2] = r3 */
         OP_R(LDWX, 4, 1, 2),
         OP_R(STHX, 3, 1, 0),          /* [r1 + zr] */
         OP_R(LDHX, 5, 1, 0),
         OP_R(LDHSX, 6, 1, 0),
         OP_I(MOVI, 7, 0, 32),
         OP_R(STBX, 3, 1, 7),
         OP_R(LDBX, 8, 1, 7),
         OP_R(LDBSX, 9, 1, 7),
         HALT);
    (void)fix_run(&f);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(fix_ram32(&f, BASE + 16u), 0xF00DFACEu);
    ASSERT_EQ_U32(REG(&f, 4), 0xF00DFACEu);
    ASSERT_EQ_U32(REG(&f, 5), 0xFACEu);
    ASSERT_EQ_U32(REG(&f, 6), 0xFFFFFACEu);
    ASSERT_EQ_U32(f.ram[32], 0xCEu);
    ASSERT_EQ_U32(REG(&f, 8), 0xCEu);
    ASSERT_EQ_U32(REG(&f, 9), 0xFFFFFFCEu);
}

TEST(load_constant_from_code_region)
{
    fixture f;
    RUN(&f, OP_I(LDW, 1, 0, 12), OP_I(LDB, 2, 0, 15), HALT, 0x89ABCDEFu);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 1), 0x89ABCDEFu);
    ASSERT_EQ_U32(REG(&f, 2), 0x89u);
}

TEST(loads_and_stores_keep_flags)
{
    fixture f;
    RUN(&f, SET_FLAGS(0xFu), LI(1, BASE), OP_I(STW, 0, 1, 0), OP_I(LDW, 2, 1, 0), HALT);
    ASSERT_EQ_U32(f.vm.cpu.flags, 0xFu);
    ASSERT_EQ_U32(REG(&f, 2), 0u);
}

SUITE(exec_mem)
{
    RUN_TEST(stw_ldw_roundtrip_little_endian);
    RUN_TEST(stb_writes_only_low_byte);
    RUN_TEST(ldb_zero_extends);
    RUN_TEST(ldbs_sign_extends);
    RUN_TEST(sth_writes_only_low_half);
    RUN_TEST(ldh_zero_extends);
    RUN_TEST(ldhs_sign_extends);
    RUN_TEST(ldw_unaligned_and_negative_offset);
    RUN_TEST(indexed_forms_use_rb_plus_rc);
    RUN_TEST(load_constant_from_code_region);
    RUN_TEST(loads_and_stores_keep_flags);
}
