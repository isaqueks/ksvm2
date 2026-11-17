/*
 * test_exec_sys.c — HALT, TRAP, SYSCALL, BRK, WFI e o ciclo de vida da VM
 * (ksvm_run/ksvm_step, YIELD, instâncias independentes, snapshot, trace).
 */
#include <stdio.h>

#include "fixture.h"
#include "test.h"

TEST(halt_sets_exit_code_and_notifies_hal)
{
    fixture f;
    RUN(&f, LI(1, 77), HALT, OP_I(MOVI, 2, 0, 1));
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(f.vm.exit_code, 77u);
    ASSERT_TRUE(f.hn.halted);
    ASSERT_EQ_U32(f.hn.halt_code, 77u);
    ASSERT_EQ_U32(f.vm.cpu.pc, 12u);            /* HALT concluída: pc avança */
    ASSERT_EQ_U32(f.vm.insn_count, 2u);
    ASSERT_EQ_U32(REG(&f, 2), 0u);
}

TEST(halt_without_hal)
{
    fixture f;
    const uint32_t prog[] = { HALT };
    fix_load_hal(&f, prog, 1, NULL);
    ASSERT_EQ(fix_run(&f), KSVM_HALTED);
}

TEST(trap_faults_with_code)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 1, 0, 1), OP_I(TRAP, 0, 0, -5), HALT);
    ASSERT_EQ(f.vm.status, KSVM_FAULT);
    ASSERT_EQ(f.vm.fault, KSVM_FAULT_TRAP);
    ASSERT_EQ_U32(f.vm.fault_addr, 0xFFFFFFFBu);
    ASSERT_EQ_U32(f.vm.cpu.pc, 4u);             /* pc fica na TRAP */
}

TEST(syscall_instruction_reaches_core_handler)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 1, 0, 'k'), OP_I(SYSCALL, 0, 0, KSVM_SYSCALL_PUTC), HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(f.hn.out_len, 1u);
    ASSERT_EQ_U32(f.hn.out[0], 'k');
    ASSERT_EQ_U32(REG(&f, 2), KSVM_ERR_OK);
}

#if KSVM_ENABLE_TRACE
typedef struct trace_log {
    unsigned steps, brks;
    uint32_t brk_pc, last_pc;
} trace_log;

static void trace_hook(void *ctx, const ksvm *vm, ksvm_trace_event ev, uint32_t pc, uint32_t word)
{
    trace_log *log = (trace_log *)ctx;
    (void)vm;
    (void)word;
    if (ev == KSVM_TRACE_BRK) {
        log->brks++;
        log->brk_pc = pc;
    } else {
        log->steps++;
        log->last_pc = pc;
    }
}
#endif

TEST(brk_continues_execution)
{
    fixture f;
#if KSVM_ENABLE_TRACE
    trace_log log;
    memset(&log, 0, sizeof log);
    LOAD(&f, OP_I(MOVI, 1, 0, 1), OP_R(BRK, 0, 0, 0), OP_I(MOVI, 2, 0, 2), HALT);
    ksvm_set_trace(&f.vm, trace_hook, &log);
    (void)fix_run(&f);
    ASSERT_EQ(log.brks, 1);
    ASSERT_EQ_U32(log.brk_pc, 4u);
    ASSERT_EQ(log.steps, 4);
    ASSERT_EQ_U32(log.last_pc, 12u);
#else
    RUN(&f, OP_I(MOVI, 1, 0, 1), OP_R(BRK, 0, 0, 0), OP_I(MOVI, 2, 0, 2), HALT);
#endif
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 2), 2u);
}

TEST(wfi_yields_and_run_resumes)
{
    fixture f;
    LOAD(&f, OP_I(MOVI, 1, 0, 1), OP_R(WFI, 0, 0, 0), OP_I(MOVI, 2, 0, 2), HALT);
    ASSERT_EQ(fix_run(&f), KSVM_YIELD);
    ASSERT_EQ_U32(f.vm.cpu.pc, 8u);
    ASSERT_EQ_U32(REG(&f, 2), 0u);
    ASSERT_EQ(fix_run(&f), KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 2), 2u);
}

TEST(step_resumes_from_yield)
{
    fixture f;
    LOAD(&f, OP_R(WFI, 0, 0, 0), OP_I(MOVI, 2, 0, 2), HALT);
    ASSERT_EQ(ksvm_step(&f.vm), KSVM_YIELD);
    ASSERT_EQ(ksvm_step(&f.vm), KSVM_RUNNING);
    ASSERT_EQ_U32(REG(&f, 2), 2u);
}

TEST(run_respects_instruction_budget)
{
    fixture f;
    LOAD(&f, OP_I(ADDI, 1, 1, 1), OP_J(JMP, -8));     /* laço infinito */
    ASSERT_EQ(ksvm_run(&f.vm, 100), KSVM_RUNNING);
    ASSERT_EQ_U32(f.vm.insn_count, 100u);
    ASSERT_EQ_U32(REG(&f, 1), 50u);
    ASSERT_EQ(ksvm_run(&f.vm, 1), KSVM_RUNNING);
    ASSERT_EQ_U32(f.vm.insn_count, 101u);
}

TEST(stopped_vm_does_not_step)
{
    fixture f;
    ksvm vm;
    RUN(&f, HALT, OP_I(MOVI, 1, 0, 1));
    ASSERT_EQ(ksvm_step(&f.vm), KSVM_HALTED);
    ASSERT_EQ(ksvm_run(&f.vm, 0), KSVM_HALTED);
    ASSERT_EQ_U32(f.vm.cpu.pc, 4u);
    ASSERT_EQ_U32(REG(&f, 1), 0u);
    ksvm_init(&vm, NULL);                            /* sem reset: parada */
    ASSERT_EQ(ksvm_step(&vm), KSVM_HALTED);
}

TEST(instances_are_independent)
{
    fixture a, b;
    LOAD(&a, OP_I(MOVI, 1, 0, 1), OP_I(ADDI, 1, 1, 1), HALT);
    LOAD(&b, OP_I(MOVI, 1, 0, 100), HALT);
    ASSERT_EQ(fix_steps(&a, 1), KSVM_RUNNING);
    ASSERT_EQ(fix_run(&b), KSVM_HALTED);
    ASSERT_EQ(fix_run(&a), KSVM_HALTED);
    ASSERT_EQ_U32(REG(&a, 1), 2u);
    ASSERT_EQ_U32(REG(&b, 1), 100u);
}

/* struct ksvm não aponta para si mesma: memcpy gera um snapshot válido. */
TEST(snapshot_by_memcpy)
{
    fixture f;
    ksvm copy;
    LOAD(&f, OP_I(MOVI, 1, 0, 3), OP_I(ADDI, 1, 1, 18), OP_I(ADDI, 1, 1, 1), HALT);
    ASSERT_EQ(fix_steps(&f, 1), KSVM_RUNNING);
    memcpy(&copy, &f.vm, sizeof copy);
    ASSERT_EQ(fix_run(&f), KSVM_HALTED);
    ASSERT_EQ(ksvm_run(&copy, 0), KSVM_HALTED);
    ASSERT_EQ_U32(copy.cpu.r[1], 22u);
    ASSERT_EQ_U32(REG(&f, 1), 22u);
}

TEST(report_struct_size)
{
    printf("  sizeof(struct ksvm) = %u bytes (cpu %u, bus %u, regioes %d, ponteiro %u)\n",
           (unsigned)sizeof(ksvm), (unsigned)sizeof(ksvm_cpu), (unsigned)sizeof(ksvm_bus),
           KSVM_MAX_REGIONS, (unsigned)sizeof(void *));
    ASSERT_EQ(sizeof(ksvm_cpu), 136);
    ASSERT_TRUE(sizeof(ksvm) <= 512u);
}

SUITE(exec_sys)
{
    RUN_TEST(halt_sets_exit_code_and_notifies_hal);
    RUN_TEST(halt_without_hal);
    RUN_TEST(trap_faults_with_code);
    RUN_TEST(syscall_instruction_reaches_core_handler);
    RUN_TEST(brk_continues_execution);
    RUN_TEST(wfi_yields_and_run_resumes);
    RUN_TEST(step_resumes_from_yield);
    RUN_TEST(run_respects_instruction_budget);
    RUN_TEST(stopped_vm_does_not_step);
    RUN_TEST(instances_are_independent);
    RUN_TEST(snapshot_by_memcpy);
    RUN_TEST(report_struct_size);
}
