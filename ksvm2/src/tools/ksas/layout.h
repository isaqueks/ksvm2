/*
 * layout.h — passo 1: endereço de cada item (doc 07 §2).
 *
 * Toda instrução tem tamanho fixo pelo opcode; só as formas marcadas com
 * KSAS_INSN_VAR (li e afins) escolhem entre 4 e 8 bytes, e a escolha é
 * feita uma única vez com o que já se sabe neste ponto: valor conhecido
 * decide; desconhecido (label à frente, endereço de .data) assume a forma
 * longa quando KSAS_INSN_LONG_UNK. Sem relaxamento iterativo.
 *
 * Ao fim, define as bases das seções:
 *   .text em 0; .rodata logo depois (alinhada); CODE = text + rodata;
 *   RAM em ram_base = CODE alinhado a 16; .data; .bss após .data alinhado.
 */
#ifndef KSAS_LAYOUT_H
#define KSAS_LAYOUT_H

#include "ir.h"

/* false se o layout não pôde terminar (seção grande demais, abortado):
 * nesse caso não há passo 2. Erros comuns (.space inválido...) são
 * reportados e o item fica com tamanho 0, para seguir encontrando erros. */
bool ksas_layout(ksas_ctx *c);

#endif /* KSAS_LAYOUT_H */
