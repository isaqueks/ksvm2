/*
 * test_ksas_errors.c — diagnósticos: formato "arquivo:linha:coluna: erro:",
 * erros de sintaxe, de alcance e semânticos, recuperação e limite de 20.
 */
#include <stdio.h>
#include <stdlib.h>

#include "ksas_test.h"

TEST(diagnostic_format)
{
    asm_out o;

    ASSERT_TRUE(ta_assemble(&o, "nop\n    frob r1\n") != 0);
    ASSERT_EQ(o.ndiag, 1);
    ASSERT_EQ(o.res.errors, 1);
    ASSERT_TRUE(o.res.image == NULL);
    ASSERT_TRUE(strcmp(o.diags, "t.ksasm:2:5: erro: mnemonico desconhecido 'frob'\n") == 0);
    ta_free(&o);
}

/* Erro no operando esquerdo de uma expressão binária: um diagnóstico só
 * (antes o lado direito era avaliado e repetia o mesmo erro). */
TEST(binary_expr_left_error_reported_once)
{
    asm_out o;

    ASSERT_TRUE(ta_assemble(&o, "li r1, Y + Y\nhalt\n") != 0);
    ASSERT_EQ(o.ndiag, 1);
    ASSERT_EQ(o.res.errors, 1);
    ASSERT_TRUE(strstr(o.diags, "t.ksasm:1:8: erro: simbolo 'Y' nao definido") != NULL);
    ta_free(&o);
    ASSERT_TRUE(ta_assemble(&o, ".data\n.word (1 / 0) * Z\n.text\nhalt\n") != 0);
    ASSERT_EQ(o.ndiag, 1);
    ta_free(&o);
}

TEST(syntax_errors)
{
    ASM_ERR("  foo r1\n", 1, 3, "mnemonico desconhecido 'foo'");
    ASM_ERR(".frob 1\n", 1, 1, "diretiva desconhecida '.frob'");
    ASM_ERR("add r1, r2\n", 1, 11, "esperado ','");
    ASM_ERR("add r1, 5, r2\n", 1, 9, "esperado registrador");
    ASM_ERR("mov r1\n", 1, 7, "'mov' espera dois operandos");
    ASM_ERR("halt r1\n", 1, 6, "texto inesperado");
    ASM_ERR("r1: nop\n", 1, 1, "registrador e nao pode ser label");
    ASM_ERR("123\n", 1, 1, "esperado label, instrucao ou diretiva");
    ASM_ERR("ldw r1, [r2\n", 1, 12, "esperado ']'");
    ASM_ERR("ldw r1, r2\n", 1, 9, "esperado '['");
    ASM_ERR("ldbx r1, [r2 + 4]\n", 1, 1, "'ldbx' exige endereco [rb + rc]");
    ASM_ERR("adc r1, r2, 5\n", 1, 13, "'adc' nao aceita imediato");
    ASM_ERR("neg r1, 3\n", 1, 9, "'neg' nao aceita imediato");
    ASM_ERR("bcond x\nx: nop\n", 1, 1, "use b<cond>");
    ASM_ERR("bxx x\n", 1, 1, "mnemonico desconhecido 'bxx'");
    ASM_ERR(".global 5\n", 1, 9, "esperado nome de simbolo");
    ASM_ERR(".equ X 5\nhalt\n", 1, 8, "esperado ','");
    ASM_ERR("halt\n.data\n.ascii 5\n", 3, 8, "esperado string");
    ASM_ERR("halt\n.data\n.half \"ab\"\n", 3, 7, "strings so sao aceitas em .byte");
    ASM_ERR("nop\n.entry 0\n.entry 0\n", 3, 1, ".entry repetido");
}

TEST(lexical_errors_have_position)
{
    ASM_ERR("li r1, 99999999999\n", 1, 8, "nao cabe em 32 bits");
    ASM_ERR("halt\n.data\n.asciz \"abc\n", 3, 8, "string sem aspas");
    ASM_ERR("halt\n.data\n.asciz \"a\\qb\"\n", 3, 10, "escape invalido");
    ASM_ERR("li r1, 3 $ 4\n", 1, 10, "caractere inesperado");
}

TEST(symbol_errors)
{
    ASM_ERR("a: nop\na: nop\n", 2, 1, "simbolo 'a' ja definido em t.ksasm:1");
    ASM_ERR("X = 1\nX = 2\nhalt\n", 2, 1, "ja definido");
    ASM_ERR("b nowhere\n", 1, 3, "'nowhere' nao definido");
    ASM_ERR(".global ghost\nhalt\n", 1, 9, "simbolo global 'ghost' nao definido");
}

TEST(section_errors)
{
    ASM_ERR("halt\n.data\n  nop\n", 3, 3, "instrucoes so podem aparecer em .text (secao atual: .data)");
    ASM_ERR("halt\n.rodata\nnop\n", 3, 1, "secao atual: .rodata");
    ASM_ERR("halt\n.bss\n.byte 1\n", 3, 1, "dados inicializados nao sao permitidos em .bss");
    ASM_ERR("halt\n.bss\n.space 4, 1\n", 3, 9, "valor de preenchimento nao permitido");
    ASM_ERR(".byte 1\nnop\n", 2, 1, "instrucao em endereco desalinhado (0x1)");
    ASM_ERR(".data\n.word 1\n", 0, 0, "nenhuma instrucao em .text");
}

TEST(layout_errors)
{
    ASM_ERR("halt\n.data\n.space fwd\nfwd:\n", 3, 8, "precisa ser constante");
    ASM_ERR("halt\n.data\n.space -1\n", 3, 8, "negativo");
    ASM_ERR("halt\n.align 3\n", 2, 8, "potencia de 2");
    ASM_ERR("halt\n.align 0\n", 2, 8, "potencia de 2");
    ASM_ERR("nop\nnop\n.org 4\n", 3, 6, ".org nao pode voltar");
    ASM_ERR("nop\n.entry 2\n", 2, 0, "fora de .text ou desalinhado");
    ASM_ERR("nop\n.entry 100\n", 2, 0, "fora de .text");
}

TEST(immediate_ranges)
{
    ASM_ERR("muli r1, r2, 8192\n", 1, 14, "imediato 8192 fora do alcance de imm14 (-8192..8191) em 'muli'");
    ASM_ERR("trap -8193\n", 1, 6, "fora do alcance de imm14");
    ASM_ERR("tsti r1, 10000\n", 1, 10, "fora do alcance de imm14");
    ASM_ERR("shli r1, r2, 32\n", 1, 14, "contagem de deslocamento 32 fora de 0..31");
    ASM_ERR("sar r1, r2, -1\n", 1, 13, "fora de 0..31");
    ASM_ERR("syscall 0x4000\n", 1, 9, "fora do alcance de 14 bits sem sinal");
    ASM_ERR("syscall -1\n", 1, 9, "14 bits sem sinal");
    ASM_ERR("mfsr r1, 20000\n", 1, 10, "14 bits sem sinal");
    ASM_ERR("movhi r1, 1\n", 1, 1, "mnemonico desconhecido 'movhi'");  /* removido da ISA */
    ASM_ERR("ldw r1, [r2 + 8192]\n", 1, 15, "deslocamento 8192 fora do alcance");
    ASM_ERR("stb [r2 - 8193], r1\n", 1, 9, "deslocamento -8193");
    ASM_ERR("ldw r1, [big]\nbig = 0x4000\n", 1, 10, "use 'la rN, simbolo'");
    ASM_ERR("adr r1, far\n.space 9000\nfar: halt\n", 1, 9, "imm14");
    /* addi explícito com valor desconhecido no passo 1 fica curto: erro no 2. */
    ASM_ERR("addi r1, r1, fwd\n.space 9000\nfwd: halt\n", 1, 14,
            "imediato 9004 fora do alcance de imm14 (-8192..8191) em 'addi'");
}

TEST(data_ranges)
{
    ASM_ERR("halt\n.data\n.byte 1, 256\n", 3, 10, "valor 256 nao cabe em um byte");
    ASM_ERR("halt\n.data\n.byte -129\n", 3, 7, "nao cabe em um byte");
    ASM_ERR("halt\n.data\n.half 65536\n", 3, 7, "nao cabe em .half");
    ASM_ERR("halt\n.data\n.half -32769\n", 3, 7, "nao cabe em .half");
    ASM_ERR("halt\n.data\n.space 2, 300\n", 3, 11, "nao cabe em um byte");
}

TEST(branch_ranges)
{
    ASM_ERR("b 6\n", 1, 3, "alvo de desvio 0x6 desalinhado");
    ASM_ERR("beq 0x100\n", 1, 5, "fora do codigo");
    ASM_ERR("halt\nb msg\n.data\nmsg: .byte 0\n", 2, 3, "fora do codigo");
    ASM_ERR("beq far\n.space 0x200000\nfar: halt\n", 1, 5, "fora do alcance de b<cond> (+-2 MiB)");
}

TEST(jump_range)
{
    /* off24 = +-32 MiB: maior que o limite de seção (16 MiB) do ksas, então
     * só o alvo fora do código ou desalinhado pode falhar. */
    ASM_ERR("call 0x1000000\n", 1, 6, "fora do codigo");
    ASM_ERR("jmp 2\n", 1, 5, "desalinhado");
}

TEST(continues_after_errors)
{
    asm_out o;

    ASSERT_TRUE(ta_assemble(&o, "foo\n"
                                "nop\n"
                                "add r1\n"
                                "li r1, undefined_sym\n"
                                "halt\n"
                                ".data\n"
                                ".byte 999\n") != 0);
    ASSERT_EQ(o.ndiag, 4);
    ASSERT_TRUE(strstr(o.diags, "t.ksasm:1:1:") != NULL);
    ASSERT_TRUE(strstr(o.diags, "t.ksasm:3:7:") != NULL);
    ASSERT_TRUE(strstr(o.diags, "t.ksasm:4:8:") != NULL);
    ASSERT_TRUE(strstr(o.diags, "t.ksasm:7:7:") != NULL);
    ta_free(&o);
}

TEST(one_error_per_line)
{
    asm_out o;

    ASSERT_TRUE(ta_assemble(&o, "add 1, 2, 3 $ @ #\nhalt\n") != 0);
    ASSERT_EQ(o.ndiag, 1);
    ta_free(&o);

    /* Sem erro em cascata ("nenhuma instrucao") quando a única linha falhou. */
    ASSERT_TRUE(ta_assemble(&o, "  foo r1\n") != 0);
    ASSERT_EQ(o.ndiag, 1);
    ta_free(&o);
}

TEST(error_limit)
{
    char src[4096];
    size_t used = 0;
    asm_out o;
    int i;

    for (i = 0; i < 40; i++) {
        used += (size_t)snprintf(src + used, sizeof src - used, "bad%d\n", i);
    }
    ASSERT_TRUE(ta_assemble(&o, src) != 0);
    ASSERT_EQ(o.res.errors, 20);
    ASSERT_EQ(o.ndiag, 21);                   /* 20 erros + "montagem abortada" */
    ASSERT_TRUE(strstr(o.diags, "muitos erros") != NULL);
    ASSERT_TRUE(strstr(o.diags, "bad20") == NULL);
    ta_free(&o);
}

SUITE(ksas_errors)
{
    RUN_TEST(diagnostic_format);
    RUN_TEST(binary_expr_left_error_reported_once);
    RUN_TEST(syntax_errors);
    RUN_TEST(lexical_errors_have_position);
    RUN_TEST(symbol_errors);
    RUN_TEST(section_errors);
    RUN_TEST(layout_errors);
    RUN_TEST(immediate_ranges);
    RUN_TEST(data_ranges);
    RUN_TEST(branch_ranges);
    RUN_TEST(jump_range);
    RUN_TEST(continues_after_errors);
    RUN_TEST(one_error_per_line);
    RUN_TEST(error_limit);
}
