/*
 * test_ksas_lexer.c — tokens, literais, escapes, comentários e erros léxicos.
 */
#include "ksas_test.h"
#include "lexer.h"

#define MAX_TOKS 64

/* Tokeniza tudo (inclusive o EOF final); devolve a quantidade. */
static int lex_all(const char *src, ksas_token *toks)
{
    ksas_lexer lx;
    int n = 0;

    ksas_lex_init(&lx, src);
    do {
        ksas_lex_next(&lx, &toks[n]);
    } while (toks[n++].kind != TK_EOF && n < MAX_TOKS);
    return n;
}

TEST(basic_instruction_line)
{
    ksas_token t[MAX_TOKS];
    int n = lex_all("  li r1, 0x1F ; comentario\n", t);

    ASSERT_EQ(n, 6);
    ASSERT_EQ(t[0].kind, TK_IDENT);
    ASSERT_EQ(t[0].col, 3);
    ASSERT_EQ(t[0].len, 2);
    ASSERT_EQ(t[1].kind, TK_REG);
    ASSERT_EQ(t[1].value, 1);
    ASSERT_EQ(t[1].col, 6);
    ASSERT_EQ(t[2].kind, TK_COMMA);
    ASSERT_EQ(t[3].kind, TK_NUM);
    ASSERT_EQ_U32(t[3].value, 0x1F);
    ASSERT_EQ(t[3].col, 10);
    ASSERT_EQ(t[4].kind, TK_NEWLINE);
    ASSERT_EQ(t[5].kind, TK_EOF);
}

TEST(registers_and_aliases)
{
    static const struct { const char *name; uint8_t idx; } regs[] = {
        { "zr", 0 }, { "r0", 0 }, { "r9", 9 }, { "r31", 31 }, { "fp", 30 }, { "sp", 31 },
        { "a0", 1 }, { "a3", 4 }, { "t0", 5 }, { "t10", 15 }, { "s0", 16 }, { "s13", 29 },
        { "R5", 5 }, { "SP", 31 }, { "Zr", 0 }
    };
    static const char *const not_regs[] = { "r32", "a4", "t11", "s14", "r01", "rx", "r" };
    size_t i;
    uint8_t idx;

    for (i = 0; i < sizeof regs / sizeof regs[0]; i++) {
        ASSERT_TRUE(ksas_reg_parse(regs[i].name, (uint32_t)strlen(regs[i].name), &idx));
        ASSERT_EQ(idx, regs[i].idx);
    }
    for (i = 0; i < sizeof not_regs / sizeof not_regs[0]; i++) {
        ASSERT_FALSE(ksas_reg_parse(not_regs[i], (uint32_t)strlen(not_regs[i]), &idx));
    }
}

TEST(numbers)
{
    ksas_token t[MAX_TOKS];
    static const uint32_t expect[] = {
        0, 42, 0x2A, 42, 1000000, 0xFFFFFFFFu, 4294967295u, 0xABCDEF, 5
    };
    int n = lex_all("0 42 0x2A 0b101010 1_000_000 0xFFFF_FFFF 4294967295 0XabcDEF 0B101", t);
    size_t i;

    ASSERT_EQ(n, 10);
    for (i = 0; i < sizeof expect / sizeof expect[0]; i++) {
        ASSERT_EQ(t[i].kind, TK_NUM);
        ASSERT_EQ_U32(t[i].value, expect[i]);
    }
}

TEST(number_errors)
{
    static const char *const bad[] = { "4294967296", "0x1_0000_0000", "12abc", "0x", "0b2", "9z" };
    ksas_token t[MAX_TOKS];
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        lex_all(bad[i], t);
        ASSERT_EQ(t[0].kind, TK_ERROR);
        ASSERT_EQ(t[0].col, 1);
        ASSERT_EQ(t[1].kind, TK_EOF);      /* o resto do lexema é consumido */
    }
    lex_all("4294967296", t);
    ASSERT_TRUE(strstr(t[0].err, "32 bits") != NULL);
}

TEST(char_literals)
{
    ksas_token t[MAX_TOKS];
    static const uint32_t expect[] = { 'A', '\n', 0x41, '\'', '"', 0, '\\', ' ' };
    int n = lex_all("'A' '\\n' '\\x41' '\\'' '\"' '\\0' '\\\\' ' '", t);
    size_t i;

    ASSERT_EQ(n, 9);
    for (i = 0; i < sizeof expect / sizeof expect[0]; i++) {
        ASSERT_EQ(t[i].kind, TK_CHAR);
        ASSERT_EQ_U32(t[i].value, expect[i]);
    }
}

TEST(char_errors)
{
    ksas_token t[MAX_TOKS];

    lex_all("''", t);
    ASSERT_EQ(t[0].kind, TK_ERROR);
    lex_all("'ab'", t);
    ASSERT_EQ(t[0].kind, TK_ERROR);
    lex_all("'\\q'", t);
    ASSERT_EQ(t[0].kind, TK_ERROR);
    ASSERT_TRUE(strstr(t[0].err, "escape") != NULL);
}

TEST(strings_and_escapes)
{
    ksas_token t[MAX_TOKS];
    static const uint8_t expect[] = { 'a', '\t', 'b', 'A', 0, '\\', '"', '\n', '\r', '\'', 0xFF };
    uint8_t buf[32];
    int n = lex_all("\"a\\tb\\x41\\0\\\\\\\"\\n\\r\\'\\xfF\"", t);

    ASSERT_EQ(n, 2);
    ASSERT_EQ(t[0].kind, TK_STR);
    ASSERT_EQ(ksas_lex_unescape(&t[0], NULL), sizeof expect);
    ASSERT_EQ(ksas_lex_unescape(&t[0], buf), sizeof expect);
    ASSERT_MEM_EQ(buf, expect, sizeof expect);
}

TEST(string_errors)
{
    ksas_token t[MAX_TOKS];

    lex_all("  \"abc\n", t);
    ASSERT_EQ(t[0].kind, TK_ERROR);
    ASSERT_EQ(t[0].col, 3);
    ASSERT_TRUE(strstr(t[0].err, "aspas") != NULL);
    ASSERT_EQ(t[1].kind, TK_NEWLINE);       /* não engole a próxima linha */

    lex_all("\"ab\\qc\" x", t);
    ASSERT_EQ(t[0].kind, TK_ERROR);
    ASSERT_EQ(t[0].col, 4);                 /* coluna da barra do escape */
    ASSERT_EQ(t[1].kind, TK_IDENT);         /* retoma depois da string */

    lex_all("\"\\x4\"", t);
    ASSERT_EQ(t[0].kind, TK_ERROR);
}

TEST(comments)
{
    ksas_token t[MAX_TOKS];
    int n = lex_all("; ponto e virgula\n# cerquilha\n// barras\nnop // fim\n", t);

    ASSERT_EQ(n, 6);
    ASSERT_EQ(t[0].kind, TK_NEWLINE);
    ASSERT_EQ(t[1].kind, TK_NEWLINE);
    ASSERT_EQ(t[2].kind, TK_NEWLINE);
    ASSERT_EQ(t[3].kind, TK_IDENT);
    ASSERT_EQ(t[4].kind, TK_NEWLINE);
}

TEST(punctuation)
{
    static const ksas_tok expect[] = {
        TK_LBRACK, TK_RBRACK, TK_LPAREN, TK_RPAREN, TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH,
        TK_PERCENT, TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_SHL, TK_SHR, TK_EQUALS,
        TK_COLON, TK_COMMA, TK_DOT, TK_EOF
    };
    ksas_token t[MAX_TOKS];
    int n = lex_all("[ ] ( ) + - * / % & | ^ ~ << >> = : , .", t);
    size_t i;

    ASSERT_EQ(n, (int)(sizeof expect / sizeof expect[0]));
    for (i = 0; i < sizeof expect / sizeof expect[0]; i++) {
        ASSERT_EQ(t[i].kind, expect[i]);
    }
}

TEST(directives_and_dotted_names)
{
    ksas_token t[MAX_TOKS];

    lex_all(".text b.eq a.b . .", t);
    ASSERT_EQ(t[0].kind, TK_DIRECTIVE);
    ASSERT_TRUE(ksas_name_eq(t[0].text, t[0].len, "text"));
    ASSERT_EQ(t[1].kind, TK_IDENT);
    ASSERT_EQ(t[1].len, 4);
    ASSERT_EQ(t[2].kind, TK_IDENT);
    ASSERT_EQ(t[3].kind, TK_DOT);
    ASSERT_EQ(t[4].kind, TK_DOT);
}

TEST(lines_columns_and_crlf)
{
    ksas_lexer lx;
    ksas_token t;

    ksas_lex_init(&lx, "nop\r\n  halt\r\n");
    ksas_lex_next(&lx, &t);
    ASSERT_EQ(t.kind, TK_IDENT);
    ASSERT_EQ(ksas_lex_line_len(lx.line_start), 3);   /* sem o \r */
    ksas_lex_next(&lx, &t);
    ASSERT_EQ(t.kind, TK_NEWLINE);
    ASSERT_EQ(lx.line, 2);
    ksas_lex_next(&lx, &t);
    ASSERT_EQ(t.kind, TK_IDENT);
    ASSERT_EQ(t.col, 3);
    ksas_lex_next(&lx, &t);
    ASSERT_EQ(t.kind, TK_NEWLINE);
    ksas_lex_next(&lx, &t);
    ASSERT_EQ(t.kind, TK_EOF);
}

TEST(unexpected_characters)
{
    ksas_token t[MAX_TOKS];

    lex_all("nop @", t);
    ASSERT_EQ(t[1].kind, TK_ERROR);
    ASSERT_EQ(t[1].col, 5);
    ASSERT_TRUE(strstr(t[1].err, "inesperado") != NULL);
    lex_all("1 < 2", t);
    ASSERT_EQ(t[1].kind, TK_ERROR);
}

TEST(peek_does_not_consume)
{
    ksas_cursor cur;
    ksas_token next;

    ksas_cur_init(&cur, "loop: nop");
    ksas_cur_advance(&cur);
    ksas_cur_peek(&cur, &next);
    ASSERT_EQ(cur.tok.kind, TK_IDENT);
    ASSERT_EQ(next.kind, TK_COLON);
    ksas_cur_advance(&cur);
    ASSERT_EQ(cur.tok.kind, TK_COLON);
}

TEST(names_case_insensitive)
{
    ASSERT_TRUE(ksas_name_eq("ADDI", 4, "addi"));
    ASSERT_TRUE(ksas_name_eq("AdDi", 4, "ADDI"));
    ASSERT_FALSE(ksas_name_eq("add", 3, "addi"));
    ASSERT_FALSE(ksas_name_eq("addi", 4, "add"));
}

SUITE(ksas_lexer)
{
    RUN_TEST(basic_instruction_line);
    RUN_TEST(registers_and_aliases);
    RUN_TEST(numbers);
    RUN_TEST(number_errors);
    RUN_TEST(char_literals);
    RUN_TEST(char_errors);
    RUN_TEST(strings_and_escapes);
    RUN_TEST(string_errors);
    RUN_TEST(comments);
    RUN_TEST(punctuation);
    RUN_TEST(directives_and_dotted_names);
    RUN_TEST(lines_columns_and_crlf);
    RUN_TEST(unexpected_characters);
    RUN_TEST(peek_does_not_consume);
    RUN_TEST(names_case_insensitive);
}
