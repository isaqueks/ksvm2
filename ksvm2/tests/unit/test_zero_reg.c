/*
 * test_zero_reg.c — registrador zero (doc 04 §1: "zr sempre 0, escritas
 * descartadas").
 *
 * Toda instrução que escreve em rd é executada duas vezes a partir do
 * MESMO estado: com rd = r0 e com rd = r9. Com r0: r0 continua 0 e nenhum
 * outro registrador muda (exceto sp no POP); flags, pc, RAM e status são
 * idênticos aos da execução com r9 (o efeito colateral não depende do
 * destino ser descartado). Um teste de completude garante que nenhum
 * opcode da tabela da ISA que escreve rd ficou de fora.
 */
#include "conform.h"

enum { K_R3, K_R2, K_I, K_L, K_POP };

typedef struct zr_insn {
    uint8_t  op;
    int      kind;    /* R3: rd,r2,r3  R2: rd,r3  I: rd,r2,imm  L: rd,r2,imm32  POP */
    int32_t  imm;
    uint32_t imm32;
} zr_insn;

static const zr_insn writers[] = {
    { KSVM_OP_MOV,   K_R2, 0, 0 },
    { KSVM_OP_MOVI,  K_I, -5, 0 },
    { KSVM_OP_MOVIL, K_L, 0, 0x89ABCDEFu },
    { KSVM_OP_ADR,   K_I, 8, 0 },
    { KSVM_OP_ADRL,  K_L, 0, 0x1000u },
    { KSVM_OP_MFSR,  K_I, KSVM_SR_PC, 0 },
    { KSVM_OP_LDB,   K_I, 3, 0 },
    { KSVM_OP_LDBS,  K_I, 3, 0 },
    { KSVM_OP_LDH,   K_I, 3, 0 },
    { KSVM_OP_LDHS,  K_I, 3, 0 },
    { KSVM_OP_LDW,   K_I, 3, 0 },
    { KSVM_OP_LDBX,  K_R3, 0, 0 },
    { KSVM_OP_LDBSX, K_R3, 0, 0 },
    { KSVM_OP_LDHX,  K_R3, 0, 0 },
    { KSVM_OP_LDHSX, K_R3, 0, 0 },
    { KSVM_OP_LDWX,  K_R3, 0, 0 },
    { KSVM_OP_POP,   K_POP, 0, 0 },
    { KSVM_OP_ADD,   K_R3, 0, 0 },
    { KSVM_OP_ADDI,  K_I, -1, 0 },
    { KSVM_OP_ADDL,  K_L, 0, 0x80000000u },
    { KSVM_OP_SUB,   K_R3, 0, 0 },
    { KSVM_OP_SUBI,  K_I, 7, 0 },
    { KSVM_OP_ADC,   K_R3, 0, 0 },
    { KSVM_OP_SBC,   K_R3, 0, 0 },
    { KSVM_OP_NEG,   K_R2, 0, 0 },
#if KSVM_ENABLE_MULDIV
    { KSVM_OP_MUL,   K_R3, 0, 0 },
    { KSVM_OP_MULI,  K_I, -3, 0 },
    { KSVM_OP_MULH,  K_R3, 0, 0 },
    { KSVM_OP_MULHU, K_R3, 0, 0 },
    { KSVM_OP_DIV,   K_R3, 0, 0 },
    { KSVM_OP_DIVU,  K_R3, 0, 0 },
    { KSVM_OP_REM,   K_R3, 0, 0 },
    { KSVM_OP_REMU,  K_R3, 0, 0 },
#endif
    { KSVM_OP_AND,   K_R3, 0, 0 },
    { KSVM_OP_ANDI,  K_I, 0xFF, 0 },
    { KSVM_OP_ANDL,  K_L, 0, 0xFFFF0000u },
    { KSVM_OP_OR,    K_R3, 0, 0 },
    { KSVM_OP_ORI,   K_I, -8192, 0 },
    { KSVM_OP_ORL,   K_L, 0, 0x80000001u },
    { KSVM_OP_XOR,   K_R3, 0, 0 },
    { KSVM_OP_XORI,  K_I, -1, 0 },
    { KSVM_OP_XORL,  K_L, 0, 0xFFFFFFFFu },
    { KSVM_OP_NOT,   K_R2, 0, 0 },
    { KSVM_OP_SHL,   K_R3, 0, 0 },
    { KSVM_OP_SHLI,  K_I, 31, 0 },
    { KSVM_OP_SHR,   K_R3, 0, 0 },
    { KSVM_OP_SHRI,  K_I, 1, 0 },
    { KSVM_OP_SAR,   K_R3, 0, 0 },
    { KSVM_OP_SARI,  K_I, 33, 0 },
    { KSVM_OP_ROL,   K_R3, 0, 0 },
    { KSVM_OP_ROR,   K_R3, 0, 0 },
    { KSVM_OP_SXB,   K_R2, 0, 0 },
    { KSVM_OP_SXH,   K_R2, 0, 0 },
    { KSVM_OP_ZXB,   K_R2, 0, 0 },
    { KSVM_OP_ZXH,   K_R2, 0, 0 },
#if KSVM_ENABLE_BITOPS
    { KSVM_OP_CLZ,   K_R2, 0, 0 },
    { KSVM_OP_CTZ,   K_R2, 0, 0 },
    { KSVM_OP_POPCNT, K_R2, 0, 0 },
#endif
#if KSVM_ENABLE_FLOAT
    /* r2 = 0x00010081 e r3 = 5 são subnormais: nenhum resultado é o sentinela */
    { KSVM_OP_FADD,  K_R3, 0, 0 },
    { KSVM_OP_FSUB,  K_R3, 0, 0 },
    { KSVM_OP_FMUL,  K_R3, 0, 0 },
    { KSVM_OP_FDIV,  K_R3, 0, 0 },
    { KSVM_OP_FMIN,  K_R3, 0, 0 },
    { KSVM_OP_FMAX,  K_R3, 0, 0 },
    { KSVM_OP_FSQRT, K_R2, 0, 0 },
    { KSVM_OP_FNEG,  K_R2, 0, 0 },
    { KSVM_OP_FABS,  K_R2, 0, 0 },
    { KSVM_OP_FRINT, K_R2, 0, 0 },
    { KSVM_OP_FFLOOR, K_R2, 0, 0 },
    { KSVM_OP_FCEIL, K_R2, 0, 0 },
    { KSVM_OP_FTRUNC, K_R2, 0, 0 },
    { KSVM_OP_FTOI,  K_R2, 0, 0 },
    { KSVM_OP_FTOU,  K_R2, 0, 0 },
    { KSVM_OP_ITOF,  K_R2, 0, 0 },
    { KSVM_OP_UTOF,  K_R2, 0, 0 },
#endif
};
#define N_WRITERS (sizeof writers / sizeof writers[0])

/* Opcodes da tabela que NÃO escrevem em rd (lista fechada). */
static const uint8_t non_writers[] = {
    KSVM_OP_NOP, KSVM_OP_MTSR,
    KSVM_OP_STB, KSVM_OP_STH, KSVM_OP_STW, KSVM_OP_STBX, KSVM_OP_STHX, KSVM_OP_STWX,
    KSVM_OP_PUSH, KSVM_OP_PUSHM, KSVM_OP_POPM,
    KSVM_OP_CMP, KSVM_OP_CMPI, KSVM_OP_CMPL, KSVM_OP_TST, KSVM_OP_TSTI,
    KSVM_OP_JMP, KSVM_OP_JMPR, KSVM_OP_BCOND, KSVM_OP_CALL, KSVM_OP_CALLR, KSVM_OP_RET,
    KSVM_OP_HALT, KSVM_OP_TRAP, KSVM_OP_SYSCALL, KSVM_OP_BRK, KSVM_OP_WFI,
    KSVM_OP_FCMP,
#if !KSVM_ENABLE_MULDIV
    KSVM_OP_MUL, KSVM_OP_MULI, KSVM_OP_MULH, KSVM_OP_MULHU,
    KSVM_OP_DIV, KSVM_OP_DIVU, KSVM_OP_REM, KSVM_OP_REMU,
#endif
#if !KSVM_ENABLE_BITOPS
    KSVM_OP_CLZ, KSVM_OP_CTZ, KSVM_OP_POPCNT,
#endif
#if !KSVM_ENABLE_FLOAT
    KSVM_OP_FADD, KSVM_OP_FSUB, KSVM_OP_FMUL, KSVM_OP_FDIV, KSVM_OP_FMIN, KSVM_OP_FMAX,
    KSVM_OP_FSQRT, KSVM_OP_FNEG, KSVM_OP_FABS, KSVM_OP_FRINT, KSVM_OP_FFLOOR,
    KSVM_OP_FCEIL, KSVM_OP_FTRUNC, KSVM_OP_FTOI, KSVM_OP_FTOU, KSVM_OP_ITOF, KSVM_OP_UTOF,
#endif
};

static void load_writer(fixture *f, const zr_insn *z, uint8_t rd)
{
    uint32_t w0, w1 = HALT;
    unsigned i;

    switch (z->kind) {
    case K_R3:  w0 = ksvm_enc_r(z->op, rd, 2, 3); break;
    case K_R2:  w0 = ksvm_enc_r(z->op, rd, 3, 0); break;
    case K_I:   w0 = ksvm_enc_i(z->op, rd, 2, z->imm); break;
    case K_L:   w0 = ksvm_enc_l(z->op, rd, 2); w1 = z->imm32; break;
    default:    w0 = ksvm_enc_r(z->op, rd, 0, 0); break;
    }
    LOAD(f, w0, w1, HALT);
    for (i = 1; i < 31u; i++) f->vm.cpu.r[i] = cf_sentinel(i);
    for (i = 0; i < FIX_RAM_SIZE; i++) f->ram[i] = (uint8_t)(i * 37u + 11u);
    f->vm.cpu.r[2] = FIX_RAM_BASE + 0x81u;      /* endereço base desalinhado */
    f->vm.cpu.r[3] = 5u;                        /* índice/divisor/contagem/valor */
    f->vm.cpu.r[KSVM_REG_SP] = FIX_RAM_END - 8u;
    f->vm.cpu.flags = KSVM_FLAG_C | KSVM_FLAG_V;
}

TEST(every_writer_discards_r0)
{
    unsigned i;
    for (i = 0; i < N_WRITERS; i++) {
        const zr_insn *z = &writers[i];
        fixture a, b;
        ksvm_cpu init, want;

        load_writer(&a, z, 0u);
        load_writer(&b, z, 9u);
        init = a.vm.cpu;
        (void)ksvm_step(&a.vm);
        (void)ksvm_step(&b.vm);

        if (a.vm.status != KSVM_RUNNING || a.vm.cpu.r[0] != 0u) {
            test_note(t_, "%s com rd = r0", ksvm_op_name(z->op));
        }
        ASSERT_EQ(b.vm.status, KSVM_RUNNING);      /* a instrução é válida */
        ASSERT_EQ(a.vm.status, KSVM_RUNNING);
        ASSERT_EQ_U32(a.vm.cpu.r[0], 0u);
        /* nada mais mudou além de pc/flags/sp, que batem com rd = r9 */
        want = init;
        want.pc = b.vm.cpu.pc;
        want.flags = b.vm.cpu.flags;
        want.r[KSVM_REG_SP] = b.vm.cpu.r[KSVM_REG_SP];
        if (memcmp(&want, &a.vm.cpu, sizeof want) != 0) {
            test_note(t_, "%s com rd = r0 alterou outro registrador", ksvm_op_name(z->op));
        }
        ASSERT_MEM_EQ(&want, &a.vm.cpu, sizeof want);
        ASSERT_MEM_EQ(a.ram, b.ram, sizeof a.ram);
        ASSERT_EQ_U32(a.vm.insn_count, 1u);
        /* e com r9 o valor de fato foi escrito (o teste não é vazio) */
        ASSERT_TRUE(b.vm.cpu.r[9] != cf_sentinel(9) || z->op == KSVM_OP_MOV);
    }
}

/* Completude: cada opcode da tabela está em writers[] ou non_writers[]. */
TEST(writer_list_covers_isa_table)
{
    unsigned op, i;
    for (op = 0; op < 256u; op++) {
        int found = 0;
        if (ksvm_isa_get(op).format == KSVM_FMT_INVALID) continue;
        for (i = 0; i < N_WRITERS; i++) found |= writers[i].op == op;
        for (i = 0; i < sizeof non_writers; i++) found |= non_writers[i] == op;
        if (!found) test_note(t_, "opcode 0x%02X (%s) sem teste de zr", op, ksvm_op_name((uint8_t)op));
        ASSERT_TRUE(found);
    }
}

/* MFSR em r0 para cada SR. */
TEST(mfsr_into_r0)
{
    unsigned sr;
    for (sr = 0; sr < 4u; sr++) {
        fixture f;
        RUN(&f, OP_I(MFSR, 0, 0, (int32_t)sr), OP_R(MOV, 1, 0, 0), HALT);
        ASSERT_EQ(f.vm.status, KSVM_HALTED);
        ASSERT_EQ_U32(REG(&f, 1), 0u);
    }
}

/* POP r0 consome a pilha; o valor é descartado. */
TEST(pop_into_r0_still_moves_sp)
{
    fixture f;
    LOAD(&f, OP_R(POP, 0, 0, 0), OP_R(ADD, 1, 0, 0), HALT);
    REG(&f, KSVM_REG_SP) = FIX_RAM_END - 4u;
    fix_set_ram32(&f, FIX_RAM_END - 4u, 0xFFFFFFFFu);
    (void)fix_run(&f);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 0), 0u);
    ASSERT_EQ_U32(REG(&f, 1), 0u);
    ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_END);
}

/* Flags continuam calculadas sobre o resultado descartado (CMP com outro
 * nome): SUB r0 é o idioma "compare" e ADDS r0 o "teste de carry". */
TEST(flags_from_discarded_result)
{
    fixture f;
    RUN(&f, LI(2, 5), LI(3, 5), OP_R(SUB, 0, 2, 3), HALT);
    ASSERT_EQ_U32(f.vm.cpu.flags, KSVM_FLAG_Z | KSVM_FLAG_C);
    ASSERT_EQ_U32(REG(&f, 0), 0u);
    RUN(&f, LI(2, 0xFFFFFFFFu), OP_I(ADDI, 0, 2, 1), HALT);
    ASSERT_EQ_U32(f.vm.cpu.flags, KSVM_FLAG_Z | KSVM_FLAG_C);
    RUN(&f, LI(2, 0x80000000u), OP_I(SHLI, 0, 2, 1), HALT);
    ASSERT_EQ_U32(f.vm.cpu.flags, KSVM_FLAG_Z | KSVM_FLAG_C);
}

/* r0 como fonte em todos os papéis: base de endereço, índice, valor do
 * store, alvo de JMPR, valor de PUSH. */
TEST(r0_as_source_everywhere)
{
    fixture f;
    RUN(&f, OP_I(LDW, 1, 0, 0), HALT);                      /* [0 + 0] = código */
    ASSERT_EQ_U32(REG(&f, 1), OP_I(LDW, 1, 0, 0));
    RUN(&f, LI(2, FIX_RAM_BASE), LI(1, 0xFFFFFFFFu), OP_I(STW, 1, 2, 0),
        OP_R(STWX, 0, 2, 0), OP_R(LDWX, 3, 2, 0), HALT);
    ASSERT_EQ_U32(REG(&f, 3), 0u);
    RUN(&f, LI(4, 9), OP_R(PUSH, 0, 0, 0), OP_R(POP, 4, 0, 0), HALT);
    ASSERT_EQ_U32(REG(&f, 4), 0u);
    LOAD(&f, OP_I(MOVI, 5, 0, 1), OP_R(JMPR, 0, 0, 0));
    ASSERT_EQ(fix_steps(&f, 2), KSVM_RUNNING);
    ASSERT_EQ_U32(f.vm.cpu.pc, 0u);                         /* JMPR zr -> 0 */
}

/* Um load em r0 que falha continua falhando (o descarte não esconde o
 * fault) e r0 segue 0. */
TEST(faulting_load_into_r0_still_faults)
{
    fixture f;
    RUN(&f, LI(2, 0x5000u), OP_I(LDW, 0, 2, 0));
    ASSERT_EQ(f.vm.status, KSVM_FAULT);
    ASSERT_EQ(f.vm.fault, KSVM_FAULT_MEM_RANGE);
    ASSERT_EQ_U32(REG(&f, 0), 0u);
#if KSVM_ENABLE_MULDIV
    RUN(&f, OP_R(DIV, 0, 0, 0));
    ASSERT_EQ(f.vm.status, KSVM_FAULT);
    ASSERT_EQ(f.vm.fault, KSVM_FAULT_DIV_ZERO);
#endif
}

SUITE(zero_reg)
{
    RUN_TEST(every_writer_discards_r0);
    RUN_TEST(writer_list_covers_isa_table);
    RUN_TEST(mfsr_into_r0);
    RUN_TEST(pop_into_r0_still_moves_sp);
    RUN_TEST(flags_from_discarded_result);
    RUN_TEST(r0_as_source_everywhere);
    RUN_TEST(faulting_load_into_r0_still_faults);
}
