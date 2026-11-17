/*
 * ksas.h — API do assembler KSVM v2 (doc 07).
 *
 * O núcleo do assembler não faz E/S de arquivo: o fonte entra como string,
 * .include é resolvido por callback e diagnósticos/listing saem por
 * callbacks. O CLI (main.c) e os testes são apenas clientes desta API.
 */
#ifndef KSAS_H
#define KSAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Um diagnóstico. line/col são 1-based; col 0 = sem coluna, line 0 = sem
 * posição. text é a linha completa "arquivo:linha:coluna: erro: mensagem". */
typedef struct ksas_diag {
    const char *file;
    uint32_t    line;
    uint32_t    col;
    const char *message;
    const char *text;
} ksas_diag;

typedef void (*ksas_diag_fn)(void *ctx, const ksas_diag *d);

/* Uma linha do listing, já terminada em '\n'. */
typedef void (*ksas_listing_fn)(void *ctx, const char *line);

/*
 * Lê o arquivo "path" pedido por um .include dentro de "from" (nome do
 * arquivo que contém a diretiva, útil para caminhos relativos). Devolve o
 * texto terminado em NUL, alocado com malloc (o assembler chama free), ou
 * NULL com *err apontando para uma mensagem estática.
 */
typedef char *(*ksas_include_fn)(void *ctx, const char *from, const char *path,
                                 const char **err);

typedef struct ksas_options {
    const char        *filename;      /* nome nas mensagens; NULL = "<fonte>" */
    const char *const *defines;       /* "NOME=expr" ou "NOME" (vale 1) */
    size_t             num_defines;
    bool               debug_symbols; /* -g: todos os labels na tabela */
    ksas_diag_fn       diag;          /* NULL: diagnósticos descartados */
    void              *diag_ctx;
    ksas_listing_fn    listing;       /* NULL: sem listing */
    void              *listing_ctx;
    ksas_include_fn    include;       /* NULL: .include é erro */
    void              *include_ctx;
} ksas_options;

typedef struct ksas_result {
    uint8_t *image;          /* .ksx completo (malloc); NULL se houve erro */
    uint32_t image_size;
    unsigned errors;
    uint32_t entry;
    uint32_t code_size;      /* .text + .rodata */
    uint32_t data_size;      /* .data com padding final */
    uint32_t bss_size;
    uint32_t ram_base;
    uint32_t stack_size;
    uint16_t flags;          /* KSVM_KSX_FLAG_* */
} ksas_result;

/*
 * Tabela de símbolos do .ksx (doc 05 §2: u32 addr, u8 kind, u8 len, name).
 * kind = seção (bits 0..1) | KSAS_SYMK_GLOBAL. Sem -g só entram os
 * símbolos marcados com .global; com -g entram todos os labels.
 */
#define KSAS_SYMK_CODE   0x00u   /* .text / .rodata */
#define KSAS_SYMK_DATA   0x01u
#define KSAS_SYMK_BSS    0x02u
#define KSAS_SYMK_ABS    0x03u   /* constante (.equ global) */
#define KSAS_SYMK_GLOBAL 0x80u

/* Monta "src" (terminado em NUL). opt pode ser NULL. Devolve 0 em sucesso
 * ou -1 se houve erros (res->errors > 0). Libere com ksas_result_free. */
int ksas_assemble(const char *src, const ksas_options *opt, ksas_result *res);

void ksas_result_free(ksas_result *res);

#endif /* KSAS_H */
