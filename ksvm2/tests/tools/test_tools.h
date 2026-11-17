/*
 * test_tools.h — suítes dos testes das ferramentas (mesmo harness test.h dos
 * testes unitários, executável próprio: build/tools/test_disasm, que
 * também leva a política de caminho de src/hal/hal_fs.c).
 */
#ifndef KSVM_TEST_TOOLS_H
#define KSVM_TEST_TOOLS_H

#include "test.h"

#define KSVM_TOOLS_TEST_SUITES(X) X(disasm) X(hal_fs)

#define KSVM_TOOLS_TEST_DECLARE(name) SUITE(name);
KSVM_TOOLS_TEST_SUITES(KSVM_TOOLS_TEST_DECLARE)
#undef KSVM_TOOLS_TEST_DECLARE

#endif /* KSVM_TEST_TOOLS_H */
