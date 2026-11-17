/*
 * test_faults.c — cada KSVM_FAULT_* provocado de propósito. Depois do
 * fault o estado é inspecionável, a instrução não teve efeito e novos
 * ksvm_step não avançam (doc 09 §2).
 */
#include "fixture.h"
#include "test.h"

/* Confere o fault e que a VM ficou congelada nele. */
static void expect_fault(test_ctx *t_, fixture *f, ksvm_fault fault,
                         uint32_t addr, uint32_t pc)
{
    uint32_t count = f->vm.insn_count;
    ksvm_cpu before = f->vm.cpu;

    ASSERT_EQ(f->vm.status, KSVM_FAULT);
    ASSERT_EQ(f->vm.fault, fault);
    ASSERT_EQ_U32(f->vm.fault_addr, addr);
    ASSERT_EQ_U32(f->vm.cpu.pc, pc);
    ASSERT_EQ(ksvm_step(&f->vm), KSVM_FAULT);
    ASSERT_EQ(ksvm_run(&f->vm, 10), KSVM_FAULT);
    ASSERT_EQ_U32(f->vm.insn_count, count);
    ASSERT_MEM_EQ(&before, &f->vm.cpu, sizeof before);
}

TEST(bad_opcode_ff_erased_flash)
{
    fixture f;
    RUN(&f, 0xFFFFFFFFu);
    expect_fault(t_, &f, KSVM_FAULT_BAD_OPCODE, 0xFFFFFFFFu, 0u);
}

TEST(bad_opcode_unassigned_and_reserved)
{
    static const uint32_t words[] = { 0x04u, 0x09u, 0x2Fu, 0x43u, 0x7Bu, 0x80u, 0xBFu, 0xC0u, 0xF0u };
    size_t i;
    for (i = 0; i < sizeof words / sizeof words[0]; i++) {
        fixture f;
        RUN(&f, OP_I(MOVI, 1, 0, 1), words[i]);
        expect_fault(t_, &f, KSVM_FAULT_BAD_OPCODE, words[i], 4u);
        if (TEST_FAILED()) return;
        ASSERT_EQ_U32(f.vm.insn_count, 1u);
    }
}

TEST(running_off_the_program_hits_erased_code)
{
    fixture f;
    RUN(&f, OP_R(NOP, 0, 0, 0));
    expect_fault(t_, &f, KSVM_FAULT_BAD_OPCODE, 0xFFFFFFFFu, 4u);
}

TEST(mfsr_invalid_sr_is_bad_opcode)
{
    fixture f;
    uint32_t w = OP_I(MFSR, 1, 0, 4);
    RUN(&f, w);
    expect_fault(t_, &f, KSVM_FAULT_BAD_OPCODE, w, 0u);
    ASSERT_EQ_U32(REG(&f, 1), 0u);
}

TEST(mtsr_read_only_sr_is_bad_opcode)
{
    fixture f;
    uint32_t w = OP_I(MTSR, 0, 1, KSVM_SR_PC);
    RUN(&f, OP_I(MOVI, 1, 0, 64), w);
    expect_fault(t_, &f, KSVM_FAULT_BAD_OPCODE, w, 4u);
}

TEST(pc_align)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 5, 0, 2), OP_R(JMPR, 0, 5, 0));
    expect_fault(t_, &f, KSVM_FAULT_PC_ALIGN, 2u, 2u);
    ASSERT_EQ_U32(f.vm.insn_count, 2u);         /* o JMPR em si completou */
}

TEST(pc_range_outside_code)
{
    fixture f;
    RUN(&f, OP_J(JMP, 0x2000 - 4));
    expect_fault(t_, &f, KSVM_FAULT_PC_RANGE, 0x2000u, 0x2000u);
}

TEST(pc_range_ram_is_not_executable)
{
    fixture f;
    RUN(&f, LI(5, FIX_RAM_BASE), OP_R(JMPR, 0, 5, 0));
    expect_fault(t_, &f, KSVM_FAULT_PC_RANGE, FIX_RAM_BASE, FIX_RAM_BASE);
}

TEST(pc_range_on_imm32_word)
{
    fixture f;
    uint32_t prog[FIX_CODE_SIZE / 4u];
    size_t last = FIX_CODE_SIZE / 4u - 1u;
    memset(prog, 0xFF, sizeof prog);
    prog[0] = OP_J(JMP, (int32_t)(last * 4u) - 4);
    prog[last] = ksvm_enc_l(KSVM_OP_MOVIL, 1, 0);   /* imm32 cairia fora do código */
    fix_load(&f, prog, FIX_CODE_SIZE / 4u);
    (void)fix_run(&f);
    expect_fault(t_, &f, KSVM_FAULT_PC_RANGE, FIX_CODE_SIZE, (uint32_t)(last * 4u));
}

/* ---- Limites do fetch (doc 04 §5.6) ------------------------------------
 * A instrução inteira numa única região EXEC e PC_next sem passar de
 * 0xFFFFFFFF; senão PC_RANGE antes de qualquer efeito. */

#define TOP_BASE UINT32_C(0xFFFFFFF0)

/* Fixture + ROM executável de 16 bytes no topo do espaço (termina em 2^32).
 * A palavra 0 do código vale 0x12345678: é o imm32 que um fetch com wrap
 * leria do endereço 0. */
static void top_setup(test_ctx *t_, fixture *f, uint8_t top[16], const uint32_t w[4])
{
    static const uint32_t code0 = 0x12345678u;
    unsigned i;
    for (i = 0; i < 4u; i++) ksvm_st32le(&top[4u * i], w[i]);
    fix_load(f, &code0, 1u);
    ASSERT_EQ(ksvm_map_rom(&f->vm, TOP_BASE, top, 16u, NULL), KSVM_OK);
}

/* Um passo em "pc" que tem de falhar com PC_RANGE em "addr" sem efeito. */
static void expect_fetch_fault(test_ctx *t_, fixture *f, uint32_t pc, uint32_t addr)
{
    f->vm.cpu.pc = pc;
    f->vm.cpu.r[1] = 7u;
    f->vm.cpu.flags = KSVM_FLAG_C;
    ASSERT_EQ(ksvm_step(&f->vm), KSVM_FAULT);
    expect_fault(t_, f, KSVM_FAULT_PC_RANGE, addr, pc);
    if (TEST_FAILED()) return;
    ASSERT_EQ_U32(f->vm.cpu.r[1], 7u);
    ASSERT_EQ_U32(f->vm.cpu.flags, KSVM_FLAG_C);
    ASSERT_EQ_U32(f->vm.insn_count, 0u);
}

TEST(fetch_l_at_top_of_space)
{
    const uint32_t w[4] = { HALT, HALT, HALT, ksvm_enc_l(KSVM_OP_MOVIL, 1, 0) };
    fixture f;
    uint8_t top[16];
    top_setup(t_, &f, top, w);
    if (TEST_FAILED()) return;
    expect_fetch_fault(t_, &f, 0xFFFFFFFCu, 0xFFFFFFFCu);   /* imm32 não vem de 0 */
}

TEST(fetch_pc_next_overflow)
{
    const uint32_t w[4] = { HALT, HALT, ksvm_enc_l(KSVM_OP_ADDL, 1, 1),
                            OP_I(ADDI, 1, 1, 1) };
    fixture f;
    uint8_t top[16];
    top_setup(t_, &f, top, w);
    if (TEST_FAILED()) return;
    expect_fetch_fault(t_, &f, 0xFFFFFFFCu, 0xFFFFFFFCu);   /* 4 bytes: PC_next = 2^32 */
    if (TEST_FAILED()) return;
    top_setup(t_, &f, top, w);
    if (TEST_FAILED()) return;
    expect_fetch_fault(t_, &f, 0xFFFFFFF8u, 0xFFFFFFF8u);   /* L inteira na região, PC_next = 2^32 */
}

TEST(fetch_near_top_is_ok)
{
    const uint32_t w[4] = { OP_I(ADDI, 1, 1, 1), ksvm_enc_l(KSVM_OP_MOVIL, 2, 0), 0xCAFEBABEu,
                            OP_I(ADDI, 1, 1, 1) };
    fixture f;
    uint8_t top[16];
    top_setup(t_, &f, top, w);
    if (TEST_FAILED()) return;
    f.vm.cpu.pc = TOP_BASE;
    ASSERT_EQ(fix_steps(&f, 2), KSVM_RUNNING);
    ASSERT_EQ_U32(REG(&f, 1), 1u);
    ASSERT_EQ_U32(REG(&f, 2), 0xCAFEBABEu);
    ASSERT_EQ_U32(f.vm.cpu.pc, 0xFFFFFFFCu);
    /* 4 bytes em 0xFFFFFFF8 (PC_next = 0xFFFFFFFC) */
    f.vm.cpu.pc = 0xFFFFFFF8u;
    ksvm_st32le(&top[8], OP_I(ADDI, 1, 1, 1));
    ASSERT_EQ(ksvm_step(&f.vm), KSVM_RUNNING);
    ASSERT_EQ_U32(REG(&f, 1), 2u);
    ASSERT_EQ_U32(f.vm.cpu.pc, 0xFFFFFFFCu);
}

/* Código em [0, FIX_CODE_SIZE) e outra ROM executável logo depois. */
static void adjacent_setup(test_ctx *t_, fixture *f, uint8_t next[16], uint32_t last2, uint32_t last,
                           uint32_t n0, uint32_t n1)
{
    uint32_t prog[FIX_CODE_SIZE / 4u];
    memset(prog, 0xFF, sizeof prog);
    prog[FIX_CODE_SIZE / 4u - 2u] = last2;
    prog[FIX_CODE_SIZE / 4u - 1u] = last;
    ksvm_st32le(&next[0], n0);
    ksvm_st32le(&next[4], n1);
    memset(&next[8], 0xFF, 8u);
    fix_load(f, prog, FIX_CODE_SIZE / 4u);
    ASSERT_EQ(ksvm_map_rom(&f->vm, FIX_CODE_SIZE, next, 16u, NULL), KSVM_OK);
}

TEST(fetch_l_split_across_adjacent_exec_regions)
{
    fixture f;
    uint8_t next[16];
    adjacent_setup(t_, &f, next, HALT, ksvm_enc_l(KSVM_OP_MOVIL, 1, 0), 0x11223344u, HALT);
    if (TEST_FAILED()) return;
    expect_fetch_fault(t_, &f, FIX_CODE_SIZE - 4u, FIX_CODE_SIZE);
}

/* L inteira numa região e execução seguindo para a região vizinha: ok. */
TEST(fetch_l_inside_region_then_next_region)
{
    fixture f;
    uint8_t next[16];
    adjacent_setup(t_, &f, next, ksvm_enc_l(KSVM_OP_MOVIL, 1, 0), 0x11223344u,
                   OP_I(ADDI, 1, 1, 1), HALT);
    if (TEST_FAILED()) return;
    f.vm.cpu.pc = FIX_CODE_SIZE - 8u;
    ASSERT_EQ(fix_run(&f), KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 1), 0x11223345u);
    ASSERT_EQ_U32(f.vm.cpu.pc, FIX_CODE_SIZE + 8u);
}

TEST(mem_range_load_unmapped)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 3, 0, 5), LI(1, 0x5000), OP_I(LDW, 3, 1, 4));
    expect_fault(t_, &f, KSVM_FAULT_MEM_RANGE, 0x5004u, 12u);
    ASSERT_EQ_U32(REG(&f, 3), 5u);              /* destino intocado */
}

TEST(mem_range_store_across_ram_end)
{
    fixture f;
    RUN(&f, LI(1, FIX_RAM_END - 2u), LI(2, 0xFFFFFFFFu), OP_I(STW, 2, 1, 0));
    expect_fault(t_, &f, KSVM_FAULT_MEM_RANGE, FIX_RAM_END - 2u, 16u);
    ASSERT_EQ_U32(f.ram[FIX_RAM_SIZE - 2u], 0u);    /* nenhuma escrita parcial */
    ASSERT_EQ_U32(f.ram[FIX_RAM_SIZE - 1u], 0u);
}

TEST(mem_perm_store_to_code)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 2, 0, 0x55), OP_I(STB, 2, 0, 0));
    expect_fault(t_, &f, KSVM_FAULT_MEM_PERM, 0u, 4u);
    ASSERT_EQ_U32(f.code[0], KSVM_OP_MOVI);
}

/* Com KSVM_ENABLE_MULDIV=0 as divisões não existem: BAD_OPCODE. */
TEST(div_zero_all_forms)
{
    static const uint8_t ops[] = { KSVM_OP_DIV, KSVM_OP_DIVU, KSVM_OP_REM, KSVM_OP_REMU };
    size_t i;
    for (i = 0; i < sizeof ops; i++) {
        fixture f;
        uint32_t w = ksvm_enc_r(ops[i], 1, 2, 0);
        RUN(&f, OP_I(MOVI, 1, 0, 9), OP_I(MOVI, 2, 0, 7), w);
#if KSVM_ENABLE_MULDIV
        expect_fault(t_, &f, KSVM_FAULT_DIV_ZERO, 8u, 8u);
#else
        expect_fault(t_, &f, KSVM_FAULT_BAD_OPCODE, w, 8u);
#endif
        if (TEST_FAILED()) return;
        ASSERT_EQ_U32(REG(&f, 1), 9u);
    }
}

TEST(stack_push_below_limit)
{
    fixture f;
    RUN(&f, LI(KSVM_REG_SP, FIX_RAM_BASE), OP_R(PUSH, 1, 0, 0));
    expect_fault(t_, &f, KSVM_FAULT_STACK, FIX_RAM_BASE - 4u, 8u);
    ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_BASE);
}

TEST(stack_call_below_limit)
{
    fixture f;
    RUN(&f, LI(KSVM_REG_SP, FIX_RAM_BASE + 2u), OP_J(CALL, 0));
    expect_fault(t_, &f, KSVM_FAULT_STACK, FIX_RAM_BASE - 2u, 8u);
}

TEST(stack_exactly_at_limit_is_allowed)
{
    fixture f;
    RUN(&f, LI(KSVM_REG_SP, FIX_RAM_BASE + 4u), OP_R(PUSH, 0, 0, 0), HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_BASE);
}

/* Sem limite de pilha, um sp fora de qualquer região falha no store. */
TEST(push_to_unmapped_stack_is_mem_range)
{
    fixture f;
    LOAD(&f, LI(KSVM_REG_SP, 0x5000u), OP_R(PUSH, 1, 0, 0));
    f.vm.stack_limit = 0u;
    (void)fix_run(&f);
    expect_fault(t_, &f, KSVM_FAULT_MEM_RANGE, 0x4FFCu, 8u);
    ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), 0x5000u);
}

TEST(ret_with_empty_stack_is_mem_range)
{
    fixture f;
    RUN(&f, OP_R(RET, 0, 0, 0));
    expect_fault(t_, &f, KSVM_FAULT_MEM_RANGE, FIX_RAM_END, 0u);
    ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_END);
}

TEST(trap)
{
    fixture f;
    RUN(&f, OP_I(TRAP, 0, 0, 8191));
    expect_fault(t_, &f, KSVM_FAULT_TRAP, 8191u, 0u);
}

TEST(fault_names)
{
#if KSVM_FAULT_NAMES
    ASSERT_TRUE(strcmp(ksvm_fault_name(KSVM_FAULT_NONE), "NONE") == 0);
    ASSERT_TRUE(strcmp(ksvm_fault_name(KSVM_FAULT_BAD_OPCODE), "BAD_OPCODE") == 0);
    ASSERT_TRUE(strcmp(ksvm_fault_name(KSVM_FAULT_PC_ALIGN), "PC_ALIGN") == 0);
    ASSERT_TRUE(strcmp(ksvm_fault_name(KSVM_FAULT_PC_RANGE), "PC_RANGE") == 0);
    ASSERT_TRUE(strcmp(ksvm_fault_name(KSVM_FAULT_MEM_RANGE), "MEM_RANGE") == 0);
    ASSERT_TRUE(strcmp(ksvm_fault_name(KSVM_FAULT_MEM_PERM), "MEM_PERM") == 0);
    ASSERT_TRUE(strcmp(ksvm_fault_name(KSVM_FAULT_STACK), "STACK") == 0);
    ASSERT_TRUE(strcmp(ksvm_fault_name(KSVM_FAULT_TRAP), "TRAP") == 0);
#endif
    ASSERT_TRUE(strcmp(ksvm_fault_name(KSVM_FAULT_COUNT), "?") == 0);
}

SUITE(faults)
{
    RUN_TEST(bad_opcode_ff_erased_flash);
    RUN_TEST(bad_opcode_unassigned_and_reserved);
    RUN_TEST(running_off_the_program_hits_erased_code);
    RUN_TEST(mfsr_invalid_sr_is_bad_opcode);
    RUN_TEST(mtsr_read_only_sr_is_bad_opcode);
    RUN_TEST(pc_align);
    RUN_TEST(pc_range_outside_code);
    RUN_TEST(fetch_l_at_top_of_space);
    RUN_TEST(fetch_pc_next_overflow);
    RUN_TEST(fetch_near_top_is_ok);
    RUN_TEST(fetch_l_split_across_adjacent_exec_regions);
    RUN_TEST(fetch_l_inside_region_then_next_region);
    RUN_TEST(pc_range_ram_is_not_executable);
    RUN_TEST(pc_range_on_imm32_word);
    RUN_TEST(mem_range_load_unmapped);
    RUN_TEST(mem_range_store_across_ram_end);
    RUN_TEST(mem_perm_store_to_code);
    RUN_TEST(div_zero_all_forms);
    RUN_TEST(stack_push_below_limit);
    RUN_TEST(stack_call_below_limit);
    RUN_TEST(stack_exactly_at_limit_is_allowed);
    RUN_TEST(push_to_unmapped_stack_is_mem_range);
    RUN_TEST(ret_with_empty_stack_is_mem_range);
    RUN_TEST(trap);
    RUN_TEST(fault_names);
}
