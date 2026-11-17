/*
 * test_ksas_expr.c — precedência, aritmética de 32 bits, labels, ".",
 * constantes (.equ, =, -D) e erros de avaliação.
 */
#include "ksas_test.h"

typedef struct expr_case {
    const char *expr;
    uint32_t    value;
} expr_case;

static bool check_cases(test_ctx *t_, const expr_case *cases, size_t n)
{
    size_t i;
    uint32_t v;

    for (i = 0; i < n; i++) {
        if (!ta_eval(t_, cases[i].expr, &v)) {
            return false;
        }
        if (v != cases[i].value) {
            test_note(t_, "\"%s\" = 0x%08lX, esperado 0x%08lX", cases[i].expr,
                      (unsigned long)v, (unsigned long)cases[i].value);
            return false;
        }
    }
    return true;
}

TEST(precedence)
{
    static const expr_case cases[] = {
        { "1 + 2 * 3", 7 },        { "(1 + 2) * 3", 9 },      { "1 << 4 + 1", 32 },
        { "6 & 3 | 8", 10 },       { "1 | 2 ^ 3", 1 },        { "2 * 3 % 4", 2 },
        { "10 - 2 - 3", 5 },       { "100 / 10 / 2", 5 },     { "1 + 2 << 3", 24 },
        { "0xFF & 0x0F ^ 0x3", 0xC }, { "-2 * -3", 6 },       { "~0 & 0xF", 0xF },
        { "-(2 + 3) * 2", (uint32_t)-10 }, { "+5", 5 },       { "((((7))))", 7 }
    };
    ASSERT_TRUE(check_cases(t_, cases, sizeof cases / sizeof cases[0]));
}

TEST(arithmetic_wraps_in_32_bits)
{
    static const expr_case cases[] = {
        { "0xFFFFFFFF + 1", 0 },        { "0 - 1", 0xFFFFFFFFu },     { "-1", 0xFFFFFFFFu },
        { "0x10000 * 0x10000", 0 },     { "1 << 31", 0x80000000u },   { "1 << 32", 0 },
        { "0x80000000 >> 31", 1 },      { "-16 >> 28", 0xF },         { "5 >> 40", 0 },
        { "7 / 2", 3 },                 { "-7 / 2", (uint32_t)-3 },   { "-7 % 2", (uint32_t)-1 },
        { "7 % -2", 1 },                { "0x80000000 / -1", 0x80000000u },
        { "0x80000000 % -1", 0 },       { "~0x0F", 0xFFFFFFF0u }
    };
    ASSERT_TRUE(check_cases(t_, cases, sizeof cases / sizeof cases[0]));
}

TEST(literals_in_expressions)
{
    static const expr_case cases[] = {
        { "'a' + 1", 98 },  { "'\\n'", 10 },  { "0b1111_0000 | 0x0F", 0xFF },
        { "1_000 * 1_000", 1000000 },  { "'0' + 9", '9' }
    };
    ASSERT_TRUE(check_cases(t_, cases, sizeof cases / sizeof cases[0]));
}

TEST(label_differences)
{
    asm_out o;

    ASM_OK(&o, "start: nop\n"
               "mid:   nop\n"
               "       halt\n"
               "       .data\n"
               "       .word mid - start, end - start, after - before\n"
               "before: .byte 1, 2, 3\n"
               "after:\n"
               "       .text\n"
               "end:   halt\n");
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off), 4);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off + 4), 12);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off + 8), 3);
    ta_free(&o);
}

TEST(label_addresses_are_absolute)
{
    asm_out o;

    ASM_OK(&o, "a: nop\n"
               "b: halt\n"
               "   .data\n"
               "x: .word b, x, y + 2\n"
               "   .bss\n"
               "y: .space 4\n");
    /* code 8 -> ram_base 0x10; .data tem 12 bytes; .bss em 0x10 + 12 */
    ASSERT_EQ_U32(o.res.ram_base, 0x10);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off), 4);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off + 4), 0x10);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off + 8), 0x10 + 12 + 2);
    ta_free(&o);
}

TEST(dot_is_current_address)
{
    asm_out o;

    ASM_OK(&o, "    nop\n"
               "here: b .\n"                       /* laço infinito: desvio para si */
               "    halt\n"
               "    .data\n"
               "    .word 0\n"
               "pos = .\n"
               "    .word pos\n"
               "    .word . - pos\n");             /* "." = início do item */
    ASSERT_EQ_U32(ta_code32(&o, 4), ksvm_enc_b(KSVM_COND_AL, -4));
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off + 4), o.res.ram_base + 4);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off + 8), 4);
    ta_free(&o);
}

TEST(equ_forms_and_chains)
{
    asm_out o;

    ASM_OK(&o, "A = B + 1\n"
               "B = 2 * C\n"
               "    .equ C, 5\n"
               "    .equ D, A << 1\n"
               "    halt\n"
               "    .data\n"
               "    .word A, B, C, D\n");
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off), 11);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off + 4), 10);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off + 8), 5);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off + 12), 22);
    ta_free(&o);
}

TEST(command_line_defines)
{
    static const char *const defs[] = { "N=10", "FLAG", "M = 1 << 4", "HEX=0x_FF" };
    ksas_options opt;
    asm_out o;

    memset(&opt, 0, sizeof opt);
    opt.defines = defs;
    opt.num_defines = 4;
    ASSERT_EQ(ta_assemble_ex(&o, "halt\n.data\n.word N, FLAG, M, HEX\n", &opt, false), 0);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off), 10);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off + 4), 1);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off + 8), 16);
    ta_free(&o);
}

TEST(command_line_define_errors)
{
    static const char *const bad[] = { "1X=2" };
    static const char *const dup[] = { "X=1" };
    ksas_options opt;
    asm_out o;

    memset(&opt, 0, sizeof opt);
    opt.defines = bad;
    opt.num_defines = 1;
    ASSERT_TRUE(ta_assemble_ex(&o, "halt\n", &opt, false) != 0);
    ASSERT_TRUE(strstr(o.diags, "<linha de comando>:1:1: erro:") != NULL);
    ta_free(&o);

    /* Redefinição no fonte aponta para a linha do fonte e cita o -D. */
    opt.defines = dup;
    ASSERT_TRUE(ta_assemble_ex(&o, "halt\nX = 2\n", &opt, false) != 0);
    ASSERT_EQ(o.first_line, 2);
    ASSERT_TRUE(strstr(o.first_msg, "<linha de comando>") != NULL);
    ta_free(&o);
}

TEST(evaluation_errors)
{
    ASM_ERR("halt\n.data\n.word 1 / (2 - 2)\n", 3, 9, "divisao por zero");
    ASM_ERR("halt\n.data\n.word 5 % 0\n", 3, 9, "divisao por zero");
    ASM_ERR("li r1, nada + 1\nhalt\n", 1, 8, "'nada' nao definido");
    ASM_ERR("A = B\nB = A + 1\nhalt\n.data\n.word A\n", 1, 0, "circular");
    ASM_ERR("halt\n.data\n.word (1 + 2\n", 3, 13, "esperado ')'");
    ASM_ERR("halt\n.data\n.word 1 +\n", 3, 10, "esperado expressao");
    ASM_ERR("li r1, r2 + 1\n", 1, 8, "registrador 'r2' nao pode aparecer");
}

TEST(unused_constants_are_checked)
{
    /* Constante nunca usada ainda é avaliada no fim: erro acusado. */
    ASM_ERR("halt\nX = Y * 2\n", 2, 5, "'Y' nao definido");
}

SUITE(ksas_expr)
{
    RUN_TEST(precedence);
    RUN_TEST(arithmetic_wraps_in_32_bits);
    RUN_TEST(literals_in_expressions);
    RUN_TEST(label_differences);
    RUN_TEST(label_addresses_are_absolute);
    RUN_TEST(dot_is_current_address);
    RUN_TEST(equ_forms_and_chains);
    RUN_TEST(command_line_defines);
    RUN_TEST(command_line_define_errors);
    RUN_TEST(evaluation_errors);
    RUN_TEST(unused_constants_are_checked);
}
