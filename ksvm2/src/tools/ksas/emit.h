/*
 * emit.h — passo 2: bytes de cada item (doc 07 §2).
 *
 * Com o layout pronto todo símbolo tem endereço absoluto. Aqui cada
 * instrução é codificada com ksvm_enc_* (mesmos codificadores dos testes
 * do núcleo), cada imediato/deslocamento tem o alcance verificado e cada
 * dado é gravado little-endian nos buffers de CODE (.text + .rodata) e
 * .data. Erros são reportados com a linha/coluna do operando e a emissão
 * continua para encontrar os demais.
 */
#ifndef KSAS_EMIT_H
#define KSAS_EMIT_H

#include "ir.h"

/* Aloca c->code/c->data e emite todos os itens. false só em falta de
 * memória (erros de montagem apenas incrementam c->errors). */
bool ksas_emit(ksas_ctx *c);

#endif /* KSAS_EMIT_H */
