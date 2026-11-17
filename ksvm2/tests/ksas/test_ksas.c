/*
 * test_ksas.c — main dos testes do assembler e auxiliares (ksas_test.h).
 *
 * O harness (test_run, test_fail...) é o mesmo contrato de tests/unit,
 * reimplementado aqui porque tests/unit/test_main.c roda as suítes do
 * núcleo, que este binário não inclui.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ksas_test.h"

/* ---- Harness ---------------------------------------------------------- */

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

/* ---- Montagem --------------------------------------------------------- */

static void collect_diag(void *ctx, const ksas_diag *d)
{
    asm_out *o = ctx;
    size_t used = strlen(o->diags);

    if (o->ndiag == 0u) {
        o->first_line = d->line;
        o->first_col = d->col;
        snprintf(o->first_msg, sizeof o->first_msg, "%s", d->message);
    }
    o->ndiag++;
    snprintf(o->diags + used, sizeof o->diags - used, "%s\n", d->text);
}

static void collect_listing(void *ctx, const char *line)
{
    asm_out *o = ctx;
    size_t n = strlen(line);
    char *nb = realloc(o->listing, o->listing_len + n + 1u);

    if (nb == NULL) {
        return;
    }
    o->listing = nb;
    memcpy(o->listing + o->listing_len, line, n + 1u);
    o->listing_len += n;
}

int ta_assemble_ex(asm_out *o, const char *src, const ksas_options *extra, bool want_listing)
{
    ksas_options opt;

    memset(o, 0, sizeof *o);
    memset(&opt, 0, sizeof opt);
    if (extra != NULL) {
        opt = *extra;
    }
    if (opt.filename == NULL) {
        opt.filename = "t.ksasm";
    }
    opt.diag = collect_diag;
    opt.diag_ctx = o;
    if (want_listing) {
        opt.listing = collect_listing;
        opt.listing_ctx = o;
    }
    o->rc = ksas_assemble(src, &opt, &o->res);
    if (o->rc == 0 &&
        ksvm_image_parse(o->res.image, o->res.image_size, NULL, &o->hdr) != KSVM_OK) {
        snprintf(o->diags, sizeof o->diags, "imagem gerada invalida\n");
        o->rc = -2;
    }
    return o->rc;
}

int ta_assemble(asm_out *o, const char *src)
{
    return ta_assemble_ex(o, src, NULL, false);
}

void ta_free(asm_out *o)
{
    ksas_result_free(&o->res);
    free(o->listing);
    o->listing = NULL;
}

uint32_t ta_code32(const asm_out *o, uint32_t addr)
{
    if (o->res.image == NULL || addr > o->hdr.code_size || o->hdr.code_size - addr < 4u) {
        return UINT32_C(0xDEADBEEF);
    }
    return ksvm_ld32le(o->res.image + o->hdr.code_off + addr);
}

uint8_t ta_code8(const asm_out *o, uint32_t addr)
{
    if (o->res.image == NULL || addr >= o->hdr.code_size) {
        return 0xEE;
    }
    return o->res.image[o->hdr.code_off + addr];
}

uint8_t ta_data8(const asm_out *o, uint32_t off)
{
    if (o->res.image == NULL || off >= o->hdr.data_size) {
        return 0xEE;
    }
    return o->res.image[o->hdr.data_off + off];
}

bool ta_expect_error(test_ctx *t_, const char *src, uint32_t line, uint32_t col,
                     const char *substr)
{
    asm_out o;
    bool ok;

    ta_assemble(&o, src);
    ok = o.rc != 0 && o.ndiag > 0u && o.first_line == line &&
         (col == 0u || o.first_col == col) && strstr(o.first_msg, substr) != NULL;
    if (!ok) {
        test_note(t_, "esperado erro %lu:%lu contendo \"%s\"; obtido rc=%d:\n%s",
                  (unsigned long)line, (unsigned long)col, substr, o.rc, o.diags);
    }
    ta_free(&o);
    return ok;
}

bool ta_eval(test_ctx *t_, const char *expr, uint32_t *value)
{
    char src[1024];
    asm_out o;

    snprintf(src, sizeof src, "halt\n.data\n.word %s\n", expr);
    if (ta_assemble(&o, src) != 0) {
        test_note(t_, "expressao \"%s\" nao montou:\n%s", expr, o.diags);
        ta_free(&o);
        return false;
    }
    *value = ksvm_ld32le(o.res.image + o.hdr.data_off);
    ta_free(&o);
    return true;
}

char *ta_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL, *nb;
    size_t len = 0, n;

    if (f == NULL) {
        return NULL;
    }
    do {
        nb = realloc(buf, len + 4097u);
        if (nb == NULL) {
            free(buf);
            fclose(f);
            return NULL;
        }
        buf = nb;
        n = fread(buf + len, 1u, 4096u, f);
        len += n;
    } while (n != 0u);
    fclose(f);
    buf[len] = '\0';
    return buf;
}

char *ta_include_file(void *ctx, const char *from, const char *path, const char **err)
{
    const char *slash = strrchr(from, '/');
    char full[512];
    char *text;

    (void)ctx;
    if (slash != NULL && path[0] != '/') {
        snprintf(full, sizeof full, "%.*s/%s", (int)(slash - from), from, path);
    } else {
        snprintf(full, sizeof full, "%s", path);
    }
    text = ta_read_file(full);
    if (text == NULL) {
        *err = "arquivo nao encontrado";
    }
    return text;
}

/* ---- Execução --------------------------------------------------------- */

vm_box *ta_run_ex(test_ctx *t_, const char *src, const ksas_options *extra, const char *input)
{
    vm_box *b = calloc(1u, sizeof *b);
    int err;

    if (b == NULL) {
        return NULL;
    }
    if (ta_assemble_ex(&b->out, src, extra, false) != 0) {
        test_note(t_, "montagem falhou:\n%s", b->out.diags);
        ta_box_free(b);
        return NULL;
    }
    ksvm_init(&b->vm, &b->hal);
    ksvm_hal_null_init(&b->hn, &b->hal, &b->vm);
    if (input != NULL) {
        ksvm_hal_null_set_input(&b->hn, (const uint8_t *)input, (uint32_t)strlen(input), true);
    }
    err = ksvm_load_image(&b->vm, b->out.res.image, b->out.res.image_size, b->ram, TA_RAM_SIZE);
    if (err != KSVM_OK) {
        test_note(t_, "ksvm_load_image devolveu %d", err);
        ta_box_free(b);
        return NULL;
    }
    b->status = ksvm_run(&b->vm, TA_MAX_INSNS);
    return b;
}

vm_box *ta_run(test_ctx *t_, const char *src)
{
    return ta_run_ex(t_, src, NULL, NULL);
}

void ta_box_free(vm_box *b)
{
    if (b != NULL) {
        ta_free(&b->out);
        free(b);
    }
}

bool ta_output_is(test_ctx *t_, const vm_box *b, const char *expected)
{
    size_t n = strlen(expected);

    if (b->hn.out_total == n && memcmp(b->hn.out, expected, n) == 0) {
        return true;
    }
    test_note(t_, "saida (%lu bytes): \"%.*s\"\n  esperada: \"%s\"", (unsigned long)b->hn.out_total,
              (int)b->hn.out_len, (const char *)b->hn.out, expected);
    return false;
}

/* ---- main ------------------------------------------------------------- */

int main(void)
{
    test_ctx t;
    unsigned before_run, before_failed;

    memset(&t, 0, sizeof t);
    printf("ksas: testes do assembler\n");
#define KSAS_TEST_RUN_SUITE(name)                                         \
    before_run = t.tests_run;                                             \
    before_failed = t.tests_failed;                                       \
    t.suite = #name;                                                      \
    suite_##name(&t);                                                     \
    printf("  %-16s %3u testes, %u falhas\n", #name,                      \
           t.tests_run - before_run, t.tests_failed - before_failed);
    KSAS_TEST_SUITES(KSAS_TEST_RUN_SUITE)
#undef KSAS_TEST_RUN_SUITE
    printf("TOTAL: %u testes, %u falhas\n", t.tests_run, t.tests_failed);
    return (t.tests_failed > 125u) ? 125 : (int)t.tests_failed;
}
