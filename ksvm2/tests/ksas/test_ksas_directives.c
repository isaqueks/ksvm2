/*
 * test_ksas_directives.c — seções, dados, alinhamento, .org, .entry,
 * .stack, .global/-g, .include, labels e listing.
 */
#include <stdlib.h>

#include "ksas_test.h"

static uint32_t data32(const asm_out *o, uint32_t off)
{
    return ksvm_ld32le(o->res.image + o->hdr.data_off + off);
}

TEST(section_layout)
{
    asm_out o;

    /* .text 12 bytes; .rodata 5 bytes logo depois (alinhado a 4);
     * CODE = 17 -> ram_base = 32; .data 3 bytes -> 4; .bss 10. */
    ASM_OK(&o, "        .text\n"
               "start:  nop\n"
               "        .rodata\n"
               "ro:     .ascii \"abcde\"\n"
               "        .data\n"
               "d:      .byte 1, 2, 3\n"
               "        .bss\n"
               "b:      .space 10\n"
               "        .text\n"
               "        nop\n"
               "        halt\n"
               "        .data\n"
               "        .word ro, d, b, start\n");
    ASSERT_EQ(o.hdr.code_size, 17);
    ASSERT_EQ_U32(o.hdr.ram_base, 32);
    ASSERT_EQ(o.hdr.data_size, 20);         /* 3 + padding até 4 + 4 words = 19 -> 20 */
    ASSERT_EQ(o.hdr.bss_size, 10);
    ASSERT_EQ(ta_code8(&o, 12), 'a');       /* .rodata depois de TODO o .text */
    ASSERT_EQ_U32(data32(&o, 3), 12);       /* sem alinhamento automático em .word */
    ASSERT_EQ_U32(data32(&o, 7), 32);
    ASSERT_EQ_U32(data32(&o, 11), 32 + 20);
    ASSERT_EQ_U32(data32(&o, 15), 0);
    ta_free(&o);
}

TEST(data_directives_little_endian)
{
    asm_out o;

    ASM_OK(&o, "halt\n"
               ".data\n"
               ".word 0x11223344, -1\n"
               ".half 0xBEEF, -2\n"
               ".byte 'A', 0x42, 67, -1, 255\n");
    ASSERT_EQ(ta_data8(&o, 0), 0x44);
    ASSERT_EQ(ta_data8(&o, 3), 0x11);
    ASSERT_EQ_U32(data32(&o, 4), 0xFFFFFFFFu);
    ASSERT_EQ(ta_data8(&o, 8), 0xEF);
    ASSERT_EQ(ta_data8(&o, 9), 0xBE);
    ASSERT_EQ(ta_data8(&o, 10), 0xFE);
    ASSERT_EQ(ta_data8(&o, 11), 0xFF);
    ASSERT_EQ(ta_data8(&o, 12), 'A');
    ASSERT_EQ(ta_data8(&o, 13), 'B');
    ASSERT_EQ(ta_data8(&o, 14), 'C');
    ASSERT_EQ(ta_data8(&o, 15), 0xFF);
    ASSERT_EQ(ta_data8(&o, 16), 0xFF);
    ta_free(&o);
}

TEST(strings)
{
    asm_out o;

    ASM_OK(&o, "halt\n"
               ".data\n"
               ".ascii \"ab\", \"c\"\n"
               ".asciz \"\\t\\x41\"\n"
               ".byte \"xy\", 0, 'z'\n"
               ".ascii \"\"\n");
    ASSERT_EQ(ta_data8(&o, 0), 'a');
    ASSERT_EQ(ta_data8(&o, 2), 'c');
    ASSERT_EQ(ta_data8(&o, 3), '\t');
    ASSERT_EQ(ta_data8(&o, 4), 'A');
    ASSERT_EQ(ta_data8(&o, 5), 0);
    ASSERT_EQ(ta_data8(&o, 6), 'x');
    ASSERT_EQ(ta_data8(&o, 7), 'y');
    ASSERT_EQ(ta_data8(&o, 8), 0);
    ASSERT_EQ(ta_data8(&o, 9), 'z');
    ta_free(&o);
}

TEST(space_and_fill)
{
    asm_out o;

    ASM_OK(&o, "halt\n"
               ".data\n"
               ".space 3, 0xAA\n"
               ".space 2\n"
               "N = 4\n"
               ".space N - 1, 'q'\n"
               ".bss\n"
               ".space 1000\n");
    ASSERT_EQ(ta_data8(&o, 0), 0xAA);
    ASSERT_EQ(ta_data8(&o, 2), 0xAA);
    ASSERT_EQ(ta_data8(&o, 3), 0);
    ASSERT_EQ(ta_data8(&o, 4), 0);
    ASSERT_EQ(ta_data8(&o, 5), 'q');
    ASSERT_EQ(ta_data8(&o, 7), 'q');
    ASSERT_EQ(o.hdr.data_size, 8);
    ASSERT_EQ(o.hdr.bss_size, 1000);
    ta_free(&o);
}

TEST(align_and_org)
{
    asm_out o;

    ASM_OK(&o, "        nop\n"
               "        .align 16\n"            /* NOPs (zeros) até 16 */
               "a16:    halt\n"
               "        .org 0x40\n"
               "o40:    halt\n"
               "        .data\n"
               "        .byte 1\n"
               "        .align 8\n"
               "d8:     .byte 2\n"
               "        .org 32\n"
               "d32:    .byte 3\n"
               "        .text\n"
               "        la r1, d8\n"
               "        la r2, d32\n");
    ASSERT_EQ_U32(ta_code32(&o, 4), 0);
    ASSERT_EQ_U32(ta_code32(&o, 16), ksvm_enc_r(KSVM_OP_HALT, 0, 0, 0));
    ASSERT_EQ_U32(ta_code32(&o, 0x40), ksvm_enc_r(KSVM_OP_HALT, 0, 0, 0));
    ASSERT_EQ(ta_data8(&o, 8), 2);
    ASSERT_EQ(ta_data8(&o, 32), 3);
    ASSERT_EQ_U32(ta_code32(&o, 0x48), o.res.ram_base + 8);
    ASSERT_EQ_U32(ta_code32(&o, 0x50), o.res.ram_base + 32);
    ta_free(&o);
}

TEST(rodata_and_ram_alignment_follow_align)
{
    asm_out o;

    ASM_OK(&o, "halt\n"
               ".rodata\n"
               ".align 64\n"
               "t: .byte 1\n"
               ".bss\n"
               ".align 256\n"
               "big: .space 4\n"
               ".text\n"
               "la r1, t\n"
               "la r2, big\n");
    ASSERT_EQ_U32(ta_code32(&o, 8) % 64u, 0);
    ASSERT_EQ_U32(ta_code32(&o, 16) % 256u, 0);
    ta_free(&o);
}

TEST(entry_and_stack)
{
    asm_out o;

    ASM_OK(&o, "nop\n_start: halt\n");
    ASSERT_EQ_U32(o.hdr.entry, 4);
    ASSERT_EQ_U32(o.hdr.stack_size, 256);
    ta_free(&o);

    ASM_OK(&o, "nop\nmain: nop\n_start: halt\n.entry main\n.stack 1024\n");
    ASSERT_EQ_U32(o.hdr.entry, 4);
    ASSERT_EQ_U32(o.hdr.stack_size, 1024);
    ta_free(&o);

    ASM_OK(&o, "nop\nhalt\n");               /* sem _start nem .entry: 0 */
    ASSERT_EQ_U32(o.hdr.entry, 0);
    ta_free(&o);
}

/* Procura um registro da tabela de símbolos; devolve o kind ou -1. */
static int find_symbol(const asm_out *o, const char *name, uint32_t *addr)
{
    const uint8_t *p = o->res.image + o->hdr.sym_off;
    const uint8_t *end = p + o->hdr.sym_size;
    size_t n = strlen(name);

    while (p + 6 <= end) {
        uint8_t len = p[5];
        if (len == n && memcmp(p + 6, name, n) == 0) {
            *addr = ksvm_ld32le(p);
            return p[4];
        }
        p += 6u + len;
    }
    return -1;
}

TEST(symbol_table)
{
    static const char *src =
        ".global _start, var, LIMIT\n"
        "_start: nop\n"
        "local:  halt\n"
        "LIMIT = 77\n"
        ".data\n"
        "var: .word 1\n"
        ".bss\n"
        "buf: .space 4\n";
    ksas_options opt;
    asm_out o;
    uint32_t addr = 0;

    ASM_OK(&o, src);                          /* sem -g: só os globais */
    ASSERT_TRUE((o.hdr.flags & KSVM_KSX_FLAG_SYMS) != 0);
    ASSERT_EQ(find_symbol(&o, "_start", &addr), KSAS_SYMK_CODE | KSAS_SYMK_GLOBAL);
    ASSERT_EQ_U32(addr, 0);
    ASSERT_EQ(find_symbol(&o, "var", &addr), KSAS_SYMK_DATA | KSAS_SYMK_GLOBAL);
    ASSERT_EQ_U32(addr, o.res.ram_base);
    ASSERT_EQ(find_symbol(&o, "LIMIT", &addr), KSAS_SYMK_ABS | KSAS_SYMK_GLOBAL);
    ASSERT_EQ_U32(addr, 77);
    ASSERT_EQ(find_symbol(&o, "local", &addr), -1);
    ta_free(&o);

    memset(&opt, 0, sizeof opt);
    opt.debug_symbols = true;                 /* -g: todos os labels */
    ASSERT_EQ(ta_assemble_ex(&o, src, &opt, false), 0);
    ASSERT_EQ(find_symbol(&o, "local", &addr), KSAS_SYMK_CODE);
    ASSERT_EQ_U32(addr, 4);
    ASSERT_EQ(find_symbol(&o, "buf", &addr), KSAS_SYMK_BSS);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.sym_off), 0);   /* ordenado por endereço */
    ta_free(&o);

    ASM_OK(&o, "halt\n");                     /* nada a exportar: sem seção */
    ASSERT_EQ(o.hdr.sym_size, 0);
    ASSERT_EQ(o.hdr.flags & KSVM_KSX_FLAG_SYMS, 0);
    ta_free(&o);
}

/* .include servido da memória: "lib.inc" e "nested.inc". */
static char *mem_include(void *ctx, const char *from, const char *path, const char **err)
{
    const char *text = NULL;
    char *copy;

    (void)ctx;
    (void)from;
    if (strcmp(path, "lib.inc") == 0) {
        text = "LIBVAL = 40\n.include \"nested.inc\"\nlibfn: ret\n";
    } else if (strcmp(path, "nested.inc") == 0) {
        text = "NESTED = 2\n";
    } else if (strcmp(path, "loop.inc") == 0) {
        text = ".include \"loop.inc\"\n";
    } else if (strcmp(path, "bad.inc") == 0) {
        text = "nop\n  foo\n";
    }
    if (text == NULL) {
        *err = "nao existe";
        return NULL;
    }
    copy = malloc(strlen(text) + 1u);
    if (copy != NULL) {
        strcpy(copy, text);
    }
    return copy;
}

TEST(include_directive)
{
    ksas_options opt;
    asm_out o;

    memset(&opt, 0, sizeof opt);
    opt.include = mem_include;
    ASSERT_EQ(ta_assemble_ex(&o, "call libfn\n.include \"lib.inc\"\nli r1, LIBVAL + NESTED\n",
                             &opt, false), 0);
    ASSERT_EQ_U32(ta_code32(&o, 0), ksvm_enc_j(KSVM_OP_CALL, 0));
    ASSERT_EQ_U32(ta_code32(&o, 8), ksvm_enc_i(KSVM_OP_MOVI, 1, 0, 42));
    ta_free(&o);

    /* Erro dentro do arquivo incluído aponta para ele. */
    ASSERT_TRUE(ta_assemble_ex(&o, "nop\n.include \"bad.inc\"\n", &opt, false) != 0);
    ASSERT_TRUE(strstr(o.diags, "bad.inc:2:3: erro: mnemonico desconhecido 'foo'") != NULL);
    ta_free(&o);

    ASSERT_TRUE(ta_assemble_ex(&o, "nop\n.include \"loop.inc\"\n", &opt, false) != 0);
    ASSERT_TRUE(strstr(o.first_msg, "aninhado demais") != NULL);
    ta_free(&o);

    ASSERT_TRUE(ta_assemble_ex(&o, "nop\n.include \"none.inc\"\n", &opt, false) != 0);
    ASSERT_EQ(o.first_line, 2);
    ASSERT_TRUE(strstr(o.first_msg, "none.inc") != NULL);
    ta_free(&o);

    ASM_ERR("nop\n.include \"lib.inc\"\n", 2, 10, ".include nao suportado");
}

TEST(labels_forward_backward_and_case)
{
    asm_out o;

    ASM_OK(&o, "Loop: nop\n"
               "loop: b Loop\n"
               "      b loop\n"
               "      b fwd\n"
               "a: b: c: halt\n"                      /* vários labels na linha */
               "fwd:  nop\n"
               "x.y:  b x.y\n");
    ASSERT_EQ_U32(ta_code32(&o, 4), ksvm_enc_b(KSVM_COND_AL, -8));
    ASSERT_EQ_U32(ta_code32(&o, 8), ksvm_enc_b(KSVM_COND_AL, -8));
    ASSERT_EQ_U32(ta_code32(&o, 12), ksvm_enc_b(KSVM_COND_AL, 4));
    ASSERT_EQ_U32(ta_code32(&o, 24), ksvm_enc_b(KSVM_COND_AL, -4));
    ta_free(&o);
}

TEST(comments_and_blank_lines)
{
    asm_out o;

    ASM_OK(&o, "\n; so comentario\n# outro\n// mais um\n   \n\tnop ; fim\nhalt # fim\n");
    ASSERT_EQ(o.hdr.code_size, 8);
    ta_free(&o);

    ASM_OK(&o, "nop\r\nhalt");                 /* CRLF e sem '\n' final */
    ASSERT_EQ(o.hdr.code_size, 8);
    ta_free(&o);
}

TEST(listing_output)
{
    asm_out o;

    ASSERT_EQ(ta_assemble_ex(&o, "start: li r1, 5\n"
                                 "       la r2, s\n"
                                 "       halt\n"
                                 "N = 7\n"
                                 "       .rodata\n"
                                 "s:     .asciz \"0123456789\"\n", NULL, true), 0);
    ASSERT_TRUE(o.listing != NULL);
    ASSERT_TRUE(strstr(o.listing, "00000000  02 01 14 00                  1  start: li r1, 5") != NULL);
    ASSERT_TRUE(strstr(o.listing, "00000004  03 02 00 00 10 00 00 00      2") != NULL);
    ASSERT_TRUE(strstr(o.listing, "= 0x00000007") != NULL);
    ASSERT_TRUE(strstr(o.listing, "00000010  30 31 32 33 34 35 36 37      6") != NULL);
    ASSERT_TRUE(strstr(o.listing, "00000018  38 39 00\n") != NULL);       /* continuação */
    ASSERT_TRUE(strstr(o.listing, "; simbolos") != NULL);
    ta_free(&o);
}

SUITE(ksas_directives)
{
    RUN_TEST(section_layout);
    RUN_TEST(data_directives_little_endian);
    RUN_TEST(strings);
    RUN_TEST(space_and_fill);
    RUN_TEST(align_and_org);
    RUN_TEST(rodata_and_ram_alignment_follow_align);
    RUN_TEST(entry_and_stack);
    RUN_TEST(symbol_table);
    RUN_TEST(include_directive);
    RUN_TEST(labels_forward_backward_and_case);
    RUN_TEST(comments_and_blank_lines);
    RUN_TEST(listing_output);
}
