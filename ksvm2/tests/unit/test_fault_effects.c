/*
 * test_fault_effects.c — efeito de cada KSVM_FAULT_* (doc 04 / fault.h:
 * "a instrução que falhou não tem efeito, o PC continua apontando para
 * ela").
 *
 * Para cada cenário: snapshot de struct ksvm inteira + RAM + código + estado
 * da HAL; provoca o fault; confere que SÓ status/fault/fault_addr mudaram;
 * depois ksvm_step e ksvm_run não avançam e não mudam mais nada (nem
 * insn_count, nem pc). Cobre todos os opcodes que podem falhar, incluindo
 * todos os 256 valores do byte de opcode.
 */
#include "conform.h"

/* MMIO só-leitura (sem write8) e só-escrita (sem read8) para MEM_PERM. */
static unsigned g_mmio_writes;
#if KSVM_MAX_REGIONS >= 4
static uint8_t mmio_rd(ksvm_region *r, uint32_t off) { (void)r; return (uint8_t)off; }
static void mmio_wr(ksvm_region *r, uint32_t off, uint8_t v)
{
    (void)r; (void)off; (void)v;
    g_mmio_writes++;
}
#endif

#define MMIO_RO 0x20000u
#define MMIO_WO 0x30000u

typedef struct fx_snapshot {
    ksvm          vm;
    uint8_t       ram[FIX_RAM_SIZE];
    uint8_t       code[FIX_CODE_SIZE];
    ksvm_hal_null hn;
} fx_snapshot;

static void take(fx_snapshot *s, const fixture *f)
{
    memcpy(&s->vm, &f->vm, sizeof s->vm);
    memcpy(s->ram, f->ram, sizeof s->ram);
    memcpy(s->code, f->code, sizeof s->code);
    memcpy(&s->hn, &f->hn, sizeof s->hn);
}

/* Estado completo da VM igual ao snapshot, exceto o que se espera mudar. */
static void same_as(test_ctx *t_, const fx_snapshot *s, const fixture *f)
{
    ASSERT_MEM_EQ(&s->vm.cpu, &f->vm.cpu, sizeof s->vm.cpu);
    ASSERT_MEM_EQ(&s->vm.bus, &f->vm.bus, sizeof s->vm.bus);
    ASSERT_TRUE(s->vm.hal == f->vm.hal);
    ASSERT_EQ_U32(s->vm.exit_code, f->vm.exit_code);
    ASSERT_EQ_U32(s->vm.stack_limit, f->vm.stack_limit);
    ASSERT_EQ_U32(s->vm.insn_count, f->vm.insn_count);
    ASSERT_EQ_U32(s->vm.heap_start, f->vm.heap_start);
    ASSERT_EQ_U32(s->vm.rng_state, f->vm.rng_state);
    ASSERT_MEM_EQ(s->ram, f->ram, sizeof s->ram);
    ASSERT_MEM_EQ(s->code, f->code, sizeof s->code);
    ASSERT_MEM_EQ(&s->hn, &f->hn, sizeof s->hn);
}

/* Provoca e confere um fault a partir do estado atual de f. */
static void provoke(test_ctx *t_, fixture *f, const char *what, ksvm_fault fault, uint32_t addr)
{
    fx_snapshot before, after;
    unsigned mmio_before = g_mmio_writes;

    take(&before, f);
    (void)ksvm_step(&f->vm);
    if (f->vm.status != KSVM_FAULT || f->vm.fault != fault || f->vm.fault_addr != addr) {
        test_note(t_, "%s: status=%d fault=%s addr=0x%08lX; esperado %s addr=0x%08lX", what,
                  (int)f->vm.status, ksvm_fault_name(f->vm.fault),
                  (unsigned long)f->vm.fault_addr, ksvm_fault_name(fault), (unsigned long)addr);
    }
    ASSERT_EQ(f->vm.status, KSVM_FAULT);
    ASSERT_EQ(f->vm.fault, fault);
    ASSERT_EQ_U32(f->vm.fault_addr, addr);
    same_as(t_, &before, f);
    if (TEST_FAILED()) {
        test_note(t_, "%s: o fault teve efeito colateral", what);
        return;
    }
    ASSERT_EQ(g_mmio_writes, mmio_before);

    /* congelada: novos passos não mudam NADA (agora nem status) */
    take(&after, f);
    ASSERT_EQ(ksvm_step(&f->vm), KSVM_FAULT);
    ASSERT_EQ(ksvm_run(&f->vm, 100), KSVM_FAULT);
    ASSERT_EQ(ksvm_run(&f->vm, 0), KSVM_FAULT);
    same_as(t_, &after, f);
    ASSERT_EQ(f->vm.fault, fault);
    ASSERT_EQ_U32(f->vm.fault_addr, addr);
}

/* Fixture com "w0, w1" em pc, sentinelas, flags e MMIO mapeado. */
static void setup(fixture *f, uint32_t pc, uint32_t w0, uint32_t w1)
{
    uint32_t prog[FIX_CODE_SIZE / 4u];
    unsigned i;

    memset(prog, 0xFF, sizeof prog);
    prog[pc / 4u] = w0;
    if (pc / 4u + 1u < FIX_CODE_SIZE / 4u) prog[pc / 4u + 1u] = w1;
    fix_load(f, prog, FIX_CODE_SIZE / 4u);
#if KSVM_MAX_REGIONS >= 4
    (void)ksvm_map_mmio(&f->vm, MMIO_RO, 16u, NULL, mmio_rd, NULL);
    (void)ksvm_map_mmio(&f->vm, MMIO_WO, 16u, NULL, NULL, mmio_wr);
#endif
    for (i = 1; i < 31u; i++) f->vm.cpu.r[i] = cf_sentinel(i);
    for (i = 0; i < FIX_RAM_SIZE; i++) f->ram[i] = (uint8_t)(i ^ 0x5Au);
    f->vm.cpu.pc = pc;
    f->vm.cpu.flags = KSVM_FLAG_N | KSVM_FLAG_V;
    f->vm.insn_count = 1234u;
    f->vm.exit_code = 99u;
}

/* ---- BAD_OPCODE ------------------------------------------------------- */

/* Os 256 valores do byte de opcode: os que não estão na tabela (e os não
 * implementados PUSHM/POPM) falham sem efeito, com campos aleatórios. */
TEST(bad_opcode_every_unassigned_byte)
{
    unsigned op, k;
    for (op = 0; op < 256u; op++) {
        int bad = ksvm_isa_get(op).format == KSVM_FMT_INVALID ||
                  op == KSVM_OP_PUSHM || op == KSVM_OP_POPM;
#if !KSVM_ENABLE_MULDIV
        bad |= (op >= KSVM_OP_MUL && op <= KSVM_OP_REMU);
#endif
#if !KSVM_ENABLE_BITOPS
        bad |= (op >= KSVM_OP_CLZ && op <= KSVM_OP_POPCNT);
#endif
#if !KSVM_ENABLE_FLOAT
        bad |= (op >= KSVM_OP_FADD && op <= KSVM_OP_FCMP);
#endif
        if (!bad) continue;
        for (k = 0; k < 3u; k++) {
            static const uint32_t hi[3] = { 0x00000000u, 0xFFFFFF00u, 0x5A5A5A00u };
            fixture f;
            uint32_t w = hi[k] | op;
            setup(&f, 8u, w, 0u);
            provoke(t_, &f, "opcode invalido", KSVM_FAULT_BAD_OPCODE, w);
            if (TEST_FAILED()) {
                test_note(t_, "opcode 0x%02X", op);
                return;
            }
            ASSERT_EQ_U32(f.vm.cpu.pc, 8u);
        }
    }
}

TEST(bad_opcode_invalid_special_registers)
{
    static const int32_t srs[] = { 4, 100, 8191, -1, -8192 };
    unsigned i;
    for (i = 0; i < sizeof srs / sizeof srs[0]; i++) {
        fixture f;
        uint32_t w = OP_I(MFSR, 5, 0, srs[i]);
        setup(&f, 0u, w, 0u);
        provoke(t_, &f, "MFSR", KSVM_FAULT_BAD_OPCODE, w);
        if (TEST_FAILED()) return;
        w = OP_I(MTSR, 0, 5, i == 0 ? (int32_t)KSVM_SR_PC : srs[i]);
        setup(&f, 0u, w, 0u);
        provoke(t_, &f, "MTSR", KSVM_FAULT_BAD_OPCODE, w);
        if (TEST_FAILED()) return;
    }
}

/* ---- PC_ALIGN / PC_RANGE ---------------------------------------------- */

TEST(pc_align_every_misalignment)
{
    uint32_t a;
    for (a = 1; a < 4u; a++) {
        fixture f;
        setup(&f, 0u, HALT, HALT);
        f.vm.cpu.pc = 0x10u + a;
        provoke(t_, &f, "pc desalinhado", KSVM_FAULT_PC_ALIGN, 0x10u + a);
        if (TEST_FAILED()) return;
        ASSERT_EQ_U32(f.vm.cpu.pc, 0x10u + a);
    }
    {
        /* desalinhado E fora de região: alinhamento é checado primeiro */
        fixture f;
        setup(&f, 0u, HALT, HALT);
        f.vm.cpu.pc = 0xFFFFFFFFu;
        provoke(t_, &f, "pc 0xFFFFFFFF", KSVM_FAULT_PC_ALIGN, 0xFFFFFFFFu);
    }
}

TEST(pc_range_cases)
{
    fixture f;
    setup(&f, 0u, HALT, HALT);
    f.vm.cpu.pc = FIX_RAM_BASE;                      /* RAM: sem EXEC */
    provoke(t_, &f, "pc na RAM", KSVM_FAULT_PC_RANGE, FIX_RAM_BASE);
    if (TEST_FAILED()) return;

    setup(&f, 0u, HALT, HALT);
    f.vm.cpu.pc = FIX_CODE_SIZE;                     /* logo após o código */
    provoke(t_, &f, "pc apos ROM", KSVM_FAULT_PC_RANGE, FIX_CODE_SIZE);
    if (TEST_FAILED()) return;

    setup(&f, 0u, HALT, HALT);
    f.vm.cpu.pc = 0xFFFFFFFCu;
    provoke(t_, &f, "pc topo", KSVM_FAULT_PC_RANGE, 0xFFFFFFFCu);
    if (TEST_FAILED()) return;

#if KSVM_MAX_REGIONS >= 4
    setup(&f, 0u, HALT, HALT);
    f.vm.cpu.pc = MMIO_RO;                           /* MMIO legível, não EXEC */
    provoke(t_, &f, "pc no MMIO", KSVM_FAULT_PC_RANGE, MMIO_RO);
    if (TEST_FAILED()) return;
#endif

    /* toda instrução de formato L na última palavra da ROM: imm32 fora */
    {
        unsigned op;
        for (op = 0; op < 256u; op++) {
            if (ksvm_isa_get(op).len != 8u) continue;
            setup(&f, FIX_CODE_SIZE - 4u, ksvm_enc_l((uint8_t)op, 1, 2), 0u);
            provoke(t_, &f, ksvm_op_name((uint8_t)op), KSVM_FAULT_PC_RANGE, FIX_CODE_SIZE);
            if (TEST_FAILED()) return;
            ASSERT_EQ_U32(f.vm.cpu.pc, FIX_CODE_SIZE - 4u);
        }
    }
}

/* ---- MEM_RANGE / MEM_PERM --------------------------------------------- */

static const uint8_t load_ops[] = {
    KSVM_OP_LDB, KSVM_OP_LDBS, KSVM_OP_LDH, KSVM_OP_LDHS, KSVM_OP_LDW,
    KSVM_OP_LDBX, KSVM_OP_LDBSX, KSVM_OP_LDHX, KSVM_OP_LDHSX, KSVM_OP_LDWX,
};
static const uint8_t store_ops[] = {
    KSVM_OP_STB, KSVM_OP_STH, KSVM_OP_STW, KSVM_OP_STBX, KSVM_OP_STHX, KSVM_OP_STWX,
};

static uint32_t mem_insn(uint8_t op, uint8_t rd)
{
    return ksvm_isa_get(op).format == KSVM_FMT_R ? ksvm_enc_r(op, rd, 2, 3)
                                             : ksvm_enc_i(op, rd, 2, -4);
}

/* EA = r2 + (-4 | r3). */
static void set_ea(fixture *f, uint8_t op, uint32_t ea)
{
    if (ksvm_isa_get(op).format == KSVM_FMT_R) {
        f->vm.cpu.r[2] = ea - 0x30u;
        f->vm.cpu.r[3] = 0x30u;
    } else {
        f->vm.cpu.r[2] = ea + 4u;
    }
}

TEST(mem_range_every_load_and_store)
{
    static const uint32_t eas[] = { 0x5000u, FIX_RAM_END, FIX_RAM_BASE - 1u, 0xFFFFFFFFu,
                                    FIX_CODE_SIZE };
    unsigned i, e;
    for (e = 0; e < sizeof eas / sizeof eas[0]; e++) {
        for (i = 0; i < sizeof load_ops; i++) {
            fixture f;
            setup(&f, 4u, mem_insn(load_ops[i], 7), 0u);
            set_ea(&f, load_ops[i], eas[e]);
            provoke(t_, &f, ksvm_op_name(load_ops[i]), KSVM_FAULT_MEM_RANGE, eas[e]);
            if (TEST_FAILED()) return;
        }
        for (i = 0; i < sizeof store_ops; i++) {
            fixture f;
            setup(&f, 4u, mem_insn(store_ops[i], 7), 0u);
            set_ea(&f, store_ops[i], eas[e]);
            provoke(t_, &f, ksvm_op_name(store_ops[i]), KSVM_FAULT_MEM_RANGE, eas[e]);
            if (TEST_FAILED()) return;
        }
    }
}

TEST(mem_perm_stores_to_rom_and_mmio)
{
    unsigned i;
    for (i = 0; i < sizeof store_ops; i++) {
        fixture f;
        setup(&f, 4u, mem_insn(store_ops[i], 7), 0u);
        set_ea(&f, store_ops[i], 0x10u);
        provoke(t_, &f, ksvm_op_name(store_ops[i]), KSVM_FAULT_MEM_PERM, 0x10u);
        if (TEST_FAILED()) return;
#if KSVM_MAX_REGIONS >= 4
        setup(&f, 4u, mem_insn(store_ops[i], 7), 0u);
        set_ea(&f, store_ops[i], MMIO_RO + 4u);
        provoke(t_, &f, "store MMIO RO", KSVM_FAULT_MEM_PERM, MMIO_RO + 4u);
        if (TEST_FAILED()) return;
#endif
    }
#if KSVM_MAX_REGIONS >= 4
    for (i = 0; i < sizeof load_ops; i++) {
        fixture f;
        setup(&f, 4u, mem_insn(load_ops[i], 7), 0u);
        set_ea(&f, load_ops[i], MMIO_WO);
        provoke(t_, &f, "load MMIO WO", KSVM_FAULT_MEM_PERM, MMIO_WO);
        if (TEST_FAILED()) return;
    }
#endif
}

/* Pilha fora de região / em ROM sem stack_limit: PUSH, POP, CALL, CALLR,
 * RET falham sem mexer em sp. */
TEST(stack_ops_memory_faults)
{
    static const struct { uint32_t sp; ksvm_fault push_f; uint32_t push_a; ksvm_fault pop_f; } t[] = {
        { 0x5000u, KSVM_FAULT_MEM_RANGE, 0x4FFCu, KSVM_FAULT_MEM_RANGE },
        { 0x100u,  KSVM_FAULT_MEM_PERM,  0xFCu,   KSVM_FAULT_NONE },     /* ROM: lê, não escreve */
        { FIX_RAM_BASE + 2u, KSVM_FAULT_MEM_RANGE, FIX_RAM_BASE - 2u, KSVM_FAULT_NONE },
        { FIX_RAM_END, KSVM_FAULT_NONE, 0u, KSVM_FAULT_MEM_RANGE },
        { FIX_RAM_END - 2u, KSVM_FAULT_NONE, 0u, KSVM_FAULT_MEM_RANGE },
        { 0u, KSVM_FAULT_MEM_RANGE, 0xFFFFFFFCu, KSVM_FAULT_NONE },        /* wrap: 0 - 4 */
    };
    unsigned i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        fixture f;
        uint32_t pushers[3];
        unsigned k;
        pushers[0] = OP_R(PUSH, 5, 0, 0);
        pushers[1] = OP_J(CALL, 64);
        pushers[2] = OP_R(CALLR, 0, 6, 0);
        if (t[i].push_f != KSVM_FAULT_NONE) {
            for (k = 0; k < 3u; k++) {
                setup(&f, 12u, pushers[k], 0u);
                f.vm.stack_limit = 0u;
                f.vm.cpu.r[KSVM_REG_SP] = t[i].sp;
                provoke(t_, &f, "push/call", t[i].push_f, t[i].push_a);
                if (TEST_FAILED()) {
                    test_note(t_, "sp=0x%08lX k=%u", (unsigned long)t[i].sp, k);
                    return;
                }
            }
        }
        if (t[i].pop_f == KSVM_FAULT_MEM_RANGE) {
            setup(&f, 12u, OP_R(POP, 5, 0, 0), 0u);
            f.vm.cpu.r[KSVM_REG_SP] = t[i].sp;
            provoke(t_, &f, "POP", KSVM_FAULT_MEM_RANGE, t[i].sp);
            if (TEST_FAILED()) return;
            setup(&f, 12u, OP_R(RET, 0, 0, 0), 0u);
            f.vm.cpu.r[KSVM_REG_SP] = t[i].sp;
            provoke(t_, &f, "RET", KSVM_FAULT_MEM_RANGE, t[i].sp);
            if (TEST_FAILED()) return;
        }
    }
}

/* ---- DIV_ZERO / STACK / TRAP ------------------------------------------ */

TEST(div_zero_all_forms_no_effect)
{
#if KSVM_ENABLE_MULDIV
    static const uint8_t ops[] = { KSVM_OP_DIV, KSVM_OP_DIVU, KSVM_OP_REM, KSVM_OP_REMU };
    unsigned i;
    for (i = 0; i < sizeof ops; i++) {
        fixture f;
        setup(&f, 0x20u, ksvm_enc_r(ops[i], 2, 2, 3), 0u);
        f.vm.cpu.r[3] = 0u;
        provoke(t_, &f, ksvm_op_name(ops[i]), KSVM_FAULT_DIV_ZERO, 0x20u);
        if (TEST_FAILED()) return;
    }
#else
    (void)t_;
#endif
}

TEST(stack_limit_push_call_callr)
{
    uint32_t ws[3];
    unsigned k;
    ws[0] = OP_R(PUSH, 5, 0, 0);
    ws[1] = OP_J(CALL, -8);
    ws[2] = OP_R(CALLR, 0, 6, 0);
    for (k = 0; k < 3u; k++) {
        uint32_t d;
        for (d = 1; d <= 4u; d++) {
            fixture f;
            setup(&f, 16u, ws[k], 0u);
            f.vm.stack_limit = FIX_RAM_BASE + 0x100u;
            f.vm.cpu.r[KSVM_REG_SP] = FIX_RAM_BASE + 0x100u + 4u - d;   /* sp-4 < limite */
            provoke(t_, &f, "stack", KSVM_FAULT_STACK, FIX_RAM_BASE + 0x100u - d);
            if (TEST_FAILED()) return;
        }
    }
}

TEST(trap_and_abort_no_effect)
{
    fixture f;
    setup(&f, 0u, OP_I(TRAP, 7, 8, -2), 0u);
    provoke(t_, &f, "TRAP", KSVM_FAULT_TRAP, 0xFFFFFFFEu);
    if (TEST_FAILED()) return;
    setup(&f, 0u, OP_I(SYSCALL, 0, 0, KSVM_SYSCALL_ABORT), 0u);
    f.vm.cpu.r[1] = 0xABCDu;
    provoke(t_, &f, "ABORT", KSVM_FAULT_TRAP, 0xABCDu);
    if (TEST_FAILED()) return;
    ASSERT_EQ_U32(f.vm.cpu.r[2], cf_sentinel(2));    /* r2 (errno) intocado */
}

/* Fault vindo de YIELD (retomada) e logo após WFI: o estado prévio é
 * preservado da mesma forma. */
TEST(fault_after_yield)
{
    fixture f;
    setup(&f, 0u, OP_R(WFI, 0, 0, 0), 0xFFFFFFFFu);
    ASSERT_EQ(ksvm_step(&f.vm), KSVM_YIELD);
    provoke(t_, &f, "apos WFI", KSVM_FAULT_BAD_OPCODE, 0xFFFFFFFFu);
    ASSERT_EQ_U32(f.vm.cpu.pc, 4u);
}

/* ksvm_reset tira a VM do fault (único caminho de saída). */
TEST(reset_clears_fault)
{
    fixture f;
    setup(&f, 0u, 0xFFFFFFFFu, 0u);
    ASSERT_EQ(ksvm_step(&f.vm), KSVM_FAULT);
    ksvm_reset(&f.vm, 0u, FIX_RAM_END);
    ASSERT_EQ(f.vm.status, KSVM_RUNNING);
    ASSERT_EQ(f.vm.fault, KSVM_FAULT_NONE);
    ASSERT_EQ_U32(f.vm.fault_addr, 0u);
    ASSERT_EQ_U32(f.vm.insn_count, 0u);
}

SUITE(fault_effects)
{
    RUN_TEST(bad_opcode_every_unassigned_byte);
    RUN_TEST(bad_opcode_invalid_special_registers);
    RUN_TEST(pc_align_every_misalignment);
    RUN_TEST(pc_range_cases);
    RUN_TEST(mem_range_every_load_and_store);
    RUN_TEST(mem_perm_stores_to_rom_and_mmio);
    RUN_TEST(stack_ops_memory_faults);
    RUN_TEST(div_zero_all_forms_no_effect);
    RUN_TEST(stack_limit_push_call_callr);
    RUN_TEST(trap_and_abort_no_effect);
    RUN_TEST(fault_after_yield);
    RUN_TEST(reset_clears_fault);
}
