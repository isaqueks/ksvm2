/*
 * ksas_test.h — auxiliares dos testes do assembler (tests/ksas).
 *
 * Usa o harness de tests/unit/test.h. Cada arquivo test_ksas_*.c define uma
 * suíte listada em KSAS_TEST_SUITES; test_ksas.c tem o main e os
 * auxiliares: montar fonte em memória, ler a imagem gerada, conferir o
 * primeiro diagnóstico e rodar o programa na VM com hal_null.
 */
#ifndef KSAS_TEST_H
#define KSAS_TEST_H

#include "ksas.h"
#include "ksvm/hal_null.h"
#include "ksvm/ksvm.h"
#include "test.h"

#define KSAS_TEST_SUITES(X) \
    X(ksas_lexer) X(ksas_expr) X(ksas_encode) X(ksas_pseudo) \
    X(ksas_directives) X(ksas_errors) X(ksas_run) X(ksas_review)

#define KSAS_TEST_DECLARE(name) SUITE(name);
KSAS_TEST_SUITES(KSAS_TEST_DECLARE)
#undef KSAS_TEST_DECLARE

/* ---- Montagem --------------------------------------------------------- */

typedef struct asm_out {
    ksas_result       res;
    int               rc;
    unsigned          ndiag;
    char              diags[4096];     /* todos os diagnósticos, um por linha */
    uint32_t          first_line;
    uint32_t          first_col;
    char              first_msg[256];
    char             *listing;         /* NULL se não pedido */
    size_t            listing_len;
    ksvm_image_header hdr;             /* válido se rc == 0 */
} asm_out;

/* Monta src. extra (pode ser NULL) fornece defines, -g e include; diag e
 * listing são instalados aqui. want_listing liga a captura do listing. */
int ta_assemble_ex(asm_out *o, const char *src, const ksas_options *extra, bool want_listing);
int ta_assemble(asm_out *o, const char *src);
void ta_free(asm_out *o);

/* Palavra de 32 bits em CODE (endereço do guest) / byte de .data. */
uint32_t ta_code32(const asm_out *o, uint32_t addr);
uint8_t  ta_code8(const asm_out *o, uint32_t addr);
uint8_t  ta_data8(const asm_out *o, uint32_t off);

/* Monta esperando erro; confere linha, coluna (0 = não conferir) e trecho
 * da mensagem do PRIMEIRO diagnóstico. */
bool ta_expect_error(test_ctx *t_, const char *src, uint32_t line, uint32_t col,
                     const char *substr);

/* Monta e devolve o valor de "expr" (via .word em .data). */
bool ta_eval(test_ctx *t_, const char *expr, uint32_t *value);

/* Callback de .include que lê do disco, relativo ao arquivo que inclui. */
char *ta_include_file(void *ctx, const char *from, const char *path, const char **err);
char *ta_read_file(const char *path);

/* ---- Execução --------------------------------------------------------- */

#define TA_RAM_SIZE   65536u
#define TA_MAX_INSNS  20000000u

typedef struct vm_box {
    ksvm          vm;
    ksvm_hal      hal;
    ksvm_hal_null hn;
    asm_out       out;
    ksvm_status   status;
    uint8_t       ram[TA_RAM_SIZE];
} vm_box;

/* Monta (com extra, pode ser NULL), carrega com ksvm_load_image + hal_null,
 * define a entrada (input pode ser NULL) e roda até parar. NULL em falha
 * de montagem/carga (com nota). Libere com ta_box_free. */
vm_box *ta_run_ex(test_ctx *t_, const char *src, const ksas_options *extra, const char *input);
vm_box *ta_run(test_ctx *t_, const char *src);
void ta_box_free(vm_box *b);

/* Saída capturada pelo hal_null é igual a "expected"? (nota se não for) */
bool ta_output_is(test_ctx *t_, const vm_box *b, const char *expected);

/* Atalhos para os testes. */
#define ASM_OK(o, src)                                                   \
    do {                                                                 \
        if (ta_assemble((o), (src)) != 0) {                              \
            test_note(t_, "diagnosticos:\n%s", (o)->diags);              \
            test_fail(t_, __FILE__, __LINE__, "montagem falhou");        \
            ta_free(o);                                                  \
            return;                                                      \
        }                                                                \
    } while (0)

#define ASM_ERR(src, line, col, substr) \
    ASSERT_TRUE(ta_expect_error(t_, (src), (line), (col), (substr)))

#endif /* KSAS_TEST_H */
