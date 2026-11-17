/*
 * test_conform_flow.c — conformidade de movimentação (doc 04 §5.1) e
 * controle de fluxo (§5.6), incluindo a tabela LITERAL de Bcc: 16
 * condições x 16 combinações de Z/N/C/V (oracle.h, o_cond_table), com
 * deslocamento para frente e para trás.
 */
#include "conform.h"

/* ---- Movimentação ----------------------------------------------------- */

TEST(conform_MOV)
{
    unsigned i;
    for (i = 0; i < cf_nvals; i++) {
        cf_case c;
        c.what = "MOV";
        c.w0 = ksvm_enc_r(KSVM_OP_MOV, CF_RD, CF_RB, CF_RA);   /* rc ignorado */
        c.w1 = 0u;
        c.a = 0x11111111u;
        c.b = cf_vals[i];
        c.fin = cf_flag_sets[i % cf_nflag_sets];
        c.rd = CF_RD;
        c.v = cf_vals[i];
        c.fl = c.fin;                       /* MOV não mexe em flags */
        cf_run(t_, &c);
        if (TEST_FAILED()) return;
    }
}

TEST(conform_MOVI_MOVIL)
{
    unsigned i;
    for (i = 0; i < cf_nimms; i++) {
        cf_case c;
        c.what = "MOVI";
        c.w0 = OP_I(MOVI, CF_RD, CF_RA, cf_imms[i]);           /* rb ignorado */
        c.w1 = 0u;
        c.a = 0x5555AAAAu;
        c.b = 0u;
        c.fin = KSVM_FLAGS_MASK;
        c.rd = CF_RD;
        c.v = (uint32_t)cf_imms[i];
        c.fl = KSVM_FLAGS_MASK;
        cf_run(t_, &c);
        if (TEST_FAILED()) return;
    }
    for (i = 0; i < cf_nvals; i++) {
        cf_case c;
        c.what = "MOVIL";
        c.w0 = ksvm_enc_l(KSVM_OP_MOVIL, CF_RD, CF_RA);
        c.w1 = cf_vals[i];
        c.a = 0u;
        c.b = 0u;
        c.fin = 0u;
        c.rd = CF_RD;
        c.v = cf_vals[i];
        c.fl = 0u;
        cf_run(t_, &c);
        if (TEST_FAILED()) return;
    }
}

/* Executa a instrução colocada em "pc" (o resto do código é 0xFF). */
static void run_at(fixture *f, uint32_t pc, uint32_t w0, uint32_t w1)
{
    uint32_t prog[FIX_CODE_SIZE / 4u];
    memset(prog, 0xFF, sizeof prog);
    prog[pc / 4u] = w0;
    prog[pc / 4u + 1u] = w1;
    fix_load(f, prog, FIX_CODE_SIZE / 4u);
    f->vm.cpu.pc = pc;
    (void)ksvm_step(&f->vm);
}

TEST(conform_ADR_ADRL)
{
    static const uint32_t pcs[] = { 0u, 4u, 0x100u, FIX_CODE_SIZE - 16u };
    unsigned p, i;
    for (p = 0; p < sizeof pcs / sizeof pcs[0]; p++) {
        for (i = 0; i < cf_nimms; i++) {
            fixture f;
            run_at(&f, pcs[p], OP_I(ADR, 7, 3, cf_imms[i]), 0xFFFFFFFFu);
            ASSERT_EQ(f.vm.status, KSVM_RUNNING);
            ASSERT_EQ_U32(REG(&f, 7), o_lo((int64_t)pcs[p] + 4 + cf_imms[i]));
            ASSERT_EQ_U32(f.vm.cpu.pc, pcs[p] + 4u);
            ASSERT_EQ_U32(f.vm.cpu.flags, 0u);
        }
        for (i = 0; i < cf_nvals; i++) {
            fixture f;
            run_at(&f, pcs[p], ksvm_enc_l(KSVM_OP_ADRL, 7, 3), cf_vals[i]);
            ASSERT_EQ(f.vm.status, KSVM_RUNNING);
            ASSERT_EQ_U32(REG(&f, 7), pcs[p] + 8u + cf_vals[i]);
            ASSERT_EQ_U32(f.vm.cpu.pc, pcs[p] + 8u);
        }
    }
}

TEST(conform_MFSR)
{
    unsigned fl;
    fixture f;

    /* PC: endereço da própria instrução, não PC_next (doc 04 §5.1). */
    run_at(&f, 0x40u, OP_I(MFSR, 9, 0, KSVM_SR_PC), 0u);
    ASSERT_EQ(f.vm.status, KSVM_RUNNING);
    ASSERT_EQ_U32(REG(&f, 9), 0x40u);

    for (fl = 0; fl < 16u; fl++) {
        cf_case c;
        c.what = "MFSR FLAGS";
        c.w0 = OP_I(MFSR, CF_RD, CF_RB, KSVM_SR_FLAGS);
        c.w1 = 0u;
        c.a = 0u;
        c.b = 0u;
        c.fin = fl;
        c.rd = CF_RD;
        c.v = fl;
        c.fl = fl;
        cf_run(t_, &c);
        if (TEST_FAILED()) return;
    }
    {
        cf_case c;
        c.what = "MFSR ISA_VERSION";
        c.w0 = OP_I(MFSR, CF_RD, 0, KSVM_SR_ISA_VERSION);
        c.w1 = 0u;
        c.a = 0u;
        c.b = 0u;
        c.fin = 0u;
        c.rd = CF_RD;
        c.v = 0x0200u;
        c.fl = 0u;
        cf_run(t_, &c);
        if (TEST_FAILED()) return;
    }
    /* INSN_COUNT: instruções concluídas ANTES desta. */
    {
        fixture g;
        LOAD(&g, OP_R(NOP, 0, 0, 0), OP_R(NOP, 0, 0, 0), OP_I(MFSR, 4, 0, KSVM_SR_INSN_COUNT), HALT);
        g.vm.insn_count = 0xFFFFFFFEu;
        (void)fix_run(&g);
        ASSERT_EQ(g.vm.status, KSVM_HALTED);
        ASSERT_EQ_U32(REG(&g, 4), 0u);           /* 0xFFFFFFFE + 2, wrap */
        ASSERT_EQ_U32(g.vm.insn_count, 2u);
    }
}

TEST(mfsr_mtsr_invalid_sr_fault_without_effect)
{
    static const int32_t bad_mfsr[] = { 4, 5, 15, 8191, -1, -8192 };
    static const int32_t bad_mtsr[] = { 0, 2, 3, 4, 8191, -1, -8192 };
    unsigned i;
    for (i = 0; i < sizeof bad_mfsr / sizeof bad_mfsr[0]; i++) {
        cf_case c;
        c.what = "MFSR invalido";
        c.w0 = OP_I(MFSR, CF_RD, CF_RB, bad_mfsr[i]);
        c.w1 = 0u;
        c.a = 1u;
        c.b = 2u;
        c.fin = O_Z | O_V;
        cf_run_fault(t_, &c, KSVM_FAULT_BAD_OPCODE, c.w0);
        if (TEST_FAILED()) return;
    }
    for (i = 0; i < sizeof bad_mtsr / sizeof bad_mtsr[0]; i++) {
        cf_case c;
        c.what = "MTSR invalido";
        c.w0 = OP_I(MTSR, 0, CF_RB, bad_mtsr[i]);
        c.w1 = 0u;
        c.a = 1u;
        c.b = 0xFFFFFFFFu;
        c.fin = O_N;
        cf_run_fault(t_, &c, KSVM_FAULT_BAD_OPCODE, c.w0);
        if (TEST_FAILED()) return;
    }
}

TEST(conform_MTSR)
{
    unsigned i;
    for (i = 0; i < cf_nvals; i++) {
        cf_case c;
        c.what = "MTSR FLAGS";
        c.w0 = OP_I(MTSR, CF_RD, CF_RB, KSVM_SR_FLAGS);   /* ra ignorado */
        c.w1 = 0u;
        c.a = 0u;
        c.b = cf_vals[i];
        c.fin = cf_flag_sets[i % cf_nflag_sets];
        c.rd = CF_NONE;
        c.v = 0u;
        c.fl = cf_vals[i] % 16u;            /* bits 4..31 reservados */
        cf_run(t_, &c);
        if (TEST_FAILED()) return;
    }
}

/* NOP com todos os campos de registrador preenchidos continua NOP. */
TEST(conform_NOP_ignores_fields)
{
    static const uint32_t ws[] = { 0x00000000u, 0xFFFFFF00u, 0x0007FF00u, 0x80000000u };
    unsigned i;
    for (i = 0; i < sizeof ws / sizeof ws[0]; i++) {
        cf_case c;
        c.what = "NOP";
        c.w0 = ws[i];
        c.w1 = 0u;
        c.a = 7u;
        c.b = 9u;
        c.fin = O_C;
        c.rd = CF_NONE;
        c.v = 0u;
        c.fl = O_C;
        cf_run(t_, &c);
        if (TEST_FAILED()) return;
    }
}

/* Bits reservados (func9 do formato R, bits 18..31 da 1a palavra do L) são
 * ignorados pela VM 2.0, sem fault (doc 04 §5.6, "Bits reservados"). */
TEST(reserved_bits_are_ignored)
{
    cf_case c;
    c.what = "ADD com func9";
    c.w0 = OP_R(ADD, CF_RD, CF_RA, CF_RB) | 0xFF800000u;
    c.w1 = 0u;
    c.a = 40u;
    c.b = 2u;
    c.fin = 0u;
    c.rd = CF_RD;
    c.v = 42u;
    c.fl = 0u;
    cf_run(t_, &c);
    if (TEST_FAILED()) return;
    c.what = "MOVIL com bits altos";
    c.w0 = ksvm_enc_l(KSVM_OP_MOVIL, CF_RD, 0) | 0xFFFC0000u;
    c.w1 = 0xCAFEF00Du;
    c.v = 0xCAFEF00Du;
    cf_run(t_, &c);
}

/* ---- Bcc: 16 condições x 16 flags, literal ---------------------------- */

/* Para cada caso: FLAGS definido antes do passo; Bcc +4 em 0 (pula uma
 * palavra) e Bcc -12 em 0x20 (PC_next 0x24 -> 0x18). Confere o pc e que
 * Bcc não altera flags nem registradores. */
TEST(bcc_literal_table_256_cases)
{
    unsigned cond, fl;
    for (cond = 0; cond < 16u; cond++) {
        for (fl = 0; fl < 16u; fl++) {
            int taken = o_cond_table[cond][fl] == '1';
            unsigned k;
            for (k = 0; k < 2u; k++) {
                fixture f;
                uint32_t prog[16];
                uint32_t pc = k ? 0x20u : 0u;
                int32_t off = k ? -12 : 4;
                uint32_t target = pc + 4u + (uint32_t)off;
                ksvm_cpu before;
                unsigned i;
                for (i = 0; i < 16u; i++) prog[i] = HALT;
                prog[pc / 4u] = ksvm_enc_b((uint8_t)cond, off);
                fix_load(&f, prog, 16u);
                for (i = 1; i < 31u; i++) f.vm.cpu.r[i] = cf_sentinel(i);
                f.vm.cpu.pc = pc;
                f.vm.cpu.flags = fl;
                before = f.vm.cpu;
                (void)ksvm_step(&f.vm);
                if (f.vm.cpu.pc != (taken ? target : pc + 4u)) {
                    test_note(t_, "cond %s (%u) flags Z=%u N=%u C=%u V=%u off=%d: esperado %s",
                              ksvm_cond_name((uint8_t)cond), cond, fl & 1u, (fl >> 1) & 1u,
                              (fl >> 2) & 1u, (fl >> 3) & 1u, (int)off,
                              taken ? "desvio" : "seguir");
                }
                ASSERT_EQ(f.vm.status, KSVM_RUNNING);
                ASSERT_EQ_U32(f.vm.cpu.pc, taken ? target : pc + 4u);
                before.pc = f.vm.cpu.pc;
                ASSERT_MEM_EQ(&before, &f.vm.cpu, sizeof before);
            }
        }
    }
}

/* O núcleo também exporta ksvm_cond_holds: mesma tabela, chamada direta. */
TEST(cond_holds_matches_literal_table)
{
    unsigned cond, fl;
    for (cond = 0; cond < 16u; cond++) {
        for (fl = 0; fl < 16u; fl++) {
            ASSERT_EQ(ksvm_cond_holds(fl, (uint8_t)cond), o_cond_table[cond][fl] == '1');
            /* bits reservados acima de V não interferem */
            ASSERT_EQ(ksvm_cond_holds(fl | 0xFFFFFFF0u, (uint8_t)cond),
                      o_cond_table[cond][fl] == '1');
        }
    }
}

/* Alcance máximo dos deslocamentos: o desvio acontece e o fault só vem
 * no próximo fetch (doc 04 §5.6). */
TEST(branch_offsets_at_limits)
{
    /* alvo = PC_next (4) + deslocamento, módulo 2^32 */
    static const uint32_t target[4] = { 0x00200000u, 0xFFE00004u, 0x02000000u, 0xFE000004u };
    uint32_t ws[4];
    unsigned i;
    ws[0] = ksvm_enc_b(KSVM_COND_AL, (1 << 21) - 4);
    ws[1] = ksvm_enc_b(KSVM_COND_AL, -(1 << 21));
    ws[2] = ksvm_enc_j(KSVM_OP_JMP, (1 << 25) - 4);
    ws[3] = ksvm_enc_j(KSVM_OP_JMP, -(1 << 25));
    for (i = 0; i < 4u; i++) {
        fixture f;
        run_at(&f, 0u, ws[i], HALT);
        ASSERT_EQ(f.vm.status, KSVM_RUNNING);
        ASSERT_EQ_U32(f.vm.cpu.pc, target[i]);
        ASSERT_EQ(ksvm_step(&f.vm), KSVM_FAULT);
        ASSERT_EQ(f.vm.fault, KSVM_FAULT_PC_RANGE);
        ASSERT_EQ_U32(f.vm.fault_addr, target[i]);
        ASSERT_EQ_U32(f.vm.cpu.pc, target[i]);
    }
}

TEST(call_offsets_at_limits_push_return)
{
    static const int32_t offs[] = { (1 << 25) - 4, -(1 << 25), 0, -4 };
    unsigned i;
    for (i = 0; i < sizeof offs / sizeof offs[0]; i++) {
        fixture f;
        run_at(&f, 0x10u, ksvm_enc_j(KSVM_OP_CALL, offs[i]), HALT);
        ASSERT_EQ(f.vm.status, KSVM_RUNNING);
        ASSERT_EQ_U32(f.vm.cpu.pc, 0x14u + (uint32_t)offs[i]);
        ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_END - 4u);
        ASSERT_EQ_U32(fix_ram32(&f, FIX_RAM_END - 4u), 0x14u);
    }
}

TEST(jmpr_callr_ret_targets)
{
    static const uint32_t targets[] = { 0x0u, 0x8u, 0x3FCu, 0x2u, 0x401u, 0xFFFFFFFCu, 0x10000u };
    unsigned i;
    for (i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        uint32_t t = targets[i];
        fixture f;
        ksvm_fault next = ((t & 3u) != 0u) ? KSVM_FAULT_PC_ALIGN :
                          (t < FIX_CODE_SIZE) ? KSVM_FAULT_BAD_OPCODE : KSVM_FAULT_PC_RANGE;
        uint32_t addr_expected = (next == KSVM_FAULT_BAD_OPCODE) ? 0xFFFFFFFFu : t;

        /* JMPR: só rb (campos ra/rc ignorados) */
        LOAD(&f, OP_R(JMPR, 5, 6, 7));
        REG(&f, 6) = t;
        REG(&f, 5) = 0x100u;
        REG(&f, 7) = 0x200u;
        (void)ksvm_step(&f.vm);
        ASSERT_EQ(f.vm.status, KSVM_RUNNING);
        ASSERT_EQ_U32(f.vm.cpu.pc, t);
        (void)ksvm_step(&f.vm);
        if (t == 0u) {
            ASSERT_EQ(f.vm.status, KSVM_RUNNING);   /* volta ao próprio JMPR */
        } else {
            ASSERT_EQ(f.vm.status, KSVM_FAULT);
            ASSERT_EQ(f.vm.fault, next);
            ASSERT_EQ_U32(f.vm.fault_addr, addr_expected);
        }

        /* CALLR: empilha PC_next e desvia */
        LOAD(&f, OP_R(CALLR, 0, 6, 0));
        REG(&f, 6) = t;
        (void)ksvm_step(&f.vm);
        ASSERT_EQ(f.vm.status, KSVM_RUNNING);
        ASSERT_EQ_U32(f.vm.cpu.pc, t);
        ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_END - 4u);
        ASSERT_EQ_U32(fix_ram32(&f, FIX_RAM_END - 4u), 4u);

        /* RET: pc = [sp]; sp += 4 */
        LOAD(&f, OP_R(RET, 0, 0, 0));
        REG(&f, KSVM_REG_SP) = FIX_RAM_END - 8u;
        fix_set_ram32(&f, FIX_RAM_END - 8u, t);
        (void)ksvm_step(&f.vm);
        ASSERT_EQ(f.vm.status, KSVM_RUNNING);
        ASSERT_EQ_U32(f.vm.cpu.pc, t);
        ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_END - 4u);
    }
}

/* CALLR sp: o alvo é o sp ANTES do push (doc 04 §5.6: rb lido antes do
 * ajuste de sp; como PUSH sp). */
TEST(callr_sp_uses_old_sp)
{
    fixture f;
    LOAD(&f, OP_R(CALLR, 0, KSVM_REG_SP, 0));
    (void)ksvm_step(&f.vm);
    ASSERT_EQ_U32(f.vm.cpu.pc, FIX_RAM_END);
    ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_END - 4u);
}

TEST(control_flow_preserves_flags_and_registers)
{
    uint32_t w[5];
    unsigned i;
    w[0] = OP_J(JMP, 8);
    w[1] = OP_R(JMPR, 0, 6, 0);
    w[2] = OP_B(AL, -4);
    w[3] = OP_R(BRK, 0, 0, 0);
    w[4] = OP_R(NOP, 0, 0, 0);
    for (i = 0; i < 5u; i++) {
        unsigned fl;
        for (fl = 0; fl < 16u; fl++) {
            fixture f;
            ksvm_cpu before;
            unsigned r;
            LOAD(&f, w[i], HALT, HALT, HALT);
            for (r = 1; r < 31u; r++) f.vm.cpu.r[r] = cf_sentinel(r);
            REG(&f, 6) = 12u;
            f.vm.cpu.flags = fl;
            before = f.vm.cpu;
            (void)ksvm_step(&f.vm);
            ASSERT_EQ(f.vm.status, KSVM_RUNNING);
            before.pc = f.vm.cpu.pc;
            ASSERT_MEM_EQ(&before, &f.vm.cpu, sizeof before);
        }
    }
}

TEST(halt_trap_conform)
{
    static const int32_t codes[] = { 0, 1, -1, 8191, -8192, 0x1234 };
    unsigned i;
    for (i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        fixture f;
        ksvm_cpu before;
        LOAD(&f, OP_I(TRAP, 3, 4, codes[i]));
        f.vm.cpu.flags = O_Z | O_V;
        before = f.vm.cpu;
        (void)ksvm_step(&f.vm);
        ASSERT_EQ(f.vm.status, KSVM_FAULT);
        ASSERT_EQ(f.vm.fault, KSVM_FAULT_TRAP);
        ASSERT_EQ_U32(f.vm.fault_addr, (uint32_t)codes[i]);   /* com sinal */
        ASSERT_MEM_EQ(&before, &f.vm.cpu, sizeof before);
        ASSERT_EQ_U32(f.vm.insn_count, 0u);
    }
    for (i = 0; i < cf_nvals; i++) {
        fixture f;
        LOAD(&f, HALT);
        REG(&f, 1) = cf_vals[i];
        (void)ksvm_step(&f.vm);
        ASSERT_EQ(f.vm.status, KSVM_HALTED);
        ASSERT_EQ_U32(f.vm.exit_code, cf_vals[i]);
        ASSERT_TRUE(f.hn.halted);
        ASSERT_EQ_U32(f.hn.halt_code, cf_vals[i]);
        ASSERT_EQ_U32(REG(&f, 1), cf_vals[i]);
        ASSERT_EQ(ksvm_step(&f.vm), KSVM_HALTED);       /* parada é definitiva */
        ASSERT_EQ_U32(f.vm.insn_count, 1u);
    }
}

/* SYSCALL: número = imm14 SEM sinal (0..0x3FFF), args intactos. */
static uint32_t g_sys_num;
static int record_syscall(struct ksvm *vm, uint32_t num)
{
    g_sys_num = num;
    ksvm_ret(vm, 0x600Du, 0u);
    return KSVM_SYS_OK;
}

TEST(syscall_number_is_unsigned_imm14)
{
    static const int32_t imms[] = { 0x100, 0x1FFF, -1, -8192, 0x2ABC - 0x4000 };
    unsigned i;
    for (i = 0; i < sizeof imms / sizeof imms[0]; i++) {
        fixture f;
        LOAD(&f, OP_I(SYSCALL, 5, 6, imms[i]), HALT);
        f.hal.syscall = record_syscall;
        g_sys_num = 0xDEADu;
        (void)ksvm_step(&f.vm);
        ASSERT_EQ(f.vm.status, KSVM_RUNNING);
        ASSERT_EQ_U32(g_sys_num, (uint32_t)imms[i] & 0x3FFFu);
        ASSERT_EQ_U32(REG(&f, 1), 0x600Du);
        ASSERT_EQ_U32(f.vm.cpu.pc, 4u);
    }
}

TEST(wfi_yields_after_advancing)
{
    fixture f;
    LOAD(&f, OP_R(WFI, 0, 0, 0), OP_I(MOVI, 1, 0, 7), HALT);
    f.vm.cpu.flags = O_N;
    ASSERT_EQ(ksvm_step(&f.vm), KSVM_YIELD);
    ASSERT_EQ_U32(f.vm.cpu.pc, 4u);
    ASSERT_EQ_U32(f.vm.insn_count, 1u);
    ASSERT_EQ_U32(f.vm.cpu.flags, O_N);
    ASSERT_EQ(ksvm_step(&f.vm), KSVM_RUNNING);
    ASSERT_EQ_U32(REG(&f, 1), 7u);
    ASSERT_EQ(ksvm_run(&f.vm, 0), KSVM_HALTED);
}

SUITE(conform_flow)
{
    RUN_TEST(conform_MOV);
    RUN_TEST(conform_MOVI_MOVIL);
    RUN_TEST(conform_ADR_ADRL);
    RUN_TEST(conform_MFSR);
    RUN_TEST(mfsr_mtsr_invalid_sr_fault_without_effect);
    RUN_TEST(conform_MTSR);
    RUN_TEST(conform_NOP_ignores_fields);
    RUN_TEST(reserved_bits_are_ignored);
    RUN_TEST(bcc_literal_table_256_cases);
    RUN_TEST(cond_holds_matches_literal_table);
    RUN_TEST(branch_offsets_at_limits);
    RUN_TEST(call_offsets_at_limits_push_return);
    RUN_TEST(jmpr_callr_ret_targets);
    RUN_TEST(callr_sp_uses_old_sp);
    RUN_TEST(control_flow_preserves_flags_and_registers);
    RUN_TEST(halt_trap_conform);
    RUN_TEST(syscall_number_is_unsigned_imm14);
    RUN_TEST(wfi_yields_after_advancing);
}
