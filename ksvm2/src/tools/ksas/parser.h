/*
 * parser.h — tokens -> itens (doc 07 §2).
 *
 * Uma linha é: [label:]* [instrução | diretiva | nome = expr] [comentário].
 * Erros de sintaxe abandonam a linha (um erro por linha) e o parser segue
 * para a próxima, de modo que vários erros são reportados numa montagem.
 * As instruções são analisadas em insn.c; aqui ficam linhas, labels,
 * diretivas e os auxiliares de operandos compartilhados.
 */
#ifndef KSAS_PARSER_H
#define KSAS_PARSER_H

#include "ir.h"

/* Analisa um arquivo inteiro (file = índice em ctx->files). */
void ksas_parse_source(ksas_ctx *c, uint32_t file, const char *text);

/* Analisa uma definição de linha de comando "NOME" ou "NOME=expr" como
 * linha 1 do arquivo sintético "file". */
void ksas_parse_define(ksas_ctx *c, uint32_t file, const char *text);

/* Registra um arquivo-fonte; KSAS_NONE em erro. owned_* são liberados no fim. */
uint32_t ksas_file_add(ksas_ctx *c, const char *name, char *owned_name, char *owned_text,
                       bool listed);

/* ---- Auxiliares de operandos (usados por insn.c) ---------------------- */

bool ksas_at_eol(const ksas_cursor *cur);
/* Exige o token "kind" e o consome; "what" descreve o esperado. */
bool ksas_expect(ksas_ctx *c, ksas_cursor *cur, ksas_tok kind, const char *what);
bool ksas_expect_reg(ksas_ctx *c, ksas_cursor *cur, uint8_t *reg);
bool ksas_parse_expr(ksas_ctx *c, ksas_cursor *cur, uint32_t *node);
/* Nó constante. */
uint32_t ksas_const_node(ksas_ctx *c, uint32_t value, uint32_t col);

/* Literal float (.float, fli) no token corrente -> bits binary32 (RNE,
 * NaN canônico com o sinal pedido); deixa o cursor no token seguinte,
 * avançado sem reportar TK_ERROR. O cursor deve ter chegado ao literal da
 * mesma forma (ksas_cur_advance), porque "1.5" não é token do lexer. */
bool ksas_parse_float(ksas_ctx *c, ksas_cursor *cur, uint32_t *bits);

#endif /* KSAS_PARSER_H */
