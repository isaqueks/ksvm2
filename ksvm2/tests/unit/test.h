/*
 * test.h — harness de testes sem dependências.
 *
 *   TEST(nome) { ... ASSERT_*(...); ... }    define um teste (static)
 *   SUITE(arquivo) { RUN_TEST(nome); ... }   lista os testes do arquivo
 *
 * Um teste definido e não listado no SUITE vira erro de compilação
 * (-Wunused-function + -Werror), então nenhum teste é esquecido.
 * Um ASSERT que falha registra a falha e encerra o teste atual (return).
 * Não há estado global: o contexto t_ é passado a cada teste.
 */
#ifndef KSVM_TEST_H
#define KSVM_TEST_H

#include <stdint.h>
#include <string.h>

typedef struct test_ctx {
    unsigned    tests_run;
    unsigned    tests_failed;
    int         current_failed;
    const char *suite;
    const char *test;
} test_ctx;

typedef void (*test_fn)(test_ctx *t_);

void test_run(test_ctx *t, const char *name, test_fn fn);
void test_fail(test_ctx *t, const char *file, int line, const char *msg);
void test_fail_u32(test_ctx *t, const char *file, int line,
                   const char *expr_a, const char *expr_b, uint32_t a, uint32_t b);
void test_fail_ll(test_ctx *t, const char *file, int line,
                  const char *expr_a, const char *expr_b, long long a, long long b);
/* Mensagem livre (ex.: valores de entrada de um laço) antes de uma falha. */
void test_note(test_ctx *t, const char *fmt, ...);

#define TEST(name)      static void test_##name(test_ctx *t_)
#define SUITE(name)     void suite_##name(test_ctx *t_)
#define RUN_TEST(name)  test_run(t_, #name, test_##name)

#define ASSERT_TRUE(cond)                                              \
    do {                                                               \
        if (!(cond)) {                                                 \
            test_fail(t_, __FILE__, __LINE__, #cond);                  \
            return;                                                    \
        }                                                              \
    } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ_U32(a, b)                                            \
    do {                                                               \
        uint32_t test_a_ = (uint32_t)(a), test_b_ = (uint32_t)(b);     \
        if (test_a_ != test_b_) {                                      \
            test_fail_u32(t_, __FILE__, __LINE__, #a, #b, test_a_, test_b_); \
            return;                                                    \
        }                                                              \
    } while (0)

#define ASSERT_EQ(a, b)                                                \
    do {                                                               \
        long long test_a_ = (long long)(a), test_b_ = (long long)(b);  \
        if (test_a_ != test_b_) {                                      \
            test_fail_ll(t_, __FILE__, __LINE__, #a, #b, test_a_, test_b_); \
            return;                                                    \
        }                                                              \
    } while (0)

#define ASSERT_MEM_EQ(a, b, n)                                         \
    ASSERT_TRUE(memcmp((a), (b), (n)) == 0)

/* Para helpers que retornam valor: só verifica se o teste já falhou. */
#define TEST_FAILED() (t_->current_failed != 0)

/* Lista única das suítes (um arquivo test_<nome>.c cada). */
#define KSVM_TEST_SUITES(X) \
    X(bytes) X(alu) X(bus) X(encode) X(exec_mov) X(exec_mem) X(exec_stack) \
    X(exec_arith) X(exec_logic) X(exec_branch) X(exec_sys) X(faults)       \
    X(loader) X(syscall)                                                    \
    X(conform_arith) X(conform_logic) X(conform_flow) X(conform_mem)        \
    X(zero_reg) X(fault_effects) X(programs) X(snapshot) X(review_core)

#define KSVM_TEST_DECLARE(name) SUITE(name);
KSVM_TEST_SUITES(KSVM_TEST_DECLARE)
#undef KSVM_TEST_DECLARE

#endif /* KSVM_TEST_H */
