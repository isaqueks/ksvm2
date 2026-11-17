/*
 * listing.h — listing de montagem (-l): endereço, bytes e fonte lado a
 * lado (doc 07 §2), seguido do resumo das seções e da tabela de símbolos.
 *
 *   ENDERECO  BYTES                    LINHA  FONTE
 *   00000000  02 04 00 00                  3  li r1, 0
 *
 * Itens com mais de 8 bytes continuam nas linhas seguintes; .space,
 * .align e .org mostram só os 8 primeiros bytes; .bss não tem bytes.
 */
#ifndef KSAS_LISTING_H
#define KSAS_LISTING_H

#include "ir.h"

/* Chamado depois de uma montagem sem erros. */
void ksas_listing(ksas_ctx *c);

#endif /* KSAS_LISTING_H */
