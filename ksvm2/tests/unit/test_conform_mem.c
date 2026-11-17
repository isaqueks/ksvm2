/*
 * test_conform_mem.c — conformidade de load/store e pilha (doc 04 §5.2,
 * §5.3): todos os alinhamentos (0..3), extensão de sinal/zero nos valores
 * de borda, EA = rb + imm14 / rb + rc com wrap módulo 2^32, bordas de
 * região, acesso cruzando a borda entre duas regiões mapeadas lado a lado
 * (deve falhar sem escrita parcial), ROM e topo do espaço de endereços.
 */
#include "conform.h"

/* Valores de 32 bits cujos bytes/halfs exercitam as extensões. */
static const uint32_t mem_vals[] = {
    0x00000000u, 0x0000007Fu, 0x00000080u, 0x000000FFu, 0x00007FFFu, 0x00008000u,
    0x0000FFFFu, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x12345678u, 0x80FF7F01u,
    0x017F80FEu,
};
#define N_MEM_VALS (sizeof mem_vals / sizeof mem_vals[0])

typedef struct load_op {
    uint8_t  op_i, op_x;
    unsigned size;
    int      sign;
} load_op;

static const load_op loads[] = {
    { KSVM_OP_LDB,  KSVM_OP_LDBX,  1u, 0 },
    { KSVM_OP_LDBS, KSVM_OP_LDBSX, 1u, 1 },
    { KSVM_OP_LDH,  KSVM_OP_LDHX,  2u, 0 },
    { KSVM_OP_LDHS, KSVM_OP_LDHSX, 2u, 1 },
    { KSVM_OP_LDW,  KSVM_OP_LDWX,  4u, 0 },
};

static const struct { uint8_t op_i, op_x; unsigned size; } stores[] = {
    { KSVM_OP_STB, KSVM_OP_STBX, 1u },
    { KSVM_OP_STH, KSVM_OP_STHX, 2u },
    { KSVM_OP_STW, KSVM_OP_STWX, 4u },
};

/* Esperado de um load: bytes little-endian compostos por aritmética. */
static uint32_t expect_load(uint32_t v, unsigned size, int sign)
{
    uint64_t m = (size == 4u) ? UINT64_C(0x100000000) : (UINT64_C(1) << (8u * size));
    uint32_t raw = (uint32_t)((uint64_t)v % m);
    return sign ? o_sext(raw, 8u * size) : raw;
}

static void put_le(uint8_t *p, uint32_t v, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        p[i] = (uint8_t)(v % 256u);
        v /= 256u;
    }
}

/* Prepara: instrução w0 em 0, sentinelas nos registradores. */
static void prep(fixture *f, uint32_t w0)
{
    unsigned i;
    LOAD(f, w0, HALT);
    for (i = 1; i < 31u; i++) f->vm.cpu.r[i] = cf_sentinel(i);
    f->vm.cpu.flags = O_N | O_C;
}

/* Um passo deve completar mudando só o registrador rd (ou nenhum). */
static void step_ok(test_ctx *t_, fixture *f, unsigned rd, uint32_t v)
{
    ksvm_cpu want = f->vm.cpu;
    if (rd < 32u) want.r[rd] = v;
    want.r[0] = 0u;
    want.pc = 4u;
    (void)ksvm_step(&f->vm);
    if (f->vm.status == KSVM_FAULT) {
        test_note(t_, "fault %s addr=0x%08lX", ksvm_fault_name(f->vm.fault),
                  (unsigned long)f->vm.fault_addr);
    }
    ASSERT_EQ(f->vm.status, KSVM_RUNNING);
    if (rd < 32u) {
        ASSERT_EQ_U32(f->vm.cpu.r[rd], v);
    }
    ASSERT_MEM_EQ(&want, &f->vm.cpu, sizeof want);
}

/* Um passo deve falhar sem mudar cpu, RAM nem código. */
static void step_fault(test_ctx *t_, fixture *f, ksvm_fault fault, uint32_t addr)
{
    ksvm_cpu before = f->vm.cpu;
    uint8_t ram[FIX_RAM_SIZE], code[FIX_CODE_SIZE];
    memcpy(ram, f->ram, sizeof ram);
    memcpy(code, f->code, sizeof code);
    (void)ksvm_step(&f->vm);
    ASSERT_EQ(f->vm.status, KSVM_FAULT);
    ASSERT_EQ(f->vm.fault, fault);
    ASSERT_EQ_U32(f->vm.fault_addr, addr);
    ASSERT_MEM_EQ(&before, &f->vm.cpu, sizeof before);
    ASSERT_MEM_EQ(ram, f->ram, sizeof ram);
    ASSERT_MEM_EQ(code, f->code, sizeof code);
    ASSERT_EQ_U32(f->vm.insn_count, 0u);
}

/* Todos os loads x valores x alinhamentos 0..3 x deslocamentos de EA. */
TEST(loads_alignment_extension_and_ea)
{
    static const int32_t imms[] = { 0, 1, -1, 8191, -8192, 4, -4 };
    static const uint32_t idx[] = { 0u, 4u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu };
    unsigned l, v, al, k;
    for (l = 0; l < sizeof loads / sizeof loads[0]; l++) {
        for (v = 0; v < N_MEM_VALS; v++) {
            for (al = 0; al < 4u; al++) {
                uint32_t ea = FIX_RAM_BASE + 0x100u + al;
                uint32_t want = expect_load(mem_vals[v], loads[l].size, loads[l].sign);
                for (k = 0; k < sizeof imms / sizeof imms[0]; k++) {
                    fixture f;
                    prep(&f, ksvm_enc_i(loads[l].op_i, 1, 2, imms[k]));
                    put_le(&f.ram[ea - FIX_RAM_BASE], mem_vals[v], 4u);
                    REG(&f, 2) = ea - (uint32_t)imms[k];
                    step_ok(t_, &f, 1u, want);
                    if (TEST_FAILED()) {
                        test_note(t_, "%s v=0x%08lX ea=0x%lX imm=%ld", ksvm_op_name(loads[l].op_i),
                                  (unsigned long)mem_vals[v], (unsigned long)ea, (long)imms[k]);
                        return;
                    }
                }
                for (k = 0; k < sizeof idx / sizeof idx[0]; k++) {
                    fixture f;
                    prep(&f, ksvm_enc_r(loads[l].op_x, 1, 2, 3));
                    put_le(&f.ram[ea - FIX_RAM_BASE], mem_vals[v], 4u);
                    REG(&f, 2) = ea - idx[k];
                    REG(&f, 3) = idx[k];
                    step_ok(t_, &f, 1u, want);
                    if (TEST_FAILED()) {
                        test_note(t_, "%s v=0x%08lX ea=0x%lX rc=0x%lX", ksvm_op_name(loads[l].op_x),
                                  (unsigned long)mem_vals[v], (unsigned long)ea, (unsigned long)idx[k]);
                        return;
                    }
                }
            }
        }
    }
}

/* Stores: só "size" bytes mudam, little-endian, vizinhos intactos. */
TEST(stores_alignment_truncation_and_neighbors)
{
    unsigned s, v, al, form;
    for (s = 0; s < sizeof stores / sizeof stores[0]; s++) {
        for (v = 0; v < N_MEM_VALS; v++) {
            for (al = 0; al < 4u; al++) {
                for (form = 0; form < 2u; form++) {
                    fixture f;
                    uint8_t want[FIX_RAM_SIZE];
                    uint32_t ea = FIX_RAM_BASE + 0x200u + al;
                    if (form == 0u) {
                        prep(&f, ksvm_enc_i(stores[s].op_i, 4, 2, -3));
                        REG(&f, 2) = ea + 3u;
                    } else {
                        prep(&f, ksvm_enc_r(stores[s].op_x, 4, 2, 3));
                        REG(&f, 2) = ea + 0x100u;
                        REG(&f, 3) = 0xFFFFFF00u;          /* -0x100 */
                    }
                    memset(f.ram, 0xA5, sizeof f.ram);
                    REG(&f, 4) = mem_vals[v];
                    memcpy(want, f.ram, sizeof want);
                    put_le(&want[ea - FIX_RAM_BASE], mem_vals[v], stores[s].size);
                    step_ok(t_, &f, CF_NONE, 0u);
                    if (TEST_FAILED()) return;
                    if (memcmp(want, f.ram, sizeof want) != 0) {
                        test_note(t_, "%s v=0x%08lX ea=0x%lX form=%u",
                                  ksvm_op_name(stores[s].op_i), (unsigned long)mem_vals[v],
                                  (unsigned long)ea, form);
                    }
                    ASSERT_MEM_EQ(want, f.ram, sizeof want);
                }
            }
        }
    }
}

/* rd = rb no load; store do próprio registrador de endereço. */
TEST(load_store_register_aliasing)
{
    fixture f;
    prep(&f, OP_I(LDW, 2, 2, 8));
    REG(&f, 2) = FIX_RAM_BASE;
    fix_set_ram32(&f, FIX_RAM_BASE + 8u, 0xFEEDFACEu);
    step_ok(t_, &f, 2u, 0xFEEDFACEu);
    if (TEST_FAILED()) return;

    prep(&f, OP_I(STW, 2, 2, 0));
    REG(&f, 2) = FIX_RAM_BASE + 0x40u;
    step_ok(t_, &f, CF_NONE, 0u);
    if (TEST_FAILED()) return;
    ASSERT_EQ_U32(fix_ram32(&f, FIX_RAM_BASE + 0x40u), FIX_RAM_BASE + 0x40u);

    prep(&f, OP_R(LDWX, 3, 2, 3));                 /* rd = rc */
    REG(&f, 2) = FIX_RAM_BASE;
    REG(&f, 3) = 0x10u;
    fix_set_ram32(&f, FIX_RAM_BASE + 0x10u, 0x0BADF00Du);
    step_ok(t_, &f, 3u, 0x0BADF00Du);
    if (TEST_FAILED()) return;

    prep(&f, OP_R(STWX, 0, 2, 3));                 /* fonte zr */
    REG(&f, 2) = FIX_RAM_BASE;
    REG(&f, 3) = 0x20u;
    fix_set_ram32(&f, FIX_RAM_BASE + 0x20u, 0xFFFFFFFFu);
    step_ok(t_, &f, CF_NONE, 0u);
    if (TEST_FAILED()) return;
    ASSERT_EQ_U32(fix_ram32(&f, FIX_RAM_BASE + 0x20u), 0u);
}

/* Bordas da RAM: último byte/half/word cabem; um além falha. Idem para
 * o começo da RAM. fault_addr = EA. */
TEST(ram_edges_all_sizes)
{
    unsigned l, s;
    for (l = 0; l < sizeof loads / sizeof loads[0]; l++) {
        unsigned size = loads[l].size;
        uint32_t d;
        for (d = 0; d < 4u; d++) {
            /* ea = END - size + d: d = 0 cabe; d > 0 cruza o fim */
            fixture f;
            uint32_t ea = FIX_RAM_END - size + d;
            prep(&f, ksvm_enc_i(loads[l].op_i, 1, 2, 0));
            memset(f.ram, 0x80, sizeof f.ram);
            REG(&f, 2) = ea;
            if (d == 0u) {
                step_ok(t_, &f, 1u, expect_load(0x80808080u, size, loads[l].sign));
            } else {
                step_fault(t_, &f, KSVM_FAULT_MEM_RANGE, ea);   /* passa do fim */
            }
            if (TEST_FAILED()) {
                test_note(t_, "%s ea=0x%lX", ksvm_op_name(loads[l].op_i), (unsigned long)ea);
                return;
            }
        }
        {
            fixture f;
            prep(&f, ksvm_enc_i(loads[l].op_i, 1, 2, -1));
            REG(&f, 2) = FIX_RAM_BASE;                    /* ea = base - 1 */
            step_fault(t_, &f, KSVM_FAULT_MEM_RANGE, FIX_RAM_BASE - 1u);
            if (TEST_FAILED()) return;
        }
    }
    for (s = 0; s < sizeof stores / sizeof stores[0]; s++) {
        unsigned size = stores[s].size;
        uint32_t d;
        for (d = 0; d < size; d++) {
            fixture f;
            uint32_t ea = FIX_RAM_END - size + d;
            prep(&f, ksvm_enc_i(stores[s].op_i, 4, 2, 0));
            REG(&f, 2) = ea;
            REG(&f, 4) = 0xFFFFFFFFu;
            if (d == 0u) {
                step_ok(t_, &f, CF_NONE, 0u);
                if (TEST_FAILED()) return;
                ASSERT_EQ_U32(f.ram[FIX_RAM_SIZE - 1u], 0xFFu);
                ASSERT_EQ_U32(f.ram[FIX_RAM_SIZE - size], 0xFFu);
            } else {
                step_fault(t_, &f, KSVM_FAULT_MEM_RANGE, ea);
            }
            if (TEST_FAILED()) return;
        }
        {
            fixture f;
            prep(&f, ksvm_enc_i(stores[s].op_i, 4, 2, 0));
            REG(&f, 2) = FIX_RAM_BASE - 1u;
            REG(&f, 4) = 0xFFFFFFFFu;
            step_fault(t_, &f, KSVM_FAULT_MEM_RANGE, FIX_RAM_BASE - 1u);
            if (TEST_FAILED()) return;
        }
    }
}

#if KSVM_MAX_REGIONS >= 3
/* Duas RAMs lado a lado: [END-k, END+...) tem todos os bytes mapeados, mas
 * o acesso cruza a borda entre regiões -> MEM_RANGE, sem escrita em
 * nenhuma das duas (doc 04 §5.2 + bus.h). */
TEST(access_crossing_two_adjacent_regions_faults)
{
    unsigned l, s, d;
    for (l = 0; l < sizeof loads / sizeof loads[0]; l++) {
        if (loads[l].size == 1u) continue;
        for (d = 1; d < loads[l].size; d++) {
            fixture f;
            uint8_t ram2[64];
            uint32_t ea = FIX_RAM_END - d;
            prep(&f, ksvm_enc_i(loads[l].op_i, 1, 2, 0));
            memset(ram2, 0x11, sizeof ram2);
            ASSERT_EQ(ksvm_map_ram(&f.vm, FIX_RAM_END, ram2, sizeof ram2), KSVM_OK);
            REG(&f, 2) = ea;
            step_fault(t_, &f, KSVM_FAULT_MEM_RANGE, ea);
            if (TEST_FAILED()) return;
        }
    }
    for (s = 0; s < sizeof stores / sizeof stores[0]; s++) {
        if (stores[s].size == 1u) continue;
        for (d = 1; d < stores[s].size; d++) {
            fixture f;
            uint8_t ram2[64], zero2[64];
            uint32_t ea = FIX_RAM_END - d;
            prep(&f, ksvm_enc_r(stores[s].op_x, 4, 2, 3));
            memset(ram2, 0, sizeof ram2);
            memset(zero2, 0, sizeof zero2);
            ASSERT_EQ(ksvm_map_ram(&f.vm, FIX_RAM_END, ram2, sizeof ram2), KSVM_OK);
            REG(&f, 2) = ea;
            REG(&f, 3) = 0u;
            REG(&f, 4) = 0xFFFFFFFFu;
            step_fault(t_, &f, KSVM_FAULT_MEM_RANGE, ea);
            if (TEST_FAILED()) return;
            ASSERT_MEM_EQ(ram2, zero2, sizeof ram2);
        }
    }
    /* Byte imediatamente após a RAM, na segunda região: funciona. */
    {
        fixture f;
        uint8_t ram2[64];
        memset(ram2, 0x5A, sizeof ram2);
        prep(&f, OP_I(LDW, 1, 2, 0));
        ASSERT_EQ(ksvm_map_ram(&f.vm, FIX_RAM_END, ram2, sizeof ram2), KSVM_OK);
        REG(&f, 2) = FIX_RAM_END;
        step_ok(t_, &f, 1u, 0x5A5A5A5Au);
    }
}
#endif

/* ROM (código) é legível; cruzar ROM -> RAM adjacente falha. */
TEST(rom_reads_writes_and_rom_ram_border)
{
    unsigned s;
    fixture f;
    uint8_t ram2[16];

    prep(&f, OP_I(LDW, 1, 0, 0));                /* lê a própria instrução */
    step_ok(t_, &f, 1u, OP_I(LDW, 1, 0, 0));
    if (TEST_FAILED()) return;
    prep(&f, OP_I(LDBS, 1, 0, 4));              /* byte 0x76 (HALT) */
    step_ok(t_, &f, 1u, KSVM_OP_HALT);
    if (TEST_FAILED()) return;
    prep(&f, OP_I(LDHS, 1, 2, 0));
    REG(&f, 2) = FIX_CODE_SIZE - 2u;            /* 0xFFFF do código apagado */
    step_ok(t_, &f, 1u, 0xFFFFFFFFu);
    if (TEST_FAILED()) return;

    for (s = 0; s < sizeof stores / sizeof stores[0]; s++) {
        prep(&f, ksvm_enc_i(stores[s].op_i, 4, 2, 0));
        REG(&f, 2) = 8u;
        step_fault(t_, &f, KSVM_FAULT_MEM_PERM, 8u);
        if (TEST_FAILED()) return;
    }

#if KSVM_MAX_REGIONS >= 3
    prep(&f, OP_I(LDW, 1, 2, 0));
    memset(ram2, 0, sizeof ram2);
    ASSERT_EQ(ksvm_map_ram(&f.vm, FIX_CODE_SIZE, ram2, sizeof ram2), KSVM_OK);
    REG(&f, 2) = FIX_CODE_SIZE - 2u;
    step_fault(t_, &f, KSVM_FAULT_MEM_RANGE, FIX_CODE_SIZE - 2u);
#else
    (void)ram2;
#endif
}

/* EA com wrap: 0xFFFFFFFF + 1 = 0 (lê o código), e endereços não mapeados
 * no topo falham. */
TEST(effective_address_wraps_modulo_2_32)
{
    fixture f;
    prep(&f, OP_I(LDB, 1, 2, 1));
    REG(&f, 2) = 0xFFFFFFFFu;
    step_ok(t_, &f, 1u, KSVM_OP_LDB);           /* [0] = byte baixo da instrução */
    if (TEST_FAILED()) return;

    prep(&f, OP_R(LDWX, 1, 2, 3));
    REG(&f, 2) = 0x80000000u;
    REG(&f, 3) = 0x80000000u + FIX_RAM_BASE;    /* soma = FIX_RAM_BASE */
    fix_set_ram32(&f, FIX_RAM_BASE, 0x13572468u);
    step_ok(t_, &f, 1u, 0x13572468u);
    if (TEST_FAILED()) return;

    prep(&f, OP_I(LDB, 1, 2, 0));
    REG(&f, 2) = 0xFFFFFFFFu;
    step_fault(t_, &f, KSVM_FAULT_MEM_RANGE, 0xFFFFFFFFu);
}

#if KSVM_MAX_REGIONS >= 3
/* Região no topo do espaço: acesso que passaria de 2^32 não dá a volta
 * para o endereço 0 (onde há ROM mapeada). */
TEST(access_at_top_of_address_space_does_not_wrap)
{
    unsigned l, s;
    for (l = 0; l < sizeof loads / sizeof loads[0]; l++) {
        uint32_t d;
        for (d = 0; d < loads[l].size; d++) {
            fixture f;
            uint8_t top[256];
            uint32_t ea = 0xFFFFFFFFu - (loads[l].size - 1u) + d;
            memset(top, 0x7F, sizeof top);
            prep(&f, ksvm_enc_i(loads[l].op_i, 1, 2, 0));
            ASSERT_EQ(ksvm_map_ram(&f.vm, 0xFFFFFF00u, top, sizeof top), KSVM_OK);
            REG(&f, 2) = ea;
            if (d == 0u) {
                step_ok(t_, &f, 1u, expect_load(0x7F7F7F7Fu, loads[l].size, loads[l].sign));
            } else {
                step_fault(t_, &f, KSVM_FAULT_MEM_RANGE, ea);
            }
            if (TEST_FAILED()) return;
        }
    }
    for (s = 0; s < sizeof stores / sizeof stores[0]; s++) {
        uint32_t d;
        for (d = 1; d < stores[s].size; d++) {
            fixture f;
            uint8_t top[256], zero[256];
            uint32_t ea = 0xFFFFFFFFu - (stores[s].size - 1u) + d;
            memset(top, 0, sizeof top);
            memset(zero, 0, sizeof zero);
            prep(&f, ksvm_enc_i(stores[s].op_i, 4, 2, 0));
            ASSERT_EQ(ksvm_map_ram(&f.vm, 0xFFFFFF00u, top, sizeof top), KSVM_OK);
            REG(&f, 2) = ea;
            REG(&f, 4) = 0xFFFFFFFFu;
            step_fault(t_, &f, KSVM_FAULT_MEM_RANGE, ea);
            if (TEST_FAILED()) return;
            ASSERT_MEM_EQ(top, zero, sizeof top);
        }
    }
}
#endif

/* ---- Pilha ------------------------------------------------------------ */

TEST(push_pop_conform)
{
    unsigned i;
    for (i = 0; i < cf_nvals; i++) {
        fixture f;
        prep(&f, OP_R(PUSH, 5, 6, 7));            /* só ra é fonte */
        REG(&f, KSVM_REG_SP) = FIX_RAM_END - 8u;
        REG(&f, 5) = cf_vals[i];
        {
            ksvm_cpu want = f.vm.cpu;
            want.r[KSVM_REG_SP] = FIX_RAM_END - 12u;
            want.pc = 4u;
            (void)ksvm_step(&f.vm);
            ASSERT_EQ(f.vm.status, KSVM_RUNNING);
            ASSERT_MEM_EQ(&want, &f.vm.cpu, sizeof want);
            ASSERT_EQ_U32(fix_ram32(&f, FIX_RAM_END - 12u), cf_vals[i]);
        }
        prep(&f, OP_R(POP, 5, 6, 7));
        REG(&f, KSVM_REG_SP) = FIX_RAM_END - 8u;
        fix_set_ram32(&f, FIX_RAM_END - 8u, cf_vals[i]);
        {
            ksvm_cpu want = f.vm.cpu;
            want.r[KSVM_REG_SP] = FIX_RAM_END - 4u;
            want.r[5] = cf_vals[i];
            want.pc = 4u;
            (void)ksvm_step(&f.vm);
            ASSERT_EQ(f.vm.status, KSVM_RUNNING);
            ASSERT_MEM_EQ(&want, &f.vm.cpu, sizeof want);
        }
    }
}

/* A VM não exige sp alinhado (é convenção da ABI): PUSH/POP com sp
 * desalinhado funcionam byte a byte. */
TEST(push_pop_unaligned_sp)
{
    uint32_t al;
    for (al = 1; al < 4u; al++) {
        fixture f;
        prep(&f, OP_R(PUSH, 5, 0, 0));
        REG(&f, KSVM_REG_SP) = FIX_RAM_END - 16u + al;
        REG(&f, 5) = 0xA1B2C3D4u;
        (void)ksvm_step(&f.vm);
        ASSERT_EQ(f.vm.status, KSVM_RUNNING);
        ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_END - 20u + al);
        ASSERT_EQ_U32(fix_ram32(&f, FIX_RAM_END - 20u + al), 0xA1B2C3D4u);
        prep(&f, OP_R(POP, 6, 0, 0));
        REG(&f, KSVM_REG_SP) = FIX_RAM_END - 20u + al;
        fix_set_ram32(&f, FIX_RAM_END - 20u + al, 0x01020304u);
        (void)ksvm_step(&f.vm);
        ASSERT_EQ(f.vm.status, KSVM_RUNNING);
        ASSERT_EQ_U32(REG(&f, 6), 0x01020304u);
        ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_END - 16u + al);
    }
}

/* POP no último word da RAM funciona; com sp a 1..3 bytes do fim, falha. */
TEST(pop_at_ram_end)
{
    uint32_t d;
    for (d = 0; d < 4u; d++) {
        fixture f;
        prep(&f, OP_R(POP, 5, 0, 0));
        REG(&f, KSVM_REG_SP) = FIX_RAM_END - 4u + d;
        if (d == 0u) {
            fix_set_ram32(&f, FIX_RAM_END - 4u, 77u);
            (void)ksvm_step(&f.vm);
            ASSERT_EQ(f.vm.status, KSVM_RUNNING);
            ASSERT_EQ_U32(REG(&f, 5), 77u);
            ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_END);
        } else {
            step_fault(t_, &f, KSVM_FAULT_MEM_RANGE, FIX_RAM_END - 4u + d);
            if (TEST_FAILED()) return;
        }
    }
}

SUITE(conform_mem)
{
    RUN_TEST(loads_alignment_extension_and_ea);
    RUN_TEST(stores_alignment_truncation_and_neighbors);
    RUN_TEST(load_store_register_aliasing);
    RUN_TEST(ram_edges_all_sizes);
#if KSVM_MAX_REGIONS >= 3
    RUN_TEST(access_crossing_two_adjacent_regions_faults);
#endif
    RUN_TEST(rom_reads_writes_and_rom_ram_border);
    RUN_TEST(effective_address_wraps_modulo_2_32);
#if KSVM_MAX_REGIONS >= 3
    RUN_TEST(access_at_top_of_address_space_does_not_wrap);
#endif
    RUN_TEST(push_pop_conform);
    RUN_TEST(push_pop_unaligned_sp);
    RUN_TEST(pop_at_ram_end);
}
