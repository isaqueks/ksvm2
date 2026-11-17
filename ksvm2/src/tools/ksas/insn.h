/*
 * insn.h — análise de instruções e pseudo-instruções (doc 04 §5 e §7).
 *
 * Cada linha de instrução vira um ou mais itens INSN já com opcode,
 * registradores e a expressão do imediato. As formas de tamanho variável
 * (li, cmp/add/... com imediato, addi/subi/... fora de 14 bits) ficam
 * marcadas com KSAS_INSN_VAR para o layout escolher I (4 bytes) ou L (8).
 */
#ifndef KSAS_INSN_H
#define KSAS_INSN_H

#include "ir.h"

/* cur->tok é o mnemônico. false em erro (já reportado). */
bool ksas_parse_insn(ksas_ctx *c, ksas_cursor *cur);

#endif /* KSAS_INSN_H */
