/*
 * symtab.h — tabela de símbolos (doc 07 §2).
 *
 * Símbolos ficam num array em ordem de criação (índices estáveis, usados
 * pelos nós de expressão e itens) e num índice paralelo ordenado por nome
 * para busca binária. Nomes diferenciam maiúsculas.
 */
#ifndef KSAS_SYMTAB_H
#define KSAS_SYMTAB_H

#include "ir.h"

void ksas_sym_free(ksas_symtab *t);

/* Índice do símbolo ou KSAS_NONE. */
uint32_t ksas_sym_find(const ksas_ctx *c, const char *name, uint32_t len);

/* Encontra ou cria (como UNDEF, guardando line/col da primeira
 * referência). KSAS_NONE em erro (nome longo demais, limite). */
uint32_t ksas_sym_intern(ksas_ctx *c, const char *name, uint32_t len,
                         uint32_t line, uint32_t col);

/* Nome do símbolo (não terminado em NUL: use name_len). */
const char *ksas_sym_name(const ksas_ctx *c, uint32_t sym);

/* Define label/equ; reporta redefinição. false em erro. */
bool ksas_sym_define(ksas_ctx *c, uint32_t sym, ksas_sym_kind kind, uint32_t col);

#endif /* KSAS_SYMTAB_H */
