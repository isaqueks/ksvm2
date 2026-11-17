/*
 * fixture.h — VM pronta para testes de execução.
 *
 * Layout: código em 0 (ROM de acesso direto, FIX_CODE_SIZE bytes, o resto
 * preenchido com 0xFF = BAD_OPCODE), RAM em FIX_RAM_BASE, sp no fim da RAM,
 * stack_limit = FIX_RAM_BASE, HAL = hal_null.
 *
 * Programas são escritos com os codificadores ksvm_enc_* pelas macros
 * abaixo, por exemplo:
 *     LOAD(&f, LI(1, 5), OP_I(ADDI, 2, 1, 1), HALT);
 */
#ifndef KSVM_TEST_FIXTURE_H
#define KSVM_TEST_FIXTURE_H

#include "ksvm/hal_null.h"
#include "ksvm/ksvm.h"

#define FIX_CODE_SIZE 1024u
#define FIX_RAM_BASE  UINT32_C(0x10000)
#define FIX_RAM_SIZE  1024u
#define FIX_RAM_END   (FIX_RAM_BASE + FIX_RAM_SIZE)
#define FIX_MAX_STEPS 100000u

typedef struct fixture {
    ksvm          vm;
    ksvm_hal      hal;
    ksvm_hal_null hn;
    uint8_t       code[FIX_CODE_SIZE];
    uint8_t       ram[FIX_RAM_SIZE];
} fixture;

/* Monta a VM com hal_null e carrega "count" palavras de código. */
void fix_load(fixture *f, const uint32_t *words, size_t count);
/* Idem com outra HAL (NULL = sem HAL). */
void fix_load_hal(fixture *f, const uint32_t *words, size_t count, const ksvm_hal *hal);
/* ksvm_run com limite de segurança. */
ksvm_status fix_run(fixture *f);
/* Executa n instruções com ksvm_step. */
ksvm_status fix_steps(fixture *f, unsigned n);
/* Acesso direto à RAM do guest (endereço do guest). */
uint32_t fix_ram32(const fixture *f, uint32_t addr);
void fix_set_ram32(fixture *f, uint32_t addr, uint32_t v);

/* ---- Montagem de programas ------------------------------------------ */
#define OP_R(op, a, b, c)    ksvm_enc_r(KSVM_OP_##op, (a), (b), (c))
#define OP_I(op, a, b, imm)  ksvm_enc_i(KSVM_OP_##op, (a), (b), (imm))
#define OP_L(op, a, b, imm)  ksvm_enc_l(KSVM_OP_##op, (a), (b)), (uint32_t)(imm)
#define OP_J(op, off)        ksvm_enc_j(KSVM_OP_##op, (off))
#define OP_B(cond, off)      ksvm_enc_b(KSVM_COND_##cond, (off))
#define LI(rd, v)            OP_L(MOVIL, (rd), 0, (v))
#define HALT                 OP_R(HALT, 0, 0, 0)
/* FLAGS = v usando r29 como rascunho (testes não usam r29 para outra coisa). */
#define SET_FLAGS(v)         LI(29, (v)), OP_I(MTSR, 0, 29, KSVM_SR_FLAGS)

#define LOAD(f, ...)                                                    \
    do {                                                                \
        const uint32_t fix_prog_[] = { __VA_ARGS__ };                   \
        fix_load((f), fix_prog_, sizeof fix_prog_ / sizeof fix_prog_[0]); \
    } while (0)

/* Carrega e roda até parar. */
#define RUN(f, ...)                                                     \
    do {                                                                \
        LOAD((f), __VA_ARGS__);                                         \
        (void)fix_run(f);                                               \
    } while (0)

#define REG(f, i) ((f)->vm.cpu.r[(i)])

#endif /* KSVM_TEST_FIXTURE_H */
