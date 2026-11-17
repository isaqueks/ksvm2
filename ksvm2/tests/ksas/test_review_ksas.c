/*
 * test_review_ksas.c — regressões da revisão adversarial do ksas e do
 * disassembler: cada TEST reproduz um defeito encontrado (ou cobre uma
 * lacuna) e falha sem a correção correspondente. Os oráculos são
 * independentes do código sob teste: palavras escritas à mão a partir do
 * diagrama de bits do doc 04 §3, e a palavra original no round-trip
 * ksvm_disasm -> ksas.
 */
#include <stdio.h>
#include <stdlib.h>

#include "ksas_test.h"
#include "ksvm/disasm.h"

/* Monta uma cópia do fonte num buffer do tamanho exato (sem folga depois
 * do NUL), para o ASan ver qualquer leitura além do fim. */
static int assemble_exact(asm_out *o, const char *src, const ksas_options *opt)
{
    size_t n = strlen(src) + 1u;
    char *copy = malloc(n);
    int rc;

    if (copy == NULL) {
        return -3;
    }
    memcpy(copy, src, n);
    rc = ta_assemble_ex(o, copy, opt, false);
    free(copy);
    return rc;
}

/* ---- Memória: crashes do assembler ------------------------------------ */

/* Instrução desalinhada recebia tamanho 0 no layout, mas o emit gravava 8
 * bytes no endereço dela: heap-buffer-overflow em c->code (ASan). */
TEST(misaligned_long_insn_does_not_overflow)
{
    asm_out o;

    ASSERT_TRUE(assemble_exact(&o, ".byte 1\nmovil r1, 0x12345678\n", NULL) != 0);
    ASSERT_EQ(o.ndiag, 1);
    ASSERT_EQ(o.first_line, 2);
    ASSERT_TRUE(strstr(o.first_msg, "desalinhado") != NULL);
    ta_free(&o);
    ASSERT_TRUE(assemble_exact(&o, "nop\n.byte 1, 2, 3\nadrl r2, 0\ncall 0\n", NULL) != 0);
    ta_free(&o);
}

/* '\' como último caractere do fonte: o escape consumia o NUL e o lexer
 * seguia lendo depois do fim do buffer (ASan: heap-buffer-overflow). */
TEST(escape_at_end_of_input_stays_in_bounds)
{
    asm_out o;

    ASSERT_TRUE(assemble_exact(&o, "halt\n.data\n.ascii \"abc\\", NULL) != 0);
    ASSERT_EQ(o.first_line, 3);
    ta_free(&o);
    ASSERT_TRUE(assemble_exact(&o, "li r1, '\\", NULL) != 0);
    ASSERT_EQ(o.first_line, 1);
    ta_free(&o);
    ASSERT_TRUE(assemble_exact(&o, "li r1, '\\x", NULL) != 0);
    ta_free(&o);
}

/* '\' no fim da linha consumia o '\n' sem contar a linha: a linha seguinte
 * era engolida e todo erro posterior saía com o número de linha errado. */
TEST(escape_before_newline_keeps_line_numbers)
{
    asm_out o;

    ASSERT_TRUE(assemble_exact(&o, "halt\n"
                                   ".data\n"
                                   ".ascii \"ab\\\n"
                                   ".byte 1\n"
                                   ".text\n"
                                   "zzz\n", NULL) != 0);
    ASSERT_EQ(o.ndiag, 2);
    ASSERT_TRUE(strstr(o.diags, "t.ksasm:3:") != NULL);
    ASSERT_TRUE(strstr(o.diags, "t.ksasm:6:1: erro: mnemonico desconhecido 'zzz'") != NULL);
    ta_free(&o);

    ASSERT_TRUE(assemble_exact(&o, "li r1, '\\\nnop\nzzz\n", NULL) != 0);
    ASSERT_EQ(o.ndiag, 2);
    ASSERT_TRUE(strstr(o.diags, "t.ksasm:3:1: erro: mnemonico desconhecido 'zzz'") != NULL);
    ta_free(&o);
}

/* ---- .include aninhado ------------------------------------------------ */

/* Sistema de arquivos em memória; resolve como o CLI (main.c): caminho
 * relativo ao diretório de "from". */
static char *vfs_include(void *ctx, const char *from, const char *path, const char **err)
{
    static const struct { const char *name, *text; } files[] = {
        { "proj/sub/a.inc", ".include \"b.inc\"\nA = B + 1\n" },
        { "proj/sub/b.inc", "B = 41\n" },
        { "proj/sub/bad.inc", "nop\n  frob\n" },
    };
    const char *slash = strrchr(from, '/');
    char full[256];
    size_t i;
    char *copy;

    (void)ctx;
    snprintf(full, sizeof full, "%.*s%s", slash != NULL ? (int)(slash - from + 1) : 0, from, path);
    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        if (strcmp(full, files[i].name) == 0) {
            copy = malloc(strlen(files[i].text) + 1u);
            if (copy != NULL) {
                strcpy(copy, files[i].text);
            }
            return copy;
        }
    }
    *err = "nao existe";
    return NULL;
}

/* O arquivo incluído era registrado com o caminho literal da diretiva
 * ("sub/a.inc"), então um .include dentro dele resolvia relativo ao
 * diretório corrente e não ao dele (doc 07: relativo a quem inclui). */
TEST(nested_include_is_relative_to_includer)
{
    ksas_options opt;
    asm_out o;

    memset(&opt, 0, sizeof opt);
    opt.filename = "proj/main.ksasm";
    opt.include = vfs_include;
    if (ta_assemble_ex(&o, ".include \"sub/a.inc\"\nli r1, A\nhalt\n", &opt, false) != 0) {
        test_note(t_, "%s", o.diags);
    }
    ASSERT_EQ(o.rc, 0);
    ASSERT_EQ_U32(ta_code32(&o, 0), 0x00A80102u);      /* movi r1, 42 (bits à mão) */
    ta_free(&o);

    /* Diagnóstico dentro do incluído cita o caminho resolvido. */
    ASSERT_TRUE(ta_assemble_ex(&o, ".include \"sub/bad.inc\"\n", &opt, false) != 0);
    ASSERT_TRUE(strstr(o.diags, "proj/sub/bad.inc:2:3: erro: mnemonico desconhecido") != NULL);
    ta_free(&o);
}

/* ---- Codificação: oráculo à mão (doc 04 §3), sem ksvm_enc_* ----------- */

typedef struct golden {
    const char *src;
    uint32_t    addr;
    uint32_t    word;
} golden;

TEST(golden_encodings_from_bit_diagram)
{
    /* op | ra<<8 | rb<<13 | rc<<18 ; imm14<<18 ; cond<<8 | off20<<12 ;
     * off24<<8 (deslocamentos em palavras, relativos a PC+4/PC+8). */
    static const golden cases[] = {
        { "add r1, r2, r3", 0, 0x000C4130u },
        { "addi r2, r1, -1", 0, 0xFFFC2231u },
        { "movi r3, 8191", 0, 0x7FFC0302u },
        { "movi r3, -8192", 0, 0x80000302u },
        { "movil r1, 0x12345678", 0, 0x00000103u },
        { "movil r1, 0x12345678", 4, 0x12345678u },
        { "stw [sp - 4], r1", 0, 0xFFF3E117u },
        { "ldbx r1, [r2 + r3]", 0, 0x000C4118u },
        { "mtsr 1, r2", 0, 0x00044008u },
        { "syscall 0x3FFF", 0, 0xFFFC0078u },
        { "jmpr r9", 0, 0x00012071u },
        { "beq fwd\nnop\nfwd: halt", 0, 0x00001172u },
        { "x: bne x", 0, 0xFFFFF272u },
        { "back: nop\ncall back", 4, 0xFFFFFE73u },
        { "jmp f\nnop\nf: halt", 0, 0x00000170u },
        { "adr r1, t\nt: halt", 0, 0x00000105u },
        { "t: adrl r4, t", 4, 0xFFFFFFF8u },
        { "sub r1, r2, 70000", 4, 0xFFFEEE90u },           /* ADDL -70000 */
        { "li r5, 0x8000", 0, 0x00000503u },
    };
    size_t i;
    asm_out o;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (ta_assemble(&o, cases[i].src) != 0 || ta_code32(&o, cases[i].addr) != cases[i].word) {
            test_note(t_, "\"%s\" @%lu: 0x%08lX, esperado 0x%08lX\n%s", cases[i].src,
                      (unsigned long)cases[i].addr, (unsigned long)ta_code32(&o, cases[i].addr),
                      (unsigned long)cases[i].word, o.diags);
            ta_free(&o);
            ASSERT_TRUE(0);
        }
        ta_free(&o);
    }
}

TEST(branch_and_adr_range_edges)
{
    asm_out o;

    /* Maior desvio para frente: +2 MiB - 4 (off20 = 0x7FFFF). */
    ASM_OK(&o, "bal far\n.space 0x200000 - 4\nfar: halt\n");
    ASSERT_EQ_U32(ta_code32(&o, 0), 0x7FFFF072u);
    ta_free(&o);
    /* ADR: imm14 entre -8192 e 8191 (alvo - PC_next). */
    ASM_OK(&o, "adr r1, . + 4 + 8191\nhalt\n");
    ASSERT_EQ_U32(ta_code32(&o, 0), 0x7FFC0105u);
    ta_free(&o);
    ASM_OK(&o, ".org 0x2000\nadr r1, . + 4 - 8192\nhalt\n");
    ASSERT_EQ_U32(ta_code32(&o, 0x2000), 0x80000105u);
    ta_free(&o);
    ASM_ERR("adr r1, . + 4 + 8192\nhalt\n", 1, 0, "imm14");
}

TEST(dot_and_data_addresses_choose_long_li)
{
    asm_out o;

    /* "." e labels de .data ainda não posicionados no passo 1: li fica
     * longo e recebe o valor final. */
    ASM_OK(&o, "li r1, here\nli r2, here - there\nhalt\n"
               ".data\n.word 0\nhere = .\nthere: .word 1\n");
    ASSERT_EQ_U32(ta_code32(&o, 0), 0x00000103u);
    ASSERT_EQ_U32(ta_code32(&o, 4), o.res.ram_base + 4u);
    ASSERT_EQ_U32(ta_code32(&o, 8), 0x00000203u);      /* labels adiante no passo 1: movil */
    ASSERT_EQ_U32(ta_code32(&o, 12), 0);               /* here - there = 0 */
    ta_free(&o);
}

TEST(define_with_hex_separator)
{
    static const char *const defs[] = { "HEX=0x_FF", "NEG=-2" };
    ksas_options opt;
    asm_out o;

    memset(&opt, 0, sizeof opt);
    opt.defines = defs;
    opt.num_defines = 2;
    ASSERT_EQ(ta_assemble_ex(&o, "halt\n.data\n.word HEX, NEG\n", &opt, false), 0);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off), 0xFF);
    ASSERT_EQ_U32(ksvm_ld32le(o.res.image + o.hdr.data_off + 4), 0xFFFFFFFEu);
    ta_free(&o);
}

/* ---- Round-trip ksvm_disasm -> ksas para todo opcode -------------------- */

static char target_name[16];

/* Alvo PC-relativo como número absoluto: expressão que o ksas aceita. */
static const char *hex_target(void *ctx, uint32_t addr)
{
    (void)ctx;
    snprintf(target_name, sizeof target_name, "0x%lX", (unsigned long)addr);
    return target_name;
}

static uint32_t rnd(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

#define RT_PC   UINT32_C(0x1000)
#define RT_END  UINT32_C(0x2000)

/* Palavra -> texto do disassembler -> ksas -> mesma(s) palavra(s)?
 * Devolve 1 se testou, 0 se a palavra saiu como .word ou o alvo não é
 * montável (desvio para fora do código), -1 em divergência. */
static int roundtrip_word(test_ctx *t_, uint32_t w, uint32_t imm32)
{
    char text[KSVM_DISASM_BUF], src[160];
    unsigned len = ksvm_disasm_one_ex(RT_PC, w, imm32, hex_target, NULL, text, sizeof text);
    uint32_t target = 0, op = w & 0xFFu;
    asm_out o;
    int r = 1;

    if (strncmp(text, ".word", 5) == 0) {
        return 0;
    }
    if ((op == KSVM_OP_BCOND || op == KSVM_OP_JMP || op == KSVM_OP_CALL) &&
        (!ksvm_disasm_target(RT_PC, w, imm32, &target) || target >= RT_END)) {
        return 0;
    }
    snprintf(src, sizeof src, ".org 0x%lX\n%s\n.org 0x%lX\nhalt\n", (unsigned long)RT_PC, text,
             (unsigned long)RT_END);
    if (ta_assemble(&o, src) != 0) {
        test_note(t_, "0x%08lX -> \"%s\" nao monta: %s", (unsigned long)w, text, o.diags);
        r = -1;
    } else if (ta_code32(&o, RT_PC) != w || (len == 8u && ta_code32(&o, RT_PC + 4u) != imm32)) {
        test_note(t_, "0x%08lX/0x%08lX -> \"%s\" -> 0x%08lX/0x%08lX", (unsigned long)w,
                  (unsigned long)imm32, text, (unsigned long)ta_code32(&o, RT_PC),
                  (unsigned long)ta_code32(&o, RT_PC + 4u));
        r = -1;
    }
    ta_free(&o);
    return r;
}

TEST(disasm_output_reassembles_bit_exact)
{
    /* Máscaras dos campos (ra, rb, rc, imm14, off20...) combinadas para
     * que todo opcode tenha formas canônicas entre as palavras geradas. */
    static const uint32_t masks[] = {
        0x00000000u, 0x00001F00u, 0x0003FF00u, 0x007FFF00u, 0xFFFC1F00u, 0xFFFFFF00u,
        0xFFFE0000u, 0xFFFC0000u, 0x0003E000u, 0x00000F00u, 0x00FFFF00u
    };
    static const uint8_t ops[] = {
#define KSVM_OP(name, code, fmt, fl, desc) code,
#include "ksvm/isa.def"
#undef KSVM_OP
    };
    uint32_t s = 0x12345u;
    size_t i, m, k;

    for (i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        unsigned tested = 0;
        for (m = 0; m < sizeof masks / sizeof masks[0]; m++) {
            for (k = 0; k < 12u; k++) {
                uint32_t w = ops[i] | (rnd(&s) & masks[m]);
                int r;
                if (ops[i] == KSVM_OP_BCOND || ops[i] == KSVM_OP_JMP || ops[i] == KSVM_OP_CALL) {
                    /* deslocamentos pequenos (com sinal) para cair no código */
                    uint32_t words = (rnd(&s) % 0x600u) - 0x300u;
                    w = ops[i] | (rnd(&s) & 0xF00u) |
                        ((ops[i] == KSVM_OP_BCOND) ? (words & 0xFFFFFu) << 12
                                                   : (words & 0xFFFFFFu) << 8);
                }
                r = roundtrip_word(t_, w, rnd(&s));
                ASSERT_TRUE(r >= 0);
                tested += (unsigned)r;
            }
        }
        if (tested == 0u) {
            test_note(t_, "opcode 0x%02X sem forma canonica testada", (unsigned)ops[i]);
        }
        ASSERT_TRUE(tested > 0u);
    }
}

/* Os valores de 14 bits que o disassembler mostra para MFSR/MTSR e as
 * contagens de SHLI/SHRI/SARI precisam caber no que o ksas aceita. */
TEST(disasm_special_immediates_reassemble)
{
    static const uint32_t words[] = {
        0xFFFC0107u,      /* mfsr r1, 16383 */
        0x8CA00107u,      /* mfsr r1, 9000 */
        0xFFFC4008u,      /* mtsr 16383, r2 */
        0x00A0415Bu,      /* shli r1, r2, 40: contagem fora de 0..31 */
        0xFFFC415Du,      /* shri r1, r2, -1 */
        0x007C415Fu,      /* sari r1, r2, 31 */
    };
    size_t i;

    for (i = 0; i < sizeof words / sizeof words[0]; i++) {
        ASSERT_TRUE(roundtrip_word(t_, words[i], 0) >= 0);
    }
}

/* ---- Robustez: fontes mutadas ----------------------------------------- */

/* Mutações determinísticas (inserir caractere "perigoso", apagar, trocar,
 * truncar) de fontes válidos. Oráculo: nada de crash/ASan, todo erro tem
 * diagnóstico e toda imagem gerada passa no ksvm_image_parse (rc -2 em
 * ta_assemble_ex). Foi assim que o overflow do emit apareceu. */
TEST(mutated_sources_never_crash)
{
    static const char *const seeds[] = {
        "_start: li r1, msg\n  la r2, buf\nloop: ldb r3, [r2 + 1]\n  beq loop\n"
        "  call f\n  halt\nf: ret\n.rodata\nmsg: .asciz \"a\\tb\\x41\"\n"
        ".data\n.align 8\nv: .word f - _start, 'c', 1 << 4\n.bss\nbuf: .space 16\n",
        "N = 3 * (2 + 1)\n.equ M, N - 1\n.entry e\n.stack 512\ne: cmp r1, N\n"
        "  sub r2, r3, 70000\n  stw [sp - 4], r1\n  mfsr r4, FLAGS\n  syscall PUTC\n"
        "  .align 16\n  .org 0x40\n  adr r5, e\n  push r1, r2\n  b.ne e\n  halt\n",
    };
    static const char danger[] = "\\\"'\n.:[](),+-*/%<>~#;0x9aZr_ \t";
    char buf[1024];
    uint32_t s = 0xC0FFEEu;
    unsigned i;
    asm_out o;

    for (i = 0; i < 3000u; i++) {
        const char *seed = seeds[i % 2u];
        size_t len = strlen(seed), pos, k, nmut = 1u + rnd(&s) % 4u;

        memcpy(buf, seed, len + 1u);
        for (k = 0; k < nmut && len > 0u; k++) {
            pos = rnd(&s) % len;
            switch (rnd(&s) % 4u) {
            case 0:                                    /* insere */
                if (len + 2u < sizeof buf) {
                    memmove(buf + pos + 1u, buf + pos, len - pos + 1u);
                    buf[pos] = danger[rnd(&s) % (sizeof danger - 1u)];
                    len++;
                }
                break;
            case 1:                                    /* apaga */
                memmove(buf + pos, buf + pos + 1u, len - pos);
                len--;
                break;
            case 2:                                    /* troca */
                buf[pos] = danger[rnd(&s) % (sizeof danger - 1u)];
                break;
            default:                                   /* trunca */
                buf[pos] = '\0';
                len = pos;
                break;
            }
        }
        (void)assemble_exact(&o, buf, NULL);
        if (o.rc == -2 || (o.rc != 0 && o.ndiag == 0u)) {
            test_note(t_, "mutacao %u: rc %d, %u diagnosticos:\n%s", i, o.rc, o.ndiag, buf);
        }
        ASSERT_TRUE(o.rc != -2);
        ASSERT_TRUE(o.rc == 0 || o.ndiag > 0u);
        ta_free(&o);
    }
}

SUITE(ksas_review)
{
    RUN_TEST(misaligned_long_insn_does_not_overflow);
    RUN_TEST(escape_at_end_of_input_stays_in_bounds);
    RUN_TEST(escape_before_newline_keeps_line_numbers);
    RUN_TEST(nested_include_is_relative_to_includer);
    RUN_TEST(golden_encodings_from_bit_diagram);
    RUN_TEST(branch_and_adr_range_edges);
    RUN_TEST(dot_and_data_addresses_choose_long_li);
    RUN_TEST(define_with_hex_separator);
    RUN_TEST(disasm_output_reassembles_bit_exact);
    RUN_TEST(disasm_special_immediates_reassemble);
    RUN_TEST(mutated_sources_never_crash);
}
