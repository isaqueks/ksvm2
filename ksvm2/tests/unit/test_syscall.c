/*
 * test_syscall.c — syscalls dos grupos 0, 1, 2 e 5 sobre hal_null,
 * encaminhamento ao host e helpers de acesso à memória do guest (doc 06).
 */
#include "fixture.h"
#include "test.h"

#define SYS(n)      OP_I(SYSCALL, 0, 0, KSVM_SYSCALL_##n)
#define ERR         0xFFFFFFFFu
#define MSG_ADDR    FIX_RAM_BASE

TEST(putc_writes_byte)
{
    fixture f;
    RUN(&f, LI(1, 0x141), SYS(PUTC), HALT);      /* só o byte baixo */
    ASSERT_EQ_U32(f.hn.out_len, 1u);
    ASSERT_EQ_U32(f.hn.out[0], 0x41u);
    ASSERT_EQ_U32(REG(&f, 1), 0u);
    ASSERT_EQ_U32(REG(&f, 2), KSVM_ERR_OK);
}

TEST(puts_writes_guest_buffer)
{
    fixture f;
    LOAD(&f, LI(1, MSG_ADDR), OP_I(MOVI, 2, 0, 5), SYS(PUTS), HALT);
    memcpy(f.ram, "hello", 5);
    (void)fix_run(&f);
    ASSERT_EQ_U32(f.hn.out_len, 5u);
    ASSERT_MEM_EQ(f.hn.out, "hello", 5);
    ASSERT_EQ_U32(REG(&f, 1), 5u);
    ASSERT_EQ_U32(REG(&f, 2), KSVM_ERR_OK);
}

TEST(puts_from_code_region)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 1, 0, 16), OP_I(MOVI, 2, 0, 4), SYS(PUTS), HALT, 0x21494B4Fu);
    ASSERT_MEM_EQ(f.hn.out, "OKI!", 4);
}

TEST(puts_invalid_pointer_is_error_not_fault)
{
    fixture f;
    RUN(&f, LI(1, FIX_RAM_END - 2u), OP_I(MOVI, 2, 0, 5), SYS(PUTS), HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 1), ERR);
    ASSERT_EQ_U32(REG(&f, 2), KSVM_ERR_FAULT);
    ASSERT_EQ_U32(f.hn.out_total, 0u);           /* nada foi escrito */
}

TEST(puts_zero_length)
{
    fixture f;
    RUN(&f, LI(1, 0x7000000u), OP_I(MOVI, 2, 0, 0), SYS(PUTS), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0u);
    ASSERT_EQ_U32(REG(&f, 2), KSVM_ERR_OK);
}

TEST(output_beyond_capture_is_counted)
{
    fixture f;
    RUN(&f, LI(1, 0), LI(2, KSVM_HAL_NULL_OUT_MAX + 10u), SYS(PUTS), HALT);
    ASSERT_EQ_U32(REG(&f, 1), KSVM_HAL_NULL_OUT_MAX + 10u);
    ASSERT_EQ_U32(f.hn.out_len, KSVM_HAL_NULL_OUT_MAX);
    ASSERT_EQ_U32(f.hn.out_total, KSVM_HAL_NULL_OUT_MAX + 10u);
}

TEST(getc_data_nodata_eof)
{
    fixture f;
    static const uint8_t in[1] = { 0xC8 };
    LOAD(&f, SYS(GETC), OP_R(MOV, 10, 1, 0), OP_R(MOV, 11, 2, 0),
         SYS(GETC), OP_R(MOV, 12, 1, 0), OP_R(MOV, 13, 2, 0), HALT);
    ksvm_hal_null_set_input(&f.hn, in, 1, false);
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 10), 0xC8u);
    ASSERT_EQ_U32(REG(&f, 11), KSVM_ERR_OK);
    ASSERT_EQ_U32(REG(&f, 12), ERR);
    ASSERT_EQ_U32(REG(&f, 13), KSVM_ERR_AGAIN);

    LOAD(&f, SYS(GETC), HALT);
    ksvm_hal_null_set_input(&f.hn, in, 0, true);
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 1), ERR);
    ASSERT_EQ_U32(REG(&f, 2), KSVM_ERR_EOF);
}

TEST(gets_stops_at_newline_inclusive)
{
    fixture f;
    static const uint8_t in[] = "ab\ncd";
    LOAD(&f, LI(1, MSG_ADDR), OP_I(MOVI, 2, 0, 16), SYS(GETS), HALT);
    ksvm_hal_null_set_input(&f.hn, in, 5, false);
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 1), 3u);
    ASSERT_MEM_EQ(f.ram, "ab\n", 3);
    ASSERT_EQ_U32(f.hn.in_pos, 3u);
}

TEST(gets_respects_max_and_empty_input)
{
    fixture f;
    static const uint8_t in[] = "abcdef";
    LOAD(&f, LI(1, MSG_ADDR), OP_I(MOVI, 2, 0, 4), SYS(GETS), OP_R(MOV, 10, 1, 0),
         LI(1, MSG_ADDR), OP_I(MOVI, 2, 0, 4), SYS(GETS), HALT);
    ksvm_hal_null_set_input(&f.hn, in, 6, false);
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 10), 4u);
    ASSERT_EQ_U32(REG(&f, 1), 2u);               /* "ef" e depois sem dado */
    ASSERT_MEM_EQ(f.ram, "efcd", 4);

    LOAD(&f, LI(1, MSG_ADDR), OP_I(MOVI, 2, 0, 4), SYS(GETS), HALT);
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 1), 0u);               /* nada disponível */
    LOAD(&f, LI(1, MSG_ADDR), OP_I(MOVI, 2, 0, 4), SYS(GETS), HALT);
    ksvm_hal_null_set_input(&f.hn, in, 0, true);
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 1), ERR);
    ASSERT_EQ_U32(REG(&f, 2), KSVM_ERR_EOF);
}

TEST(gets_invalid_buffer)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 1, 0, 0), OP_I(MOVI, 2, 0, 4), SYS(GETS), HALT);   /* código: só leitura */
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 2), KSVM_ERR_FAULT);
}

TEST(exit_halts_with_code)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 1, 0, 3), SYS(EXIT), OP_I(MOVI, 1, 0, 9), HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(f.vm.exit_code, 3u);
    ASSERT_TRUE(f.hn.halted);
    ASSERT_EQ_U32(f.vm.cpu.pc, 8u);
}

TEST(caps_reports_core_syscalls)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 1, 0, KSVM_SYSCALL_PUTS), SYS(CAPS), OP_R(MOV, 10, 1, 0),
        OP_I(MOVI, 1, 0, KSVM_SYSCALL_OPEN), SYS(CAPS), OP_R(MOV, 11, 1, 0),
        OP_I(MOVI, 1, 0, KSVM_SYSCALL_RANDOM), SYS(CAPS), HALT);
    ASSERT_EQ_U32(REG(&f, 10), 1u);
    ASSERT_EQ_U32(REG(&f, 11), 0u);
    ASSERT_EQ_U32(REG(&f, 1), 1u);
}

TEST(caps_without_hal_services)
{
    fixture f;
    const uint32_t prog[] = { OP_I(MOVI, 1, 0, KSVM_SYSCALL_PUTC), SYS(CAPS), HALT };
    fix_load_hal(&f, prog, 3, NULL);
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 1), 0u);
}

TEST(missing_hal_services_return_nosys)
{
    fixture f;
    const uint32_t prog[] = { SYS(PUTC), OP_R(MOV, 10, 2, 0), SYS(GETS), OP_R(MOV, 11, 2, 0),
                              SYS(MILLIS), HALT };
    fix_load_hal(&f, prog, sizeof prog / sizeof prog[0], NULL);
    ASSERT_EQ(fix_run(&f), KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 10), KSVM_ERR_NOSYS);
    ASSERT_EQ_U32(REG(&f, 11), KSVM_ERR_NOSYS);
    ASSERT_EQ_U32(REG(&f, 1), ERR);
    ASSERT_EQ_U32(REG(&f, 2), KSVM_ERR_NOSYS);
}

static int failing_putc(void *ctx, uint8_t c)
{
    (void)ctx;
    (void)c;
    return -1;
}

TEST(putc_failure_reports_io)
{
    fixture f;
    LOAD(&f, SYS(PUTC), OP_R(MOV, 10, 1, 0), OP_R(MOV, 11, 2, 0),
         OP_I(MOVI, 1, 0, 0), OP_I(MOVI, 2, 0, 4), SYS(PUTS), HALT);
    f.hal.putc = failing_putc;
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 10), ERR);
    ASSERT_EQ_U32(REG(&f, 11), KSVM_ERR_IO);
    ASSERT_EQ_U32(REG(&f, 1), ERR);
    ASSERT_EQ_U32(REG(&f, 2), KSVM_ERR_IO);
}

TEST(caps_follows_hal_services)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 1, 0, KSVM_SYSCALL_GETS), SYS(CAPS), OP_R(MOV, 10, 1, 0),
        OP_I(MOVI, 1, 0, KSVM_SYSCALL_MILLIS), SYS(CAPS), HALT);
    ASSERT_EQ_U32(REG(&f, 10), 1u);
    ASSERT_EQ_U32(REG(&f, 1), 1u);
    LOAD(&f, OP_I(MOVI, 1, 0, KSVM_SYSCALL_GETC), SYS(CAPS), HALT);
    f.hal.getc = NULL;
    (void)fix_run(&f);
    ASSERT_EQ_U32(REG(&f, 1), 0u);
}

TEST(version)
{
    fixture f;
    RUN(&f, SYS(VERSION), HALT);
    ASSERT_EQ_U32(REG(&f, 1), 0x020000u);
}

TEST(yield_returns_control)
{
    fixture f;
    LOAD(&f, SYS(YIELD), OP_I(MOVI, 3, 0, 1), HALT);
    ASSERT_EQ(fix_run(&f), KSVM_YIELD);
    ASSERT_EQ_U32(f.vm.cpu.pc, 4u);
    ASSERT_EQ(fix_run(&f), KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 3), 1u);
}

TEST(abort_is_trap_fault)
{
    fixture f;
    RUN(&f, OP_I(MOVI, 1, 0, 42), SYS(ABORT), HALT);
    ASSERT_EQ(f.vm.status, KSVM_FAULT);
    ASSERT_EQ(f.vm.fault, KSVM_FAULT_TRAP);
    ASSERT_EQ_U32(f.vm.fault_addr, 42u);
    ASSERT_EQ_U32(f.vm.cpu.pc, 4u);
}

TEST(millis_and_sleep_with_hal_null)
{
    fixture f;
    RUN(&f, OP_R(NOP, 0, 0, 0), SYS(MILLIS), OP_R(MOV, 10, 1, 0),
        OP_I(MOVI, 1, 0, 1000), SYS(SLEEP_MS), SYS(MILLIS), HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 10), 1u);              /* 1 instrução antes */
    ASSERT_EQ_U32(f.hn.slept_ms, 1000u);
    ASSERT_EQ_U32(REG(&f, 1), 5u + 1000u);
}

TEST(sleep_without_hal_yields)
{
    fixture f;
    const uint32_t prog[] = { OP_I(MOVI, 1, 0, 10), SYS(SLEEP_MS), HALT };
    fix_load_hal(&f, prog, 3, NULL);
    ASSERT_EQ(fix_run(&f), KSVM_YIELD);
    ASSERT_EQ_U32(REG(&f, 1), 0u);
    ASSERT_EQ(fix_run(&f), KSVM_HALTED);
}

TEST(mem_info_reports_layout)
{
    fixture f;
    RUN(&f, SYS(MEM_INFO), HALT);
    ASSERT_EQ_U32(REG(&f, 1), FIX_RAM_BASE);
    ASSERT_EQ_U32(REG(&f, 2), FIX_RAM_SIZE);
    ASSERT_EQ_U32(REG(&f, 3), FIX_RAM_BASE);
    ASSERT_EQ_U32(REG(&f, 4), FIX_RAM_BASE);
}

TEST(random_is_deterministic_xorshift)
{
    fixture f;
    uint32_t x = KSVM_RNG_SEED;
    RUN(&f, SYS(RANDOM), OP_R(MOV, 10, 1, 0), SYS(RANDOM), HALT);
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    ASSERT_EQ_U32(REG(&f, 10), x);
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    ASSERT_EQ_U32(REG(&f, 1), x);
    ASSERT_TRUE(REG(&f, 1) != REG(&f, 10));
}

TEST(unknown_syscall_is_nosys)
{
    fixture f;
    RUN(&f, OP_I(SYSCALL, 0, 0, 0x05), OP_R(MOV, 10, 2, 0), OP_I(SYSCALL, 0, 0, 0x3FFF), HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 10), KSVM_ERR_NOSYS);
    ASSERT_EQ_U32(REG(&f, 1), ERR);
    ASSERT_EQ_U32(REG(&f, 2), KSVM_ERR_NOSYS);
}

/* Host que implementa 0x3FFF (soma r1..r6), recusa 0x100 e cede em 0x101. */
static int host_syscall(ksvm *vm, uint32_t num)
{
    unsigned i;
    uint32_t sum = 0;
    switch (num) {
    case 0x3FFFu:
        for (i = 1; i <= 7; i++) {
            sum += ksvm_arg(vm, i);              /* arg 7 não existe: 0 */
        }
        ksvm_ret(vm, sum, KSVM_ERR_OK);
        return KSVM_SYS_OK;
    case 0x101u:
        return KSVM_SYS_YIELD;
    default:
        return KSVM_SYS_UNKNOWN;
    }
}

TEST(forwarded_to_host_handler)
{
    fixture f;
    LOAD(&f, OP_I(MOVI, 1, 0, 1), OP_I(MOVI, 2, 0, 2), OP_I(MOVI, 3, 0, 3),
         OP_I(MOVI, 4, 0, 4), OP_I(MOVI, 5, 0, 5), OP_I(MOVI, 6, 0, 6), OP_I(MOVI, 7, 0, 100),
         OP_I(SYSCALL, 0, 0, 0x3FFF), OP_R(MOV, 10, 1, 0),
         OP_I(SYSCALL, 0, 0, 0x100), OP_R(MOV, 11, 2, 0),
         OP_I(SYSCALL, 0, 0, 0x101), HALT);
    f.hal.syscall = host_syscall;
    ASSERT_EQ(fix_run(&f), KSVM_YIELD);
    ASSERT_EQ_U32(REG(&f, 10), 21u);
    ASSERT_EQ_U32(REG(&f, 11), KSVM_ERR_NOSYS);
    ASSERT_EQ(fix_run(&f), KSVM_HALTED);
}

TEST(copy_helpers_validate_ranges)
{
    fixture f;
    uint8_t buf[8] = { 0 };
    LOAD(&f, HALT);
    ASSERT_EQ(ksvm_copy_to_guest(&f.vm, FIX_RAM_BASE + 4u, "abcd", 4u), KSVM_OK);
    ASSERT_MEM_EQ(f.ram + 4, "abcd", 4);
    ASSERT_EQ(ksvm_copy_from_guest(&f.vm, buf, FIX_RAM_BASE + 4u, 4u), KSVM_OK);
    ASSERT_MEM_EQ(buf, "abcd", 4);
    ASSERT_EQ(ksvm_copy_from_guest(&f.vm, buf, 0u, 4u), KSVM_OK);          /* código: legível */
    ASSERT_EQ(ksvm_copy_to_guest(&f.vm, 0u, "x", 1u), KSVM_E_RANGE);      /* código: não gravável */
    ASSERT_EQ(ksvm_copy_to_guest(&f.vm, FIX_RAM_END - 2u, "abcd", 4u), KSVM_E_RANGE);
    ASSERT_EQ_U32(f.ram[FIX_RAM_SIZE - 2u], 0u);                            /* nada copiado */
    ASSERT_EQ(ksvm_copy_from_guest(&f.vm, buf, 0x9000000u, 1u), KSVM_E_RANGE);
    ASSERT_EQ(ksvm_copy_from_guest(&f.vm, buf, 0x9000000u, 0u), KSVM_OK);
}

TEST(guest_ptr_only_for_direct_ram)
{
    fixture f;
    LOAD(&f, HALT);
    ASSERT_TRUE(ksvm_guest_ptr(&f.vm, FIX_RAM_BASE + 8u, 8u, KSVM_MEM_WRITE) == f.ram + 8);
    ASSERT_TRUE(ksvm_guest_ptr(&f.vm, FIX_RAM_END - 4u, 8u, KSVM_MEM_READ) == NULL);
    ASSERT_TRUE(ksvm_guest_ptr(&f.vm, 0u, 4u, KSVM_MEM_READ) == NULL);    /* ROM */
    ASSERT_TRUE(ksvm_guest_ptr(&f.vm, FIX_RAM_BASE, 0u, KSVM_MEM_READ) == NULL);
}

TEST(arg_and_ret_helpers)
{
    fixture f;
    LOAD(&f, HALT);
    f.vm.cpu.r[6] = 66u;
    f.vm.cpu.r[7] = 77u;
    ASSERT_EQ_U32(ksvm_arg(&f.vm, 6), 66u);
    ASSERT_EQ_U32(ksvm_arg(&f.vm, 7), 0u);
    ASSERT_EQ_U32(ksvm_arg(&f.vm, 0), 0u);
    ksvm_ret(&f.vm, 5u, 9u);
    ASSERT_EQ_U32(f.vm.cpu.r[1], 5u);
    ASSERT_EQ_U32(f.vm.cpu.r[2], 9u);
}

SUITE(syscall)
{
    RUN_TEST(putc_writes_byte);
    RUN_TEST(puts_writes_guest_buffer);
    RUN_TEST(puts_from_code_region);
    RUN_TEST(puts_invalid_pointer_is_error_not_fault);
    RUN_TEST(puts_zero_length);
    RUN_TEST(output_beyond_capture_is_counted);
    RUN_TEST(getc_data_nodata_eof);
    RUN_TEST(gets_stops_at_newline_inclusive);
    RUN_TEST(gets_respects_max_and_empty_input);
    RUN_TEST(gets_invalid_buffer);
    RUN_TEST(exit_halts_with_code);
    RUN_TEST(caps_reports_core_syscalls);
    RUN_TEST(caps_without_hal_services);
    RUN_TEST(missing_hal_services_return_nosys);
    RUN_TEST(putc_failure_reports_io);
    RUN_TEST(caps_follows_hal_services);
    RUN_TEST(version);
    RUN_TEST(yield_returns_control);
    RUN_TEST(abort_is_trap_fault);
    RUN_TEST(millis_and_sleep_with_hal_null);
    RUN_TEST(sleep_without_hal_yields);
    RUN_TEST(mem_info_reports_layout);
    RUN_TEST(random_is_deterministic_xorshift);
    RUN_TEST(unknown_syscall_is_nosys);
    RUN_TEST(forwarded_to_host_handler);
    RUN_TEST(copy_helpers_validate_ranges);
    RUN_TEST(guest_ptr_only_for_direct_ram);
    RUN_TEST(arg_and_ret_helpers);
}
