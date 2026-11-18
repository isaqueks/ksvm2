/*
 * test_exec_float.c — extensão F executada pela VM (doc 04 §5.7).
 *
 * A exatidão das operações está em test_softfloat.c; aqui se verifica a
 * decodificação (qual campo é qual operando), que só o destino muda, que
 * as flags ficam intactas (exceto FCMP, que as reescreve nos quatro casos),
 * o mapeamento FCMP + Bcc para os operadores de comparação do C, o bit F de
 * CAPS e, com KSVM_ENABLE_FLOAT=0, FAULT_BAD_OPCODE sem efeito.
 */
#include "fixture.h"
#include "test.h"

#define Z KSVM_FLAG_Z
#define N KSVM_FLAG_N
#define C KSVM_FLAG_C
#define V KSVM_FLAG_V

#define F_ONE   0x3F800000u
#define F_TWO   0x40000000u
#define F_THREE 0x40400000u
#define F_HALF  0x3F000000u
#define F_NAN   0x7FC00000u

#define SYS(n)  OP_I(SYSCALL, 0, 0, KSVM_SYSCALL_##n)

/* Os 18 opcodes da extensão, na ordem do isa.def. */
static const uint8_t float_ops[] = {
    KSVM_OP_FADD, KSVM_OP_FSUB, KSVM_OP_FMUL, KSVM_OP_FDIV, KSVM_OP_FMIN, KSVM_OP_FMAX,
    KSVM_OP_FSQRT, KSVM_OP_FNEG, KSVM_OP_FABS, KSVM_OP_FRINT, KSVM_OP_FFLOOR,
    KSVM_OP_FCEIL, KSVM_OP_FTRUNC, KSVM_OP_FTOI, KSVM_OP_FTOU, KSVM_OP_ITOF,
    KSVM_OP_UTOF, KSVM_OP_FCMP,
};
#define N_FLOAT_OPS (sizeof float_ops / sizeof float_ops[0])

/* Faixa contígua 0xA0..0xB1, todos formato R, só FCMP altera flags. */
TEST(opcodes_in_isa_table)
{
    unsigned i;
    for (i = 0; i < N_FLOAT_OPS; i++) {
        ksvm_isa_info e = ksvm_isa_get(float_ops[i]);
        ASSERT_EQ_U32(float_ops[i], 0xA0u + i);
        ASSERT_EQ(e.format, KSVM_FMT_R);
        ASSERT_EQ(e.len, 4);
        ASSERT_EQ(e.flags, float_ops[i] == KSVM_OP_FCMP ? KSVM_AFFECTS_ZNCV : KSVM_AFFECTS_NONE);
    }
    ASSERT_EQ(ksvm_isa_format(0xB2u), KSVM_FMT_INVALID);
    ASSERT_EQ(ksvm_isa_format(0x9Fu), KSVM_FMT_INVALID);
}

#if KSVM_ENABLE_FLOAT

/* "rd, ra, rb" = campos ra, rb, rc: r1 = r2 op r3, fontes e flags intactas. */
TEST(three_operand_fields)
{
    static const struct { uint8_t op; uint32_t a, b, want; } c[] = {
        { KSVM_OP_FADD, F_ONE, F_TWO, F_THREE },
        { KSVM_OP_FSUB, F_ONE, F_TWO, 0xBF800000u },        /* 1 - 2 = -1 */
        { KSVM_OP_FMUL, F_THREE, F_HALF, 0x3FC00000u },     /* 1.5 */
        { KSVM_OP_FDIV, F_ONE, F_THREE, 0x3EAAAAABu },      /* 1/3 */
        { KSVM_OP_FDIV, F_ONE, 0x80000000u, 0xFF800000u },  /* 1/-0 = -inf, sem fault */
        { KSVM_OP_FMIN, F_TWO, F_ONE, F_ONE },
        { KSVM_OP_FMAX, F_TWO, F_ONE, F_TWO },
        { KSVM_OP_FMIN, 0x7FC00001u, F_TWO, F_TWO },        /* NaN: o outro */
    };
    unsigned i;
    for (i = 0; i < sizeof c / sizeof c[0]; i++) {
        fixture f;
        RUN(&f, LI(2, c[i].a), LI(3, c[i].b), LI(4, 0x12345678u), SET_FLAGS(Z | V),
            ksvm_enc_r(c[i].op, 1, 2, 3), HALT);
        ASSERT_EQ(f.vm.status, KSVM_HALTED);
        ASSERT_EQ_U32(REG(&f, 1), c[i].want);
        ASSERT_EQ_U32(REG(&f, 2), c[i].a);
        ASSERT_EQ_U32(REG(&f, 3), c[i].b);
        ASSERT_EQ_U32(REG(&f, 4), 0x12345678u);
        ASSERT_EQ_U32(f.vm.cpu.flags, Z | V);
    }
}

/* "rd, rb" = campos ra, rb: o campo rc (r4, com lixo) é ignorado. */
TEST(two_operand_fields)
{
    static const struct { uint8_t op; uint32_t a, want; } c[] = {
        { KSVM_OP_FSQRT,  0x40800000u, F_TWO },             /* sqrt(4) */
        { KSVM_OP_FSQRT,  0xBF800000u, F_NAN },             /* sqrt(-1) */
        { KSVM_OP_FNEG,   0x7FC12345u, 0xFFC12345u },       /* só o sinal, NaN intacto */
        { KSVM_OP_FABS,   0xC0400000u, F_THREE },
        { KSVM_OP_FRINT,  0x40200000u, F_TWO },             /* 2.5 -> 2 */
        { KSVM_OP_FFLOOR, 0xBFC00000u, 0xC0000000u },       /* -1.5 -> -2 */
        { KSVM_OP_FCEIL,  0x3FC00000u, F_TWO },
        { KSVM_OP_FTRUNC, 0xBFC00000u, 0xBF800000u },
        { KSVM_OP_FTOI,   0xC0700000u, 0xFFFFFFFDu },       /* -3.75 -> -3 */
        { KSVM_OP_FTOI,   0x7F800000u, 0x7FFFFFFFu },       /* satura */
        { KSVM_OP_FTOU,   0xBF800000u, 0u },
        { KSVM_OP_FTOU,   0x4F800000u, 0xFFFFFFFFu },
        { KSVM_OP_ITOF,   0xFFFFFFFDu, 0xC0400000u },       /* -3 */
        { KSVM_OP_UTOF,   0xFFFFFFFDu, 0x4F800000u },       /* 2^32 - 3 -> 2^32 */
    };
    unsigned i;
    for (i = 0; i < sizeof c / sizeof c[0]; i++) {
        fixture f;
        RUN(&f, LI(3, c[i].a), LI(4, F_ONE), SET_FLAGS(N | C), ksvm_enc_r(c[i].op, 1, 3, 4), HALT);
        ASSERT_EQ(f.vm.status, KSVM_HALTED);
        ASSERT_EQ_U32(REG(&f, 1), c[i].want);
        ASSERT_EQ_U32(REG(&f, 3), c[i].a);
        ASSERT_EQ_U32(f.vm.cpu.flags, N | C);
    }
}

/* Destino igual a uma fonte: os operandos são lidos antes da escrita. */
TEST(destination_may_alias_source)
{
    fixture f;
    RUN(&f, LI(1, F_THREE), OP_R(FMUL, 1, 1, 1), OP_R(FSQRT, 1, 1, 0), HALT);
    ASSERT_EQ_U32(REG(&f, 1), F_THREE);
}

/* FCMP ra, rb: campos ra e rb (rc ignorado), nenhum registrador muda e
 * as quatro flags são reescritas nos quatro casos. */
TEST(fcmp_flags_four_cases)
{
    static const struct { uint32_t a, b, flags; } c[] = {
        { F_ONE, F_TWO, N },                 /* menor */
        { F_TWO, F_TWO, Z | C },             /* igual */
        { 0x80000000u, 0u, Z | C },          /* -0 == +0 */
        { F_TWO, F_ONE, C },                 /* maior */
        { F_NAN, F_ONE, C | V },             /* unordered */
        { F_ONE, 0xFF800001u, C | V },
    };
    static const uint32_t before[] = { 0u, Z | N | C | V, N | V };
    unsigned i, k;
    for (i = 0; i < sizeof c / sizeof c[0]; i++) {
        for (k = 0; k < 3u; k++) {
            fixture f;
            RUN(&f, LI(5, c[i].a), LI(6, c[i].b), LI(7, F_NAN), SET_FLAGS(before[k]),
                OP_R(FCMP, 5, 6, 7), HALT);
            ASSERT_EQ(f.vm.status, KSVM_HALTED);
            ASSERT_EQ_U32(f.vm.cpu.flags, c[i].flags);
            ASSERT_EQ_U32(REG(&f, 5), c[i].a);
            ASSERT_EQ_U32(REG(&f, 6), c[i].b);
        }
    }
}

/*
 * Mapeamento do compilador (doc 04 §5.7): depois de FCMP a, b,
 *   == EQ, != NE, < MI, <= LS, > GT, >= GE, unordered VS
 * dão exatamente o resultado do C (NaN: só != e unordered são verdade).
 */
TEST(fcmp_branch_mapping_matches_c)
{
    static const uint32_t vals[] = { F_ONE, F_TWO, 0x80000000u, 0u, F_NAN, 0xFF800000u };
    static const uint8_t conds[] = {
        KSVM_COND_EQ, KSVM_COND_NE, KSVM_COND_MI, KSVM_COND_LS,
        KSVM_COND_GT, KSVM_COND_GE, KSVM_COND_VS,
    };
    unsigned i, j, k;
    for (i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        for (j = 0; j < sizeof vals / sizeof vals[0]; j++) {
            /* relação esperada: os operadores do C sobre o float do host */
            uint32_t a = vals[i], b = vals[j];
            volatile float x, y;
            float fx, fy;
            memcpy(&fx, &a, sizeof fx);
            memcpy(&fy, &b, sizeof fy);
            x = fx;
            y = fy;
            for (k = 0; k < sizeof conds / sizeof conds[0]; k++) {
                fixture f;
                int want;
                switch (conds[k]) {
                case KSVM_COND_EQ: want = x == y; break;
                case KSVM_COND_NE: want = x != y; break;
                case KSVM_COND_MI: want = x < y; break;
                case KSVM_COND_LS: want = x <= y; break;
                case KSVM_COND_GT: want = x > y; break;
                case KSVM_COND_GE: want = x >= y; break;
                default:           want = x != x || y != y; break;
                }
                /* r9 = 1 se o desvio foi tomado */
                RUN(&f, LI(5, a), LI(6, b), OP_R(FCMP, 5, 6, 0), OP_I(MOVI, 9, 0, 0),
                    ksvm_enc_b(conds[k], 4), HALT, OP_I(MOVI, 9, 0, 1), HALT);
                ASSERT_EQ(f.vm.status, KSVM_HALTED);
                if ((int)REG(&f, 9) != want) {
                    test_note(t_, "a=0x%08lX b=0x%08lX cond=%s", (unsigned long)a,
                              (unsigned long)b, ksvm_cond_name(conds[k]));
                }
                ASSERT_EQ(REG(&f, 9), want);
            }
        }
    }
}

/* Cada opcode chama a operação certa do soft-float. */
TEST(every_opcode_dispatches_to_softfloat)
{
    static const uint32_t a = 0x40490FDBu, b = 0xBFB504F3u;   /* pi, -sqrt(2) */
    const uint32_t want[] = {
        ksvm_f32_add(a, b), ksvm_f32_sub(a, b), ksvm_f32_mul(a, b), ksvm_f32_div(a, b),
        ksvm_f32_min(a, b), ksvm_f32_max(a, b), ksvm_f32_sqrt(a), ksvm_f32_neg(a),
        ksvm_f32_abs(a), ksvm_f32_round_int(a, KSVM_F32_RNE),
        ksvm_f32_round_int(a, KSVM_F32_FLOOR), ksvm_f32_round_int(a, KSVM_F32_CEIL),
        ksvm_f32_round_int(a, KSVM_F32_TRUNC), ksvm_f32_to_i32(a), ksvm_f32_to_u32(a),
        ksvm_i32_to_f32(a), ksvm_u32_to_f32(a),
    };
    unsigned i;
    for (i = 0; i + 1u < N_FLOAT_OPS; i++) {
        fixture f;
        RUN(&f, LI(2, a), LI(3, b), ksvm_enc_r(float_ops[i], 1, 2, 3), HALT);
        ASSERT_EQ_U32(REG(&f, 1), want[i]);
    }
}

/*
 * Os pares que every_opcode_dispatches_to_softfloat não separa (pi dá 3 em
 * FRINT, FFLOOR e FTRUNC; FTOI = FTOU e ITOF = UTOF para positivos; FNEG e
 * FABS só diferem em positivos): com 2.7, -2.3 e -2.7 os quatro
 * arredondamentos, as duas conversões para inteiro e as duas de inteiro
 * dão vetores de resultados distintos, então trocar o despacho de dois
 * deles no exec.c falha aqui.
 */
TEST(dispatch_distinguishes_similar_opcodes)
{
    static const uint32_t vals[] = { 0x40490FDBu, 0x402CCCCDu, 0xC0133333u, 0xC02CCCCDu };
    static const uint8_t ops[] = {
        KSVM_OP_FNEG, KSVM_OP_FABS, KSVM_OP_FRINT, KSVM_OP_FFLOOR, KSVM_OP_FCEIL,
        KSVM_OP_FTRUNC, KSVM_OP_FTOI, KSVM_OP_FTOU, KSVM_OP_ITOF, KSVM_OP_UTOF,
    };
    unsigned i, k;
    for (k = 0; k < sizeof ops / sizeof ops[0]; k++) {
        for (i = 0; i < sizeof vals / sizeof vals[0]; i++) {
            uint32_t a = vals[i], want;
            fixture f;
            switch (ops[k]) {
            case KSVM_OP_FNEG:   want = ksvm_f32_neg(a); break;
            case KSVM_OP_FABS:   want = ksvm_f32_abs(a); break;
            case KSVM_OP_FRINT:  want = ksvm_f32_round_int(a, KSVM_F32_RNE); break;
            case KSVM_OP_FFLOOR: want = ksvm_f32_round_int(a, KSVM_F32_FLOOR); break;
            case KSVM_OP_FCEIL:  want = ksvm_f32_round_int(a, KSVM_F32_CEIL); break;
            case KSVM_OP_FTRUNC: want = ksvm_f32_round_int(a, KSVM_F32_TRUNC); break;
            case KSVM_OP_FTOI:   want = ksvm_f32_to_i32(a); break;
            case KSVM_OP_FTOU:   want = ksvm_f32_to_u32(a); break;
            case KSVM_OP_ITOF:   want = ksvm_i32_to_f32(a); break;
            default:             want = ksvm_u32_to_f32(a); break;
            }
            RUN(&f, LI(2, a), ksvm_enc_r(ops[k], 1, 2, 0), HALT);
            if (REG(&f, 1) != want) {
                test_note(t_, "op 0x%02X a=0x%08lX", ops[k], (unsigned long)a);
            }
            ASSERT_EQ_U32(REG(&f, 1), want);
        }
    }
    /* os valores separam mesmo cada par (senão o teste não pegaria a troca) */
    ASSERT_EQ_U32(ksvm_f32_round_int(0x402CCCCDu, KSVM_F32_RNE), 0x40400000u);     /* 2.7 -> 3 */
    ASSERT_EQ_U32(ksvm_f32_round_int(0x402CCCCDu, KSVM_F32_FLOOR), 0x40000000u);   /* 2 */
    ASSERT_EQ_U32(ksvm_f32_round_int(0xC0133333u, KSVM_F32_RNE), 0xC0000000u);     /* -2.3 -> -2 */
    ASSERT_EQ_U32(ksvm_f32_round_int(0xC0133333u, KSVM_F32_CEIL), 0xC0000000u);    /* -2 */
    ASSERT_EQ_U32(ksvm_f32_round_int(0xC0133333u, KSVM_F32_FLOOR), 0xC0400000u);   /* -3 */
    ASSERT_EQ_U32(ksvm_f32_round_int(0xC02CCCCDu, KSVM_F32_RNE), 0xC0400000u);     /* -2.7 -> -3 */
    ASSERT_EQ_U32(ksvm_f32_round_int(0xC02CCCCDu, KSVM_F32_TRUNC), 0xC0000000u);   /* -2 */
}

/* Programa: raiz de 2 por Newton, x = (x + 2/x) / 2, até FCMP dar igual
 * (no máximo 20 passos); confere com a mesma conta feita em C. */
TEST(newton_sqrt_program)
{
    fixture f;
    uint32_t x = F_ONE, y, n = 0u;
    int eq;

    RUN(&f, LI(2, F_TWO), LI(3, F_HALF), LI(1, F_ONE), OP_I(MOVI, 4, 0, 0),
        OP_R(FDIV, 5, 2, 1),             /* 28: r5 = 2 / x */
        OP_R(FADD, 5, 5, 1),
        OP_R(FMUL, 5, 5, 3),             /* r5 = (x + 2/x) / 2 */
        OP_I(ADDI, 4, 4, 1),
        OP_R(FCMP, 5, 1, 0),             /* 44 */
        OP_R(MOV, 1, 5, 0),
        OP_B(EQ, 8),                     /* 52: -> 64 */
        OP_I(CMPI, 4, 0, 20),
        OP_B(LO, -36),                   /* 60: -> 28 */
        HALT);                           /* 64 */
    do {
        y = ksvm_f32_mul(ksvm_f32_add(ksvm_f32_div(F_TWO, x), x), F_HALF);
        n++;
        eq = ksvm_f32_cmp(y, x) == (Z | C);
        x = y;
    } while (!eq && n < 20u);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 1), x);
    ASSERT_EQ_U32(REG(&f, 4), n);
    ASSERT_TRUE(x + 1u >= 0x3FB504F3u && x <= 0x3FB504F3u + 1u);   /* sqrt(2) +- 1 ulp */
}

#else  /* !KSVM_ENABLE_FLOAT */

/* Desligada: todo opcode F é BAD_OPCODE sem efeito (pc na instrução,
 * registradores e flags intactos), com qualquer conteúdo dos campos. */
TEST(float_disabled_is_bad_opcode)
{
    unsigned i;
    for (i = 0; i < N_FLOAT_OPS; i++) {
        fixture f;
        uint32_t w = ksvm_enc_r(float_ops[i], 1, 2, 3);
        RUN(&f, LI(1, 5), LI(2, F_ONE), LI(3, F_TWO), SET_FLAGS(Z), w, HALT);
        ASSERT_EQ(f.vm.status, KSVM_FAULT);
        ASSERT_EQ(f.vm.fault, KSVM_FAULT_BAD_OPCODE);
        ASSERT_EQ_U32(f.vm.fault_addr, w);
        ASSERT_EQ_U32(f.vm.cpu.pc, 36u);
        ASSERT_EQ_U32(REG(&f, 1), 5u);
        ASSERT_EQ_U32(f.vm.cpu.flags, Z);
    }
}

#endif /* KSVM_ENABLE_FLOAT */

/* CAPS(KSVM_CAPS_ISA): máscara de extensões; bit F conforme a config. */
TEST(caps_reports_isa_extensions)
{
    fixture f;
    uint32_t want = (KSVM_ENABLE_FLOAT ? KSVM_CAPS_ISA_F : 0u) |
                    (KSVM_ENABLE_MULDIV ? KSVM_CAPS_ISA_M : 0u) |
                    (KSVM_ENABLE_BITOPS ? KSVM_CAPS_ISA_B : 0u);
    RUN(&f, LI(1, KSVM_CAPS_ISA), SYS(CAPS), OP_R(MOV, 10, 1, 0), OP_R(MOV, 11, 2, 0),
        LI(1, KSVM_CAPS_ISA + 1u), SYS(CAPS), HALT);
    ASSERT_EQ(f.vm.status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(&f, 10), want);
    ASSERT_EQ_U32(REG(&f, 11), KSVM_ERR_OK);
    ASSERT_EQ_U32(REG(&f, 1), 0u);                   /* consulta desconhecida */
    ASSERT_EQ_U32(KSVM_CAPS_ISA_BITS, want);
#if KSVM_ENABLE_FLOAT
    ASSERT_TRUE((REG(&f, 10) & KSVM_CAPS_ISA_F) != 0u);
#else
    ASSERT_TRUE((REG(&f, 10) & KSVM_CAPS_ISA_F) == 0u);
#endif
}

SUITE(exec_float)
{
    RUN_TEST(opcodes_in_isa_table);
#if KSVM_ENABLE_FLOAT
    RUN_TEST(three_operand_fields);
    RUN_TEST(two_operand_fields);
    RUN_TEST(destination_may_alias_source);
    RUN_TEST(fcmp_flags_four_cases);
    RUN_TEST(fcmp_branch_mapping_matches_c);
    RUN_TEST(every_opcode_dispatches_to_softfloat);
    RUN_TEST(dispatch_distinguishes_similar_opcodes);
    RUN_TEST(newton_sqrt_program);
#else
    RUN_TEST(float_disabled_is_bad_opcode);
#endif
    RUN_TEST(caps_reports_isa_extensions);
}
