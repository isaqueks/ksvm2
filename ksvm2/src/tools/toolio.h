/*
 * toolio.h — utilitários comuns às ferramentas de host (ksdis, ksimg, ksvm):
 * leitura de arquivo inteiro, números da linha de comando e nomes de erro.
 */
#ifndef KSVM_TOOLIO_H
#define KSVM_TOOLIO_H

#include "ksvm/types.h"

/* Maior arquivo .ksx aceito pelas ferramentas. */
#define TOOL_MAX_FILE (UINT32_C(64) * 1024u * 1024u)

/* Lê "path" inteiro para um buffer alocado (liberar com free). Em erro
 * imprime "prog: path: motivo" em stderr e devolve false. */
bool tool_read_file(const char *prog, const char *path, uint8_t **data, uint32_t *size);

/* Decimal, 0x hex ou 0b binário, com sufixo opcional k/K (x1024) ou m/M
 * (x1024^2). false se inválido ou fora de 32 bits. */
bool tool_parse_u32(const char *s, uint32_t *out);

/* Nome curto de um KSVM_E_* ("IMAGE_CHECKSUM"...). */
const char *tool_err_name(int err);

/* true se s é um identificador C: [A-Za-z_][A-Za-z0-9_]*. */
bool tool_is_ident(const char *s);

#endif /* KSVM_TOOLIO_H */
