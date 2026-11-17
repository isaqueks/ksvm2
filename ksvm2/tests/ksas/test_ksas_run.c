/*
 * test_ksas_run.c — programas montados em memória, carregados com
 * ksvm_load_image + hal_null e executados; e os exemplos de examples/.
 */
#include <stdlib.h>

#include "ksas_test.h"

#define REG(b, i) ((b)->vm.cpu.r[(i)])

TEST(sum_loop)
{
    vm_box *b = ta_run(t_, "        .global _start\n"
                           "_start: clr r1\n"
                           "        li  r2, 100\n"
                           "loop:   add r1, r1, r2\n"
                           "        dec r2\n"
                           "        bne loop\n"
                           "        halt\n");
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ(b->status, KSVM_HALTED);
    ASSERT_EQ_U32(b->vm.exit_code, 5050);
    ASSERT_EQ_U32(REG(b, 2), 0);
    ta_box_free(b);
}

TEST(arithmetic_and_long_forms)
{
    vm_box *b = ta_run(t_, "li   r1, 0x7FFFFFFF\n"
                           "addi r2, r1, 1\n"
                           "sub  r3, zr, r1\n"
                           "li   r4, -7\n"
                           "li   r5, 2\n"
                           "div  r6, r4, r5\n"
                           "rem  r7, r4, r5\n"
                           "sub  r8, r1, 100000\n"          /* ADDL -100000 */
                           "subi r9, r5, -9000\n"           /* ADDL 9000 */
                           "and  r10, r1, 0xFFFF0000\n"     /* ANDL */
                           "mov  r11, -1\n"                 /* MOVI -1 */
                           "mfsr r12, ISA_VERSION\n"
                           "halt\n");
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ(b->status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(b, 2), 0x80000000u);
    ASSERT_EQ_U32(REG(b, 3), 0x80000001u);
    ASSERT_EQ_U32(REG(b, 6), (uint32_t)-3);
    ASSERT_EQ_U32(REG(b, 7), (uint32_t)-1);
    ASSERT_EQ_U32(REG(b, 8), 0x7FFFFFFFu - 100000u);
    ASSERT_EQ_U32(REG(b, 9), 9002);
    ASSERT_EQ_U32(REG(b, 10), 0x7FFF0000u);
    ASSERT_EQ_U32(REG(b, 11), 0xFFFFFFFFu);
    ASSERT_EQ_U32(REG(b, 12), KSVM_ISA_VERSION);
    ta_box_free(b);
}

TEST(memory_data_bss_rodata)
{
    vm_box *b = ta_run(t_, "        la   r1, table\n"
                           "        ldw  r2, [r1 + 4]\n"
                           "        ldh  r3, [r1 + 8]\n"
                           "        ldhs r4, [r1 + 8]\n"
                           "        ldb  r5, [r1 + 10]\n"
                           "        ldbs r6, [r1 + 10]\n"
                           "        la   r7, counter\n"
                           "        stw  [r7], r2\n"
                           "        ldw  r8, [r7]\n"
                           "        li   r9, 4\n"
                           "        ldw  r10, [r1 + r9]\n"
                           "        stb  [r7 + 4], r9\n"
                           "        ldb  r11, [r7 + 4]\n"
                           "        la   r12, ro\n"
                           "        ldw  r13, [r12]\n"
                           "        ldw  r14, [small]\n"     /* [abs] = [zr + abs] */
                           "        halt\n"
                           "        .rodata\n"
                           "ro:     .word 0xC0DE\n"
                           "        .data\n"
                           "table:  .word 10, 20\n"
                           "        .half -2\n"
                           "        .byte 0x80\n"
                           "        .bss\n"
                           "counter: .space 8\n"
                           "small = ro\n");
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ(b->status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(b, 2), 20);
    ASSERT_EQ_U32(REG(b, 3), 0xFFFE);
    ASSERT_EQ_U32(REG(b, 4), 0xFFFFFFFEu);
    ASSERT_EQ_U32(REG(b, 5), 0x80);
    ASSERT_EQ_U32(REG(b, 6), 0xFFFFFF80u);
    ASSERT_EQ_U32(REG(b, 8), 20);
    ASSERT_EQ_U32(REG(b, 10), 20);
    ASSERT_EQ_U32(REG(b, 11), 4);
    ASSERT_EQ_U32(REG(b, 13), 0xC0DE);
    ASSERT_EQ_U32(REG(b, 14), 0xC0DE);
    ta_box_free(b);
}

TEST(calls_stack_and_frames)
{
    vm_box *b = ta_run(t_, "_start: li   r1, 12\n"
                           "        mov  r20, sp\n"
                           "        call square\n"
                           "        mov  r16, r1\n"
                           "        li   r17, 7\n"
                           "        push r16, r17\n"
                           "        pop  r18, r19\n"          /* r18 = 7, r19 = 144 */
                           "        mov  r21, sp\n"
                           "        la   r5, square\n"
                           "        li   r1, 3\n"
                           "        callr r5\n"
                           "        halt\n"
                           "square: push fp\n"
                           "        mov  fp, sp\n"
                           "        mul  r1, r1, r1\n"
                           "        mov  sp, fp\n"
                           "        pop  fp\n"
                           "        ret\n");
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ(b->status, KSVM_HALTED);
    ASSERT_EQ_U32(REG(b, 16), 144);
    ASSERT_EQ_U32(REG(b, 18), 7);
    ASSERT_EQ_U32(REG(b, 19), 144);
    ASSERT_EQ_U32(REG(b, 21), REG(b, 20));     /* pilha equilibrada */
    ASSERT_EQ_U32(REG(b, 1), 9);
    ta_box_free(b);
}

TEST(hello_with_puts)
{
    vm_box *b = ta_run(t_, "        la   r1, msg\n"
                           "        li   r2, len\n"
                           "        syscall PUTS\n"
                           "        li   r1, '!'\n"
                           "        syscall PUTC\n"
                           "        li   r1, 3\n"
                           "        syscall EXIT\n"
                           "        .data\n"
                           "msg:    .ascii \"oi, ksvm\"\n"
                           "len = . - msg\n");
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ(b->status, KSVM_HALTED);
    ASSERT_EQ_U32(b->vm.exit_code, 3);
    ASSERT_TRUE(ta_output_is(t_, b, "oi, ksvm!"));
    ta_box_free(b);
}

TEST(pic_program_runs)
{
    /* Com .pic, "la" vira ADRL; o resultado na VM é o mesmo endereço. */
    vm_box *b = ta_run(t_, "        .pic\n"
                           "        la   r1, msg\n"
                           "        la   r3, msg\n"
                           "        li   r2, 2\n"
                           "        syscall PUTS\n"
                           "        halt\n"
                           "        .rodata\n"
                           "msg:    .ascii \"ok\"\n");
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ(b->status, KSVM_HALTED);
    ASSERT_TRUE((b->out.hdr.flags & KSVM_KSX_FLAG_PIC) != 0);
    ASSERT_EQ_U32(ta_code32(&b->out, 0), ksvm_enc_l(KSVM_OP_ADRL, 1, 0));
    ASSERT_TRUE(ta_output_is(t_, b, "ok"));
    ta_box_free(b);
}

TEST(signed_and_unsigned_branches)
{
    vm_box *b = ta_run(t_, "        clr  r10\n"
                           "        li   r1, -1\n"
                           "        cmp  r1, 1\n"
                           "        bge  bad\n"           /* -1 < 1 com sinal */
                           "        blo  bad\n"           /* 0xFFFFFFFF > 1 sem sinal */
                           "        b.lt ok1\n"
                           "        b    bad\n"
                           "ok1:    bhi  ok2\n"
                           "        b    bad\n"
                           "ok2:    li   r10, 1\n"
                           "bad:    halt\n");
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ_U32(REG(b, 10), 1);
    ta_box_free(b);
}

TEST(trap_faults_with_code)
{
    vm_box *b = ta_run(t_, "nop\ntrap 77\n");

    ASSERT_TRUE(b != NULL);
    ASSERT_EQ(b->status, KSVM_FAULT);
    ASSERT_EQ(b->vm.fault, KSVM_FAULT_TRAP);
    ASSERT_EQ_U32(b->vm.fault_addr, 77);
    ASSERT_EQ_U32(b->vm.cpu.pc, 4);
    ta_box_free(b);
}

TEST(entry_point_respected)
{
    vm_box *b = ta_run(t_, "        li r1, 1\n"
                           "        halt\n"
                           "main:   li r1, 2\n"
                           "        halt\n"
                           "        .entry main\n");
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ_U32(b->vm.exit_code, 2);
    ta_box_free(b);
}

TEST(stack_directive_sets_limit)
{
    /* .stack 64: 17 PUSHs (68 bytes) estouram stack_limit -> FAULT_STACK. */
    vm_box *b = ta_run(t_, "        .stack 64\n"
                           "        li r2, 17\n"
                           "loop:   push r1\n"
                           "        dec r2\n"
                           "        bne loop\n"
                           "        halt\n");
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ(b->status, KSVM_FAULT);
    ASSERT_EQ(b->vm.fault, KSVM_FAULT_STACK);
    ta_box_free(b);
}

/* ---- Exemplos do diretório examples/ ---------------------------------- */

typedef struct example {
    const char *path;
    const char *input;
    const char *output;
} example;

static const example examples[] = {
    { "examples/hello.ksasm", NULL, "Hello, KSVM!\n" },
    { "examples/fib.ksasm", NULL,
      "0 1 1 2 3 5 8 13 21 34 55 89 144 233 377 610 987 1597 2584 4181\n" },
    { "examples/strlen.ksasm", NULL,
      "\"KSVM\" tem 4 bytes\n\"\" tem 0 bytes\n\"assembler\" tem 9 bytes\n" },
    { "examples/sieve.ksasm", NULL,
      "2 3 5 7 11 13 17 19 23 29 31 37 41 43 47 53 59 61 67 71 73 79 83 89 97\n" },
    { "examples/recursion.ksasm", NULL, "10! = 3628800\nfib(20) = 6765\n" },
    { "examples/bitops.ksasm", NULL,
      "popcnt(0xF0F0F0F0) = 16\n"
      "clz(0x00010000) = 15\n"
      "ctz(0x00010000) = 16\n"
      "rol(0x80000001, 4) = 0x00000018\n"
      "ror(0x80000001, 4) = 0x18000000\n"
      "sar(0x80000000, 4) = 0xF8000000\n"
      "shr(0x80000000, 4) = 0x08000000\n"
      "0x12345678 & 0xFFFF = 0x00005678\n"
      "0x12345678 ^ -1 = 0xEDCBA987\n" },
    { "examples/jumptable.ksasm", NULL,
      "0: zero\n1: um\n2: dois\n3: tres\n4: outro\n5: outro\n" },
    { "examples/v1_ported/ask_name.ksasm", "Isaque\n",
      "Type your name: Your name is Isaque\n" },
    { "examples/v1_ported/show_string.ksasm", NULL, "This is a string message!\n" },
    { "examples/v1_ported/jg_test.ksasm", NULL,
      "18 to 20\nGreater\n22 to 20\nGreater\n20 to 20\nGreater or equal\n" },
    { "examples/v1_ported/stack_test.ksasm", NULL, "Hello, World!\n" }
};

static bool run_example(test_ctx *t_, const example *ex)
{
    ksas_options opt;
    char *src = ta_read_file(ex->path);
    vm_box *b;
    bool ok;

    if (src == NULL) {
        test_note(t_, "nao foi possivel ler %s (rode a partir da raiz do projeto)", ex->path);
        return false;
    }
    memset(&opt, 0, sizeof opt);
    opt.filename = ex->path;
    opt.include = ta_include_file;
    b = ta_run_ex(t_, src, &opt, ex->input);
    free(src);
    if (b == NULL) {
        test_note(t_, "%s nao montou/carregou", ex->path);
        return false;
    }
    ok = b->status == KSVM_HALTED && b->vm.exit_code == 0u && ta_output_is(t_, b, ex->output);
    if (!ok) {
        test_note(t_, "%s: status %d, exit %lu, fault %d", ex->path, (int)b->status,
                  (unsigned long)b->vm.exit_code, (int)b->vm.fault);
    }
    ta_box_free(b);
    return ok;
}

TEST(examples_produce_expected_output)
{
    size_t i;

    for (i = 0; i < sizeof examples / sizeof examples[0]; i++) {
        ASSERT_TRUE(run_example(t_, &examples[i]));
    }
}

SUITE(ksas_run)
{
    RUN_TEST(sum_loop);
    RUN_TEST(arithmetic_and_long_forms);
    RUN_TEST(memory_data_bss_rodata);
    RUN_TEST(calls_stack_and_frames);
    RUN_TEST(hello_with_puts);
    RUN_TEST(pic_program_runs);
    RUN_TEST(signed_and_unsigned_branches);
    RUN_TEST(trap_faults_with_code);
    RUN_TEST(entry_point_respected);
    RUN_TEST(stack_directive_sets_limit);
    RUN_TEST(examples_produce_expected_output);
}
