/*
 * test_encode.c — round-trip enc -> decode de todos os formatos com campos
 * nos limites, e coerência da tabela da ISA gerada de isa.def.
 */
#include "ksvm/ksvm.h"
#include "test.h"

TEST(format_r_roundtrip)
{
    static const uint8_t regs[] = { 0, 1, 15, 16, 30, 31 };
    size_t i, j, k;
    ksvm_insn in;
    for (i = 0; i < sizeof regs; i++)
    for (j = 0; j < sizeof regs; j++)
    for (k = 0; k < sizeof regs; k++) {
        uint32_t w = ksvm_enc_r(KSVM_OP_ADD, regs[i], regs[j], regs[k]);
        ASSERT_TRUE(ksvm_decode(w, 0u, &in));
        ASSERT_EQ(in.op, KSVM_OP_ADD);
        ASSERT_EQ(in.ra, regs[i]);
        ASSERT_EQ(in.rb, regs[j]);
        ASSERT_EQ(in.rc, regs[k]);
        ASSERT_EQ(in.format, KSVM_FMT_R);
        ASSERT_EQ(in.len, 4);
        ASSERT_EQ_U32(w >> 23, 0u);                         /* func9 = 0 */
    }
}

TEST(format_i_roundtrip_limits)
{
    static const int32_t imms[] = { -8192, -8191, -1, 0, 1, 8190, 8191 };
    size_t i;
    ksvm_insn in;
    for (i = 0; i < sizeof imms / sizeof imms[0]; i++) {
        uint32_t w = ksvm_enc_i(KSVM_OP_ADDI, 31, 17, imms[i]);
        ASSERT_TRUE(ksvm_decode(w, 0u, &in));
        ASSERT_EQ(in.ra, 31);
        ASSERT_EQ(in.rb, 17);
        ASSERT_EQ(in.format, KSVM_FMT_I);
        ASSERT_EQ(ksvm_u32_to_i32(in.imm), imms[i]);
    }
}

TEST(format_i_uimm14_for_syscall)
{
    uint32_t w = ksvm_enc_i(KSVM_OP_SYSCALL, 0, 0, 0x3FFF);
    ASSERT_EQ_U32(ksvm_field_uimm14(w), 0x3FFFu);
    ASSERT_EQ_U32(ksvm_field_imm14(w), 0xFFFFFFFFu);
}

TEST(format_l_roundtrip)
{
    ksvm_insn in;
    uint32_t w = ksvm_enc_l(KSVM_OP_MOVIL, 7, 3);
    ASSERT_TRUE(ksvm_decode(w, 0xFFFFFFFFu, &in));
    ASSERT_EQ(in.format, KSVM_FMT_L);
    ASSERT_EQ(in.len, 8);
    ASSERT_EQ(in.ra, 7);
    ASSERT_EQ(in.rb, 3);
    ASSERT_EQ_U32(in.imm, 0xFFFFFFFFu);
    ASSERT_EQ_U32(w >> 18, 0u);                             /* reservado = 0 */
}

TEST(format_b_roundtrip_limits)
{
    static const int32_t offs[] = { -(1 << 21), -4, 0, 4, (1 << 21) - 4 };
    size_t i;
    uint8_t c;
    ksvm_insn in;
    for (c = 0; c < 16; c++)
    for (i = 0; i < sizeof offs / sizeof offs[0]; i++) {
        uint32_t w = ksvm_enc_b(c, offs[i]);
        ASSERT_TRUE(ksvm_off20_fits(offs[i]));
        ASSERT_TRUE(ksvm_decode(w, 0u, &in));
        ASSERT_EQ(in.op, KSVM_OP_BCOND);
        ASSERT_EQ(in.format, KSVM_FMT_B);
        ASSERT_EQ(in.cond, c);
        ASSERT_EQ(ksvm_u32_to_i32(in.imm), offs[i]);
    }
}

TEST(format_j_roundtrip_limits)
{
    static const int32_t offs[] = { -(1 << 25), -4, 0, 4, (1 << 25) - 4 };
    size_t i;
    ksvm_insn in;
    for (i = 0; i < sizeof offs / sizeof offs[0]; i++) {
        uint32_t w = ksvm_enc_j(KSVM_OP_CALL, offs[i]);
        ASSERT_TRUE(ksvm_off24_fits(offs[i]));
        ASSERT_TRUE(ksvm_decode(w, 0u, &in));
        ASSERT_EQ(in.op, KSVM_OP_CALL);
        ASSERT_EQ(in.format, KSVM_FMT_J);
        ASSERT_EQ(ksvm_u32_to_i32(in.imm), offs[i]);
    }
}

TEST(fits_reject_out_of_range)
{
    ASSERT_TRUE(ksvm_imm14_fits(-8192));
    ASSERT_FALSE(ksvm_imm14_fits(-8193));
    ASSERT_FALSE(ksvm_imm14_fits(8192));
    ASSERT_FALSE(ksvm_off20_fits(1 << 21));
    ASSERT_FALSE(ksvm_off20_fits(-(1 << 21) - 4));
    ASSERT_FALSE(ksvm_off20_fits(2));                       /* desalinhado */
    ASSERT_FALSE(ksvm_off24_fits(1 << 25));
    ASSERT_FALSE(ksvm_off24_fits(-6));
}

TEST(fields_are_masked)
{
    uint32_t w = ksvm_enc_r(KSVM_OP_MOV, 32, 33, 63);       /* só 5 bits */
    ASSERT_EQ(ksvm_field_ra(w), 0);
    ASSERT_EQ(ksvm_field_rb(w), 1);
    ASSERT_EQ(ksvm_field_rc(w), 31);
    w = ksvm_enc_i(KSVM_OP_MOVI, 1, 0, 8192);               /* imm truncado a 14 bits */
    ASSERT_EQ_U32(ksvm_field_imm14(w), 0xFFFFE000u);
}

TEST(doc_example_addi_encoding)
{
    /* doc 09: ksvm_enc_i(KSVM_OP_ADDI, 2, 1, 1) = r2 = r1 + 1 */
    uint32_t w = ksvm_enc_i(KSVM_OP_ADDI, 2, 1, 1);
    ASSERT_EQ_U32(w, 0x31u | (2u << 8) | (1u << 13) | (1u << 18));
}

TEST(isa_table_matches_def)
{
    unsigned count = 0, valid = 0, op;
#define KSVM_OP(name, code, fmt, fl, desc)                                  \
    count++;                                                                \
    ASSERT_EQ(ksvm_isa_get(code).format, KSVM_FMT_##fmt);                       \
    ASSERT_EQ(ksvm_isa_get(code).len, KSVM_FMT_##fmt == KSVM_FMT_L ? 8 : 4);    \
    ASSERT_EQ(ksvm_isa_get(code).flags, KSVM_AFFECTS_##fl);                     \
    ASSERT_TRUE(KSVM_ISA_NAMES == 0 || strcmp(ksvm_op_name(code), #name) == 0);
#include "ksvm/isa.def"
#undef KSVM_OP
    for (op = 0; op < 256; op++) {
        if (ksvm_isa_get(op).format != KSVM_FMT_INVALID) {
            valid++;
        } else {
            ASSERT_EQ(ksvm_isa_get(op).len, 0);
        }
    }
    ASSERT_EQ(valid, count);
    ASSERT_EQ(count, 103);                     /* opcodes do doc 04: 85 + 18 da extensão F */
}

TEST(reserved_opcodes_invalid)
{
    ksvm_insn in;
    unsigned op;
    for (op = 0x80; op < 0x100; op++) {     /* reservados, resto de F, plataforma, inválidos */
        if (op >= KSVM_OP_FADD && op <= KSVM_OP_FCMP) {
            ASSERT_TRUE(ksvm_decode(op, 0u, &in));      /* extensão F: 0xA0..0xB1 */
            continue;
        }
        ASSERT_FALSE(ksvm_decode(op, 0u, &in));
    }
    ASSERT_FALSE(ksvm_decode(0x09u, 0u, &in));
    ASSERT_FALSE(ksvm_decode(0xFFFFFFFFu, 0u, &in));        /* flash apagada */
}

TEST(names)
{
#if KSVM_ISA_NAMES
    ASSERT_TRUE(strcmp(ksvm_op_name(KSVM_OP_POPCNT), "POPCNT") == 0);
    ASSERT_TRUE(ksvm_op_name(0xFF) == NULL);
    ASSERT_TRUE(strcmp(ksvm_cond_name(KSVM_COND_LE), "LE") == 0);
    ASSERT_TRUE(strcmp(ksvm_cond_name(KSVM_COND_NV), "NV") == 0);
#endif
    ASSERT_TRUE(strcmp(ksvm_fault_name(KSVM_FAULT_DIV_ZERO), KSVM_FAULT_NAMES ? "DIV_ZERO" : "?") == 0);
}

SUITE(encode)
{
    RUN_TEST(format_r_roundtrip);
    RUN_TEST(format_i_roundtrip_limits);
    RUN_TEST(format_i_uimm14_for_syscall);
    RUN_TEST(format_l_roundtrip);
    RUN_TEST(format_b_roundtrip_limits);
    RUN_TEST(format_j_roundtrip_limits);
    RUN_TEST(fits_reject_out_of_range);
    RUN_TEST(fields_are_masked);
    RUN_TEST(doc_example_addi_encoding);
    RUN_TEST(isa_table_matches_def);
    RUN_TEST(reserved_opcodes_invalid);
    RUN_TEST(names);
}
