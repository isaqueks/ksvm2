/*
 * test_exec_branch.c — desvios, chamadas e as 16 condições de BCOND
 * (doc 04 §4 e §5.6).
 */
#include "fixture.h"
#include "test.h"

/*
 * Tabela de verdade escrita à mão a partir do doc 04 §4: para cada
 * condição, bit f da máscara = 1 se a condição vale com FLAGS = f
 * (f = Z | N<<1 | C<<2 | V<<3, f = 0..15).
 */
static const uint16_t cond_truth[16] = {
    0xFFFFu,  /* AL  sempre        */
    0xAAAAu,  /* EQ  Z=1           */
    0x5555u,  /* NE  Z=0           */
    0xF0F0u,  /* HS  C=1           */
    0x0F0Fu,  /* LO  C=0           */
    0xCCCCu,  /* MI  N=1           */
    0x3333u,  /* PL  N=0           */
    0xFF00u,  /* VS  V=1           */
    0x00FFu,  /* VC  V=0           */
    0x5050u,  /* HI  C=1 e Z=0     */
    0xAFAFu,  /* LS  C=0 ou Z=1    */
    0xCC33u,  /* GE  N=V           */
    0x33CCu,  /* LT  N!=V          */
    0x4411u,  /* GT  Z=0 e N=V     */
    0xBBEEu,  /* LE  Z=1 ou N!=V   */
    0x0000u   /* NV  nunca         */
};

static int expected(unsigned cond, unsigned flags)
{
    return (cond_truth[cond] >> flags) & 1u;
}

TEST(cond_holds_truth_table)
{
    unsigned cond, flags;
    for (cond = 0; cond < 16; cond++)
    for (flags = 0; flags < 16; flags++) {
        int got = ksvm_cond_holds(flags, (uint8_t)cond) ? 1 : 0;
        if (got != expected(cond, flags)) {
            test_note(t_, "cond=%u flags=0x%X", cond, flags);
        }
        ASSERT_EQ(got, expected(cond, flags));
    }
}

/* Executa BCOND de verdade: tomado pula o MOVI r1, 1. */
TEST(bcond_all_conditions_by_execution)
{
    unsigned cond, flags;
    for (cond = 0; cond < 16; cond++)
    for (flags = 0; flags < 16; flags++) {
        fixture f;
        RUN(&f, SET_FLAGS(flags),                    /* 0..11  */
            ksvm_enc_b((uint8_t)cond, 4),            /* 12     */
            OP_I(MOVI, 1, 0, 1),                     /* 16     */
            HALT);                                   /* 20     */
        ASSERT_EQ(f.vm.status, KSVM_HALTED);
        if ((REG(&f, 1) == 0u) != (expected(cond, flags) == 1)) {
            test_note(t_, "cond=%u flags=0x%X", cond, flags);
        }
        ASSERT_EQ(REG(&f, 1) == 0u, expected(cond, flags) == 1);
        ASSERT_EQ_U32(f.vm.cpu.flags, flags);        /* desvio não altera flags */
    }
}

TEST(bcond_after_cmp_signed_vs_unsigned)
{
    fixture f;
    /* -1 < 1 com sinal (BLT tomado), mas 0xFFFFFFFF > 1 sem sinal (BHI tomado). */
    RUN(&f, LI(1, 0xFFFFFFFFu), LI(2, 1), OP_R(CMP, 1, 2, 0),
        OP_B(LT, 4), OP_I(MOVI, 3, 0, 1),
        OP_B(HI, 4), OP_I(MOVI, 4, 0, 1),
        HALT);
    ASSERT_EQ_U32(REG(&f, 3), 0u);
    ASSERT_EQ_U32(REG(&f, 4), 0u);
}

TEST(jmp_forward_and_backward)
{
    fixture f;
    RUN(&f,
        OP_J(JMP, 8),                   /* 0: -> 12         */
        OP_I(MOVI, 1, 0, 1),            /* 4: pulado        */
        HALT,                           /* 8: fim           */
        OP_I(ADDI, 2, 2, 1),            /* 12: r2++         */
        OP_I(CMPI, 2, 0, 3),            /* 16               */
        OP_B(NE, -12),                  /* 20: -> 12        */
        OP_J(JMP, -24));                /* 24: -> 4 (PC_next 28 - 24) */
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 2), 3u);
    ASSERT_EQ_U32(REG(&f, 1), 1u);      /* o JMP para trás caiu em 4 */
}

TEST(jmpr_jumps_to_register)
{
    fixture f;
    RUN(&f, LI(5, 16), OP_R(JMPR, 0, 5, 0), OP_I(MOVI, 1, 0, 1), OP_I(MOVI, 2, 0, 2), HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 1), 0u);      /* 12 foi pulado */
    ASSERT_EQ_U32(REG(&f, 2), 2u);
}

TEST(call_pushes_return_address_and_ret_pops)
{
    fixture f;
    RUN(&f,
        OP_J(CALL, 8),                  /* 0: -> 12, empilha 4 */
        OP_I(MOVI, 2, 0, 7),            /* 4 */
        HALT,                           /* 8 */
        OP_I(MOVI, 1, 0, 42),           /* 12 */
        OP_R(RET, 0, 0, 0));            /* 16 */
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 1), 42u);
    ASSERT_EQ_U32(REG(&f, 2), 7u);
    ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_END);
    ASSERT_EQ_U32(fix_ram32(&f, FIX_RAM_END - 4u), 4u);
}

TEST(callr_calls_register_target)
{
    fixture f;
    RUN(&f,
        LI(5, 16),                      /* 0 */
        OP_R(CALLR, 0, 5, 0),           /* 8: -> 16, empilha 12 */
        HALT,                           /* 12 */
        OP_I(MOVI, 1, 0, 9),            /* 16 */
        OP_R(RET, 0, 0, 0));            /* 20 */
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 1), 9u);
    ASSERT_EQ_U32(fix_ram32(&f, FIX_RAM_END - 4u), 12u);
}

/* sum(n) = n + sum(n - 1): recursão real com PUSH/POP/CALL/RET. */
TEST(recursive_sum)
{
    fixture f;
    RUN(&f,
        OP_I(MOVI, 1, 0, 10),           /* 0 */
        OP_J(CALL, 4),                  /* 4: -> 12 */
        HALT,                           /* 8 */
        OP_I(CMPI, 1, 0, 0),            /* 12: sum */
        OP_B(NE, 4),                    /* 16: -> 24 */
        OP_R(RET, 0, 0, 0),             /* 20 */
        OP_R(PUSH, 1, 0, 0),            /* 24 */
        OP_I(SUBI, 1, 1, 1),            /* 28 */
        OP_J(CALL, -24),                /* 32: -> 12 */
        OP_R(POP, 2, 0, 0),             /* 36 */
        OP_R(ADD, 1, 1, 2),             /* 40 */
        OP_R(RET, 0, 0, 0));            /* 44 */
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(f.vm.exit_code, 55u);
    ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_END);
}

SUITE(exec_branch)
{
    RUN_TEST(cond_holds_truth_table);
    RUN_TEST(bcond_all_conditions_by_execution);
    RUN_TEST(bcond_after_cmp_signed_vs_unsigned);
    RUN_TEST(jmp_forward_and_backward);
    RUN_TEST(jmpr_jumps_to_register);
    RUN_TEST(call_pushes_return_address_and_ret_pops);
    RUN_TEST(callr_calls_register_target);
    RUN_TEST(recursive_sum);
}
