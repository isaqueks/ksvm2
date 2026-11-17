/*
 * test_main.c — executa todas as suítes e devolve o número de testes que
 * falharam (limitado a 125 para não colidir com códigos especiais do shell).
 */
#include <stdarg.h>
#include <stdio.h>

#include "ksvm/config.h"
#include "test.h"

void test_run(test_ctx *t, const char *name, test_fn fn)
{
    t->test = name;
    t->current_failed = 0;
    fn(t);
    t->tests_run++;
    if (t->current_failed) {
        t->tests_failed++;
    }
}

static void fail_header(test_ctx *t, const char *file, int line)
{
    printf("FALHA %s.%s (%s:%d): ", t->suite, t->test, file, line);
    t->current_failed = 1;
}

void test_fail(test_ctx *t, const char *file, int line, const char *msg)
{
    fail_header(t, file, line);
    printf("%s\n", msg);
}

void test_fail_u32(test_ctx *t, const char *file, int line,
                   const char *expr_a, const char *expr_b, uint32_t a, uint32_t b)
{
    fail_header(t, file, line);
    printf("%s == 0x%08lX, esperado %s == 0x%08lX\n",
           expr_a, (unsigned long)a, expr_b, (unsigned long)b);
}

void test_fail_ll(test_ctx *t, const char *file, int line,
                  const char *expr_a, const char *expr_b, long long a, long long b)
{
    fail_header(t, file, line);
    printf("%s == %lld, esperado %s == %lld\n", expr_a, a, expr_b, b);
}

void test_note(test_ctx *t, const char *fmt, ...)
{
    va_list ap;

    printf("  nota %s.%s: ", t->suite, t->test);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

int main(void)
{
    test_ctx t;
    unsigned before_run, before_failed;

    memset(&t, 0, sizeof t);
    printf("KSVM testes unitarios (TRACE=%d NO_INT64=%d MAX_REGIONS=%d MULDIV=%d BITOPS=%d "
           "ISA_COMPACT=%d ISA_NAMES=%d FAULT_NAMES=%d)\n",
           KSVM_ENABLE_TRACE, KSVM_NO_INT64, KSVM_MAX_REGIONS, KSVM_ENABLE_MULDIV,
           KSVM_ENABLE_BITOPS, KSVM_ISA_COMPACT, KSVM_ISA_NAMES, KSVM_FAULT_NAMES);
#define KSVM_TEST_RUN_SUITE(name)                                         \
    before_run = t.tests_run;                                             \
    before_failed = t.tests_failed;                                       \
    t.suite = #name;                                                      \
    suite_##name(&t);                                                     \
    printf("  %-12s %3u testes, %u falhas\n", #name,                      \
           t.tests_run - before_run, t.tests_failed - before_failed);
    KSVM_TEST_SUITES(KSVM_TEST_RUN_SUITE)
#undef KSVM_TEST_RUN_SUITE
    printf("TOTAL: %u testes, %u falhas\n", t.tests_run, t.tests_failed);
    return (t.tests_failed > 125u) ? 125 : (int)t.tests_failed;
}
