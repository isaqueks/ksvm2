/*
 * test_programs.c — programas inteiros montados com ksvm_enc_* (via
 * miniasm) e conferidos contra o mesmo cálculo feito em C no host:
 * fatorial recursivo (CALL/RET/pilha), fibonacci iterativo, strlen/strcpy/
 * memcmp em RAM, bubble sort de 32 elementos (com e sem sinal), aritmética
 * de 64 bits com ADC/SBC/MULHU contra uint64_t, switch por tabela de saltos
 * com JMPR e código independente de posição com ADR/ADRL carregado em
 * várias bases.
 */
#include <stdlib.h>

#include "fixture.h"
#include "miniasm.h"
#include "test.h"

#define E(x)                ma_emit(&a, (x))
#define EL(op, rd, rb, imm) ma_emit_l(&a, ksvm_enc_l(KSVM_OP_##op, (rd), (rb)), (uint32_t)(imm))
#define LBL(id)             ma_label(&a, (id))
#define BR(cond, id)        ma_b(&a, KSVM_COND_##cond, (id))
#define JMPL(id)            ma_j(&a, KSVM_OP_JMP, (id))
#define CALLL(id)           ma_j(&a, KSVM_OP_CALL, (id))
#define RET                 OP_R(RET, 0, 0, 0)

static void load_prog(test_ctx *t_, fixture *f, ma *a)
{
    ASSERT_EQ(ma_link(a), 0);
    ASSERT_TRUE(a->n * 4u <= FIX_CODE_SIZE);
    fix_load(f, a->w, a->n);
}

/* ---- Fatorial recursivo ----------------------------------------------- */

#if KSVM_ENABLE_MULDIV
enum { F_FACT = 1, F_BASE };

static void build_fact(ma *aa)
{
#define a (*aa)
    ma_init(&a);
    /* main: r1 = n (definido pelo host); r1 = fact(r1); HALT(r1) */
    CALLL(F_FACT);
    E(HALT);
    /* fact(r1): frame com fp; n <= 1 -> 1, senão n * fact(n - 1) */
    LBL(F_FACT);
    E(OP_R(PUSH, KSVM_REG_FP, 0, 0));
    E(OP_R(MOV, KSVM_REG_FP, KSVM_REG_SP, 0));
    E(OP_I(CMPI, 1, 0, 1));
    BR(LE, F_BASE);
    E(OP_R(PUSH, 1, 0, 0));
    E(OP_I(SUBI, 1, 1, 1));
    CALLL(F_FACT);
    E(OP_R(POP, 2, 0, 0));
    E(OP_R(MUL, 1, 1, 2));
    E(OP_R(MOV, KSVM_REG_SP, KSVM_REG_FP, 0));
    E(OP_R(POP, KSVM_REG_FP, 0, 0));
    E(RET);
    LBL(F_BASE);
    E(OP_I(MOVI, 1, 0, 1));
    E(OP_R(MOV, KSVM_REG_SP, KSVM_REG_FP, 0));
    E(OP_R(POP, KSVM_REG_FP, 0, 0));
    E(RET);
#undef a
}

TEST(factorial_recursive)
{
    ma a;
    uint32_t n;
    build_fact(&a);
    for (n = 0; n <= 20u; n++) {
        fixture f;
        uint32_t want = 1u, i;
        for (i = 2; i <= n; i++) want *= i;     /* uint32_t: mod 2^32 como a VM */
        load_prog(t_, &f, &a);
        if (TEST_FAILED()) return;
        REG(&f, 1) = n;
        REG(&f, KSVM_REG_FP) = 0xF00DF00Du;
        ASSERT_EQ(fix_run(&f), KSVM_HALTED);
        ASSERT_EQ_U32(f.vm.exit_code, want);
        ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_END);   /* pilha equilibrada */
        ASSERT_EQ_U32(REG(&f, KSVM_REG_FP), 0xF00DF00Du);   /* callee-saved */
    }
}

/* Recursão até estourar a pilha: STACK fault no limite, não corrupção. */
TEST(factorial_deep_recursion_hits_stack_limit)
{
    ma a;
    fixture f;
    build_fact(&a);
    load_prog(t_, &f, &a);
    REG(&f, 1) = 100000u;
    ASSERT_EQ(fix_run(&f), KSVM_FAULT);
    ASSERT_EQ(f.vm.fault, KSVM_FAULT_STACK);
    ASSERT_TRUE(REG(&f, KSVM_REG_SP) >= FIX_RAM_BASE);
    ASSERT_TRUE(f.vm.fault_addr < FIX_RAM_BASE);
}
#endif

/* ---- Fibonacci iterativo ---------------------------------------------- */

TEST(fibonacci_iterative_to_ram)
{
    enum { LOOP = 1 };
    ma a;
    fixture f;
    uint32_t x = 0u, y = 1u, i;
    ma_init(&a);
    EL(MOVIL, 10, 0, FIX_RAM_BASE);
    E(OP_I(MOVI, 1, 0, 0));
    E(OP_I(MOVI, 2, 0, 1));
    E(OP_I(MOVI, 3, 0, 0));
    E(OP_I(MOVI, 4, 0, 60));
    LBL(LOOP);
    E(OP_I(STW, 1, 10, 0));
    E(OP_R(ADD, 5, 1, 2));
    E(OP_R(MOV, 1, 2, 0));
    E(OP_R(MOV, 2, 5, 0));
    E(OP_I(ADDI, 10, 10, 4));
    E(OP_I(ADDI, 3, 3, 1));
    E(OP_R(CMP, 3, 4, 0));
    BR(LO, LOOP);
    E(HALT);
    load_prog(t_, &f, &a);
    if (TEST_FAILED()) return;
    ASSERT_EQ(fix_run(&f), KSVM_HALTED);
    for (i = 0; i < 60u; i++) {
        uint32_t z = x + y;
        ASSERT_EQ_U32(fix_ram32(&f, FIX_RAM_BASE + 4u * i), x);
        x = y;
        y = z;
    }
    ASSERT_EQ_U32(fix_ram32(&f, FIX_RAM_BASE + 240u), 0u);
}

/* ---- strlen / strcpy / memcmp ---------------------------------------- */

enum { S_STRLEN = 1, S_SL_LOOP, S_SL_DONE, S_STRCPY, S_SC_LOOP,
       S_MEMCMP, S_MC_LOOP, S_MC_EQ, S_MC_NE };

#define SRC (FIX_RAM_BASE + 0x40u)
#define DST (FIX_RAM_BASE + 0x200u)

TEST(string_functions)
{
    static const char text[] = "Hello, KSVM v2! \x80\xFF\x7F bytes altos e baixos.";
    ma a;
    fixture f;
    uint32_t len = (uint32_t)strlen(text);
    ma_init(&a);
    /* main */
    EL(MOVIL, 1, 0, SRC);
    CALLL(S_STRLEN);
    E(OP_R(MOV, 20, 1, 0));
    EL(MOVIL, 1, 0, DST);
    EL(MOVIL, 2, 0, SRC);
    CALLL(S_STRCPY);
    E(OP_R(MOV, 23, 1, 0));
    EL(MOVIL, 1, 0, SRC);
    EL(MOVIL, 2, 0, DST);
    E(OP_I(ADDI, 3, 20, 1));
    CALLL(S_MEMCMP);
    E(OP_R(MOV, 21, 1, 0));
    EL(MOVIL, 8, 0, DST);
    E(OP_I(MOVI, 9, 0, 'A'));
    E(OP_I(STB, 9, 8, 5));                        /* DST[5] = 'A' (era ',') */
    EL(MOVIL, 1, 0, SRC);
    EL(MOVIL, 2, 0, DST);
    E(OP_I(ADDI, 3, 20, 1));
    CALLL(S_MEMCMP);
    E(OP_R(MOV, 22, 1, 0));
    EL(MOVIL, 1, 0, SRC + 17u);                   /* compara 0xFF com 0x80: > 0 */
    EL(MOVIL, 2, 0, SRC + 16u);
    E(OP_I(MOVI, 3, 0, 1));
    CALLL(S_MEMCMP);
    E(OP_R(MOV, 24, 1, 0));
    E(HALT);
    /* strlen(r1) -> r1 */
    LBL(S_STRLEN);
    E(OP_R(MOV, 2, 1, 0));
    LBL(S_SL_LOOP);
    E(OP_I(LDB, 3, 2, 0));
    E(OP_I(CMPI, 3, 0, 0));
    BR(EQ, S_SL_DONE);
    E(OP_I(ADDI, 2, 2, 1));
    BR(AL, S_SL_LOOP);
    LBL(S_SL_DONE);
    E(OP_R(SUB, 1, 2, 1));
    E(RET);
    /* strcpy(r1 = dst, r2 = src) -> r1 = dst */
    LBL(S_STRCPY);
    E(OP_R(MOV, 3, 1, 0));
    LBL(S_SC_LOOP);
    E(OP_I(LDB, 4, 2, 0));
    E(OP_I(STB, 4, 3, 0));
    E(OP_I(ADDI, 2, 2, 1));
    E(OP_I(ADDI, 3, 3, 1));
    E(OP_I(CMPI, 4, 0, 0));
    BR(NE, S_SC_LOOP);
    E(RET);
    /* memcmp(r1, r2, r3) -> r1 = a[i] - b[i] (bytes sem sinal) ou 0 */
    LBL(S_MEMCMP);
    E(OP_I(MOVI, 4, 0, 0));
    LBL(S_MC_LOOP);
    E(OP_R(CMP, 4, 3, 0));
    BR(HS, S_MC_EQ);
    E(OP_R(LDBX, 5, 1, 4));
    E(OP_R(LDBX, 6, 2, 4));
    E(OP_R(SUB, 7, 5, 6));
    BR(NE, S_MC_NE);
    E(OP_I(ADDI, 4, 4, 1));
    BR(AL, S_MC_LOOP);
    LBL(S_MC_EQ);
    E(OP_I(MOVI, 1, 0, 0));
    E(RET);
    LBL(S_MC_NE);
    E(OP_R(MOV, 1, 7, 0));
    E(RET);

    load_prog(t_, &f, &a);
    if (TEST_FAILED()) return;
    memcpy(&f.ram[SRC - FIX_RAM_BASE], text, sizeof text);
    memset(&f.ram[DST - FIX_RAM_BASE], 0xEE, 64);
    ASSERT_EQ(fix_run(&f), KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 20), len);
    ASSERT_EQ_U32(REG(&f, 23), DST);
    ASSERT_EQ_U32(REG(&f, 21), 0u);
    ASSERT_EQ_U32(REG(&f, 22), (uint32_t)',' - (uint32_t)'A');
    ASSERT_EQ_U32(REG(&f, 24), 0xFFu - 0x80u);
    ASSERT_EQ_U32(f.ram[DST - FIX_RAM_BASE + len + 1u], 0xEEu);  /* parou no NUL */
    f.ram[DST - FIX_RAM_BASE + 5u] = ',';
    ASSERT_MEM_EQ(&f.ram[DST - FIX_RAM_BASE], text, sizeof text);
    ASSERT_EQ_U32(REG(&f, KSVM_REG_SP), FIX_RAM_END);
}

/* ---- Bubble sort de 32 elementos -------------------------------------- */

#define ARR (FIX_RAM_BASE + 0x100u)
#define NSORT 32u

static int cmp_s32(const void *x, const void *y)
{
    int32_t a = *(const int32_t *)x, b = *(const int32_t *)y;
    return (a > b) - (a < b);
}

static int cmp_u32(const void *x, const void *y)
{
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return (a > b) - (a < b);
}

/* noswap_cond: LE (com sinal) ou LS (sem sinal). */
static void build_bubble(ma *aa, uint8_t noswap_cond)
{
    enum { OUTER = 1, INNER, NOSWAP, OUTER_END, DONE };
#define a (*aa)
    ma_init(&a);
    EL(MOVIL, 10, 0, ARR);
    E(OP_I(MOVI, 11, 0, NSORT));
    LBL(OUTER);
    E(OP_I(SUBI, 11, 11, 1));
    E(OP_I(CMPI, 11, 0, 0));
    BR(LE, DONE);
    E(OP_I(MOVI, 12, 0, 0));
    E(OP_R(MOV, 13, 10, 0));
    E(OP_I(MOVI, 15, 0, 0));
    LBL(INNER);
    E(OP_R(CMP, 12, 11, 0));
    BR(GE, OUTER_END);
    E(OP_I(LDW, 1, 13, 0));
    E(OP_I(LDW, 2, 13, 4));
    E(OP_R(CMP, 1, 2, 0));
    ma_b(&a, noswap_cond, NOSWAP);
    E(OP_I(STW, 2, 13, 0));
    E(OP_I(STW, 1, 13, 4));
    E(OP_I(MOVI, 15, 0, 1));
    LBL(NOSWAP);
    E(OP_I(ADDI, 13, 13, 4));
    E(OP_I(ADDI, 12, 12, 1));
    BR(AL, INNER);
    LBL(OUTER_END);
    E(OP_I(CMPI, 15, 0, 0));
    BR(NE, OUTER);
    LBL(DONE);
    E(HALT);
#undef a
}

static void fill_sort_input(uint32_t *v, uint32_t seed)
{
    uint32_t i, x = seed;
    for (i = 0; i < NSORT; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        v[i] = x;
    }
    v[3] = 0x80000000u;          /* INT32_MIN */
    v[7] = 0x7FFFFFFFu;          /* INT32_MAX */
    v[11] = 0u;
    v[12] = 0xFFFFFFFFu;
    v[20] = v[21] = 42u;         /* repetidos */
    v[31] = 1u;
}

TEST(bubble_sort_32_signed_and_unsigned)
{
    unsigned variant, seed;
    for (variant = 0; variant < 2u; variant++) {
        for (seed = 1; seed <= 4u; seed++) {
            ma a;
            fixture f;
            uint32_t in[NSORT], want[NSORT], i;
            build_bubble(&a, variant ? KSVM_COND_LS : KSVM_COND_LE);
            load_prog(t_, &f, &a);
            if (TEST_FAILED()) return;
            fill_sort_input(in, 0x9E3779B9u * seed);
            if (seed == 4u) {                            /* já ordenado ao contrário */
                for (i = 0; i < NSORT; i++) in[i] = (NSORT - i) * 0x08000001u;
            }
            memcpy(want, in, sizeof want);
            qsort(want, NSORT, sizeof want[0], variant ? cmp_u32 : cmp_s32);
            for (i = 0; i < NSORT; i++) fix_set_ram32(&f, ARR + 4u * i, in[i]);
            ASSERT_EQ(fix_run(&f), KSVM_HALTED);
            for (i = 0; i < NSORT; i++) {
                if (fix_ram32(&f, ARR + 4u * i) != want[i]) {
                    test_note(t_, "variante %s semente %u indice %u", variant ? "u32" : "s32", seed, i);
                }
                ASSERT_EQ_U32(fix_ram32(&f, ARR + 4u * i), want[i]);
            }
            ASSERT_EQ_U32(fix_ram32(&f, ARR - 4u), 0u);           /* sem escrita fora */
            ASSERT_EQ_U32(fix_ram32(&f, ARR + 4u * NSORT), 0u);
        }
    }
}

/* ---- Aritmética de 64 bits ------------------------------------------- */

#if KSVM_ENABLE_MULDIV
/* Entradas: x = r2:r1, y = r4:r3. Saídas: soma r6:r5, diferença r8:r7,
 * produto (64 bits baixos) r10:r9, r11 = x < y com sinal, r12 = x < y sem
 * sinal, r14:r13 = produto 32x32 completo dos words baixos (MUL+MULHU). */
static void build_u64(ma *aa)
{
    enum { SLT = 1, SDONE, ULT, UDONE };
#define a (*aa)
    ma_init(&a);
    E(OP_R(ADD, 5, 1, 3));
    E(OP_R(ADC, 6, 2, 4));
    E(OP_R(SUB, 7, 1, 3));
    E(OP_R(SBC, 8, 2, 4));
    E(OP_R(MUL, 9, 1, 3));
    E(OP_R(MULHU, 10, 1, 3));
    E(OP_R(MOV, 14, 10, 0));
    E(OP_R(MOV, 13, 9, 0));
    E(OP_R(MUL, 15, 1, 4));
    E(OP_R(ADD, 10, 10, 15));
    E(OP_R(MUL, 15, 2, 3));
    E(OP_R(ADD, 10, 10, 15));
    /* comparação 64 bits com sinal: SUB + SBC, depois LT (N != V) */
    E(OP_R(SUB, 0, 1, 3));
    E(OP_R(SBC, 0, 2, 4));
    E(OP_I(MOVI, 11, 0, 0));
    BR(GE, SDONE);
    E(OP_I(MOVI, 11, 0, 1));
    LBL(SDONE);
    E(OP_R(SUB, 0, 1, 3));
    E(OP_R(SBC, 0, 2, 4));
    E(OP_I(MOVI, 12, 0, 0));
    BR(HS, UDONE);
    E(OP_I(MOVI, 12, 0, 1));
    LBL(UDONE);
    E(HALT);
#undef a
}

TEST(arith_64bit_against_uint64)
{
    static const uint64_t vals[] = {
        0u, 1u, 2u, UINT64_C(0xFFFFFFFF), UINT64_C(0x100000000), UINT64_C(0x1FFFFFFFF),
        UINT64_C(0x7FFFFFFFFFFFFFFF), UINT64_C(0x8000000000000000), UINT64_C(0xFFFFFFFFFFFFFFFF),
        UINT64_C(0xFFFFFFFF00000000), UINT64_C(0x00000000FFFFFFFE), UINT64_C(0x123456789ABCDEF0),
        UINT64_C(0xFEDCBA9876543210), UINT64_C(0x8000000000000001), UINT64_C(0x7FFFFFFF80000000),
    };
    ma a;
    unsigned i, j;
    build_u64(&a);
    ASSERT_EQ(ma_link(&a), 0);
    for (i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        for (j = 0; j < sizeof vals / sizeof vals[0]; j++) {
            fixture f;
            uint64_t x = vals[i], y = vals[j];
            uint64_t sum = x + y, dif = x - y, prod = x * y;
            uint64_t full = (x & 0xFFFFFFFFu) * (y & 0xFFFFFFFFu);
            int64_t sx = (x >> 63) ? -(int64_t)(~x) - 1 : (int64_t)x;
            int64_t sy = (y >> 63) ? -(int64_t)(~y) - 1 : (int64_t)y;
            fix_load(&f, a.w, a.n);
            REG(&f, 1) = (uint32_t)x;
            REG(&f, 2) = (uint32_t)(x >> 32);
            REG(&f, 3) = (uint32_t)y;
            REG(&f, 4) = (uint32_t)(y >> 32);
            ASSERT_EQ(fix_run(&f), KSVM_HALTED);
            if (REG(&f, 6) != (uint32_t)(sum >> 32) || REG(&f, 8) != (uint32_t)(dif >> 32) ||
                REG(&f, 10) != (uint32_t)(prod >> 32) || REG(&f, 11) != (uint32_t)(sx < sy)) {
                test_note(t_, "x=0x%016llX y=0x%016llX", (unsigned long long)x, (unsigned long long)y);
            }
            ASSERT_EQ_U32(REG(&f, 5), (uint32_t)sum);
            ASSERT_EQ_U32(REG(&f, 6), (uint32_t)(sum >> 32));
            ASSERT_EQ_U32(REG(&f, 7), (uint32_t)dif);
            ASSERT_EQ_U32(REG(&f, 8), (uint32_t)(dif >> 32));
            ASSERT_EQ_U32(REG(&f, 9), (uint32_t)prod);
            ASSERT_EQ_U32(REG(&f, 10), (uint32_t)(prod >> 32));
            ASSERT_EQ_U32(REG(&f, 11), (uint32_t)(sx < sy));
            ASSERT_EQ_U32(REG(&f, 12), (uint32_t)(x < y));
            ASSERT_EQ_U32(REG(&f, 13), (uint32_t)full);
            ASSERT_EQ_U32(REG(&f, 14), (uint32_t)(full >> 32));
        }
    }
}
#endif

/* ---- switch por tabela de saltos (JMPR) ------------------------------- */

TEST(switch_jump_table_jmpr)
{
    enum { DEFAULT = 1, END, TABLE, C0, C1, C2, C3, C4 };
    static const uint32_t sels[] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 0xFFFFFFFFu, 0x80000000u };
    ma a;
    unsigned i;
    ma_init(&a);
    E(OP_I(CMPI, 1, 0, 5));
    BR(HS, DEFAULT);                               /* sem sinal: negativos caem aqui */
    EL(MOVIL, 2, 0, 0);                            /* corrigido abaixo: base absoluta */
    E(OP_I(SHLI, 3, 1, 2));
    E(OP_R(LDWX, 4, 2, 3));
    E(OP_R(JMPR, 0, 4, 0));
    LBL(C0); E(OP_I(MOVI, 5, 0, 100)); JMPL(END);
    LBL(C1); E(OP_I(MOVI, 5, 0, 111)); JMPL(END);
    LBL(C2); E(OP_I(MOVI, 5, 0, 122)); JMPL(END);
    LBL(C3); E(OP_I(MOVI, 5, 0, 133)); JMPL(END);
    LBL(C4); E(OP_I(MOVI, 5, 0, 144)); JMPL(END);
    LBL(DEFAULT); E(OP_I(MOVI, 5, 0, -1));
    LBL(END); E(HALT);
    LBL(TABLE);                                    /* tabela na ROM, endereços absolutos */
    ma_word_abs(&a, C0, 0u);
    ma_word_abs(&a, C1, 0u);
    ma_word_abs(&a, C2, 0u);
    ma_word_abs(&a, C3, 0u);
    ma_word_abs(&a, C4, 0u);
    ASSERT_EQ(ma_link(&a), 0);
    a.w[3] = ma_addr(&a, TABLE);                   /* imm32 do MOVIL r2 (palavras 2-3) */
    for (i = 0; i < sizeof sels / sizeof sels[0]; i++) {
        fixture f;
        uint32_t want = sels[i] < 5u ? 100u + 11u * sels[i] : 0xFFFFFFFFu;
        fix_load(&f, a.w, a.n);
        REG(&f, 1) = sels[i];
        ASSERT_EQ(fix_run(&f), KSVM_HALTED);
        ASSERT_EQ_U32(REG(&f, 5), want);
    }
}

/* ---- Código independente de posição (ADR/ADRL) ------------------------- */

enum { P_START = 1, P_LOOP, P_FUNC, P_RTABLE, P_DATA, P_C0, P_C1, P_C2, P_END };

static const uint32_t pic_data[8] = {
    1u, 2u, 3u, 0x10000000u, 0x7FFFFFFFu, 0xFFFFFFFFu, 12345u, 0x80000000u,
};

static void build_pic(ma *aa)
{
    unsigned i;
#define a (*aa)
    ma_init(&a);
    LBL(P_START);
    ma_adrl(&a, 10, P_DATA);                     /* r10 = &data (longo) */
    E(OP_I(MOVI, 11, 0, 0));
    E(OP_I(MOVI, 12, 0, 0));
    LBL(P_LOOP);
    E(OP_I(SHLI, 13, 12, 2));
    E(OP_R(LDWX, 14, 10, 13));
    E(OP_R(ADD, 11, 11, 14));
    E(OP_I(ADDI, 12, 12, 1));
    E(OP_I(CMPI, 12, 0, 8));
    BR(LT, P_LOOP);
    ma_adr(&a, 15, P_FUNC);                      /* ponteiro de função relativo */
    E(OP_R(CALLR, 0, 15, 0));
    ma_adr(&a, 2, P_RTABLE);                     /* tabela de deslocamentos */
    E(OP_I(SHLI, 3, 1, 2));
    E(OP_R(LDWX, 4, 2, 3));
    E(OP_R(ADD, 4, 4, 2));
    E(OP_R(JMPR, 0, 4, 0));
    LBL(P_C0); E(OP_I(MOVI, 5, 0, 200)); JMPL(P_END);
    LBL(P_C1); E(OP_I(MOVI, 5, 0, 201)); JMPL(P_END);
    LBL(P_C2); E(OP_I(MOVI, 5, 0, 202)); JMPL(P_END);
    LBL(P_END);
    ma_adrl(&a, 6, P_START);                     /* r6 = endereço de carga */
    E(HALT);
    LBL(P_FUNC);
    E(OP_R(ADD, 16, 11, 11));
    E(OP_R(ADD, 16, 16, 11));
    E(RET);
    LBL(P_RTABLE);
    ma_word_diff(&a, P_C0, P_RTABLE);
    ma_word_diff(&a, P_C1, P_RTABLE);
    ma_word_diff(&a, P_C2, P_RTABLE);
    LBL(P_DATA);
    for (i = 0; i < 8u; i++) E(pic_data[i]);
#undef a
}

/* Roda o código PIC com a ROM em "base". */
static void run_pic_at(test_ctx *t_, const ma *a, uint32_t base, uint32_t sel, ksvm *vm,
                       uint8_t *code, uint32_t code_size, uint8_t *ram)
{
    unsigned i;
    memset(code, 0xFF, code_size);
    for (i = 0; i < a->n; i++) ksvm_st32le(&code[4u * i], a->w[i]);
    memset(ram, 0, FIX_RAM_SIZE);
    ksvm_init(vm, NULL);
    ASSERT_EQ(ksvm_map_rom(vm, base, code, code_size, NULL), KSVM_OK);
    ASSERT_EQ(ksvm_map_ram(vm, FIX_RAM_BASE, ram, FIX_RAM_SIZE), KSVM_OK);
    vm->stack_limit = FIX_RAM_BASE;
    ksvm_reset(vm, base, FIX_RAM_END);
    vm->cpu.r[1] = sel;
    ASSERT_EQ(ksvm_run(vm, FIX_MAX_STEPS), KSVM_HALTED);
}

TEST(pic_code_runs_at_any_base)
{
    static const uint32_t bases[] = { 0u, 0x400u, 0x40000u, 0x7FFFF000u, 0xFFFFF000u };
    static uint8_t code[0x1000];
    static uint8_t ram[FIX_RAM_SIZE];
    ma a;
    uint32_t sum = 0u, i, sel;
    build_pic(&a);
    ASSERT_EQ(ma_link(&a), 0);
    for (i = 0; i < 8u; i++) sum += pic_data[i];
    for (sel = 0; sel < 3u; sel++) {
        for (i = 0; i < sizeof bases / sizeof bases[0]; i++) {
            ksvm vm;
            run_pic_at(t_, &a, bases[i], sel, &vm, code, sizeof code, ram);
            if (TEST_FAILED()) {
                test_note(t_, "base 0x%08lX", (unsigned long)bases[i]);
                return;
            }
            ASSERT_EQ_U32(vm.cpu.r[11], sum);
            ASSERT_EQ_U32(vm.cpu.r[16], sum * 3u);
            ASSERT_EQ_U32(vm.cpu.r[5], 200u + sel);
            ASSERT_EQ_U32(vm.cpu.r[6], bases[i]);
            ASSERT_EQ_U32(vm.cpu.r[10], bases[i] + ma_addr(&a, P_DATA));
            ASSERT_EQ_U32(vm.cpu.r[15], bases[i] + ma_addr(&a, P_FUNC));
            ASSERT_EQ_U32(vm.cpu.r[KSVM_REG_SP], FIX_RAM_END);
        }
    }
}

SUITE(programs)
{
#if KSVM_ENABLE_MULDIV
    RUN_TEST(factorial_recursive);
    RUN_TEST(factorial_deep_recursion_hits_stack_limit);
#endif
    RUN_TEST(fibonacci_iterative_to_ram);
    RUN_TEST(string_functions);
    RUN_TEST(bubble_sort_32_signed_and_unsigned);
#if KSVM_ENABLE_MULDIV
    RUN_TEST(arith_64bit_against_uint64);
#endif
    RUN_TEST(switch_jump_table_jmpr);
    RUN_TEST(pic_code_runs_at_any_base);
}
