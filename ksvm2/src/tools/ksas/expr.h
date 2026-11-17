/*
 * expr.h — expressões: parser com precedência e avaliador (doc 07 §1).
 *
 * Precedência (menor -> maior), como em C:  |  ^  &  << >>  + -  * / %
 * Unários: - ~ +. Primários: número, 'c', símbolo, "." (endereço do item
 * corrente) e parênteses. Aritmética em 32 bits com wrap; "/" e "%" com
 * sinal (truncando para zero); ">>" lógico; deslocamento >= 32 dá 0.
 *
 * Avaliação em dois modos:
 *  - layout (passo 1): só constantes e labels de .text já posicionados têm
 *    valor absoluto. Labels de .rodata/.data/.bss posicionados têm valor
 *    relativo à seção (a base só é conhecida no fim do layout), de modo
 *    que "fim - inicio" na mesma seção já é constante. O resto é
 *    "desconhecido".
 *  - final (passo 2): todo símbolo tem endereço absoluto.
 */
#ifndef KSAS_EXPR_H
#define KSAS_EXPR_H

#include "ir.h"

#define KSAS_BASE_ABS 0xFFu   /* valor absoluto; senão = seção (relativo) */

typedef struct ksas_value {
    uint32_t v;
    uint8_t  base;
} ksas_value;

typedef enum ksas_ev {
    KSAS_EV_OK,
    KSAS_EV_UNKNOWN,   /* ainda sem valor (só no modo layout) */
    KSAS_EV_ERROR      /* erro reportado (ou suprimido se !report) */
} ksas_ev;

typedef struct ksas_evalctx {
    uint32_t line;      /* linha para mensagens de erro */
    uint32_t dot_item;  /* item cujo endereço é "." (KSAS_NONE: proibido) */
    bool     report;    /* reportar erros (no layout, só quem não reavalia) */
} ksas_evalctx;

/* Cria um nó; KSAS_NONE em erro. */
uint32_t ksas_node_new(ksas_ctx *c, ksas_node_op op, uint32_t col, uint32_t a, uint32_t b);

/* Analisa uma expressão a partir de cur->tok. false em erro (reportado). */
bool ksas_expr_parse(ksas_ctx *c, ksas_cursor *cur, uint32_t *out);

/* Avalia no modo corrente (c->final). */
ksas_ev ksas_expr_eval(ksas_ctx *c, const ksas_evalctx *e, uint32_t node, ksas_value *out);

/* Como ksas_expr_eval, mas exige valor absoluto: relativo vira UNKNOWN. */
ksas_ev ksas_expr_abs(ksas_ctx *c, const ksas_evalctx *e, uint32_t node, uint32_t *out);

/* true se o nó é um símbolo simples (sem operadores). */
bool ksas_expr_is_symbol(const ksas_ctx *c, uint32_t node);

#endif /* KSAS_EXPR_H */
