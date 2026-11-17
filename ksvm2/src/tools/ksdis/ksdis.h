/*
 * ksdis.h — estado compartilhado pelos módulos do ksdis.
 *
 *   symtab.c  tabela de símbolos do .ksx e conjunto de labels gerados
 *   listing.c saída de inspeção (cabeçalho, endereços, bytes, hex+ASCII)
 *   raw.c     saída reassemblável (--raw): diretivas + instruções
 *   main.c    linha de comando
 */
#ifndef KSDIS_H
#define KSDIS_H

#include <stdio.h>

#include "ksvm/disasm.h"
#include "ksvm/loader.h"

/* Campo kind da tabela de símbolos, na convenção do ksas (ksas.h): seção
 * nos bits 0..1 (3 = constante .equ) e bit 7 = .global. */
#define KSDIS_SYMK_SECTION 0x03u
#define KSDIS_SYMK_CODE    0x00u
#define KSDIS_SYMK_DATA    0x01u
#define KSDIS_SYMK_BSS     0x02u
#define KSDIS_SYMK_ABS     0x03u
#define KSDIS_SYMK_GLOBAL  0x80u

typedef struct ksdis_sym {
    uint32_t addr;
    uint32_t index;      /* ordem no arquivo (desempate estável) */
    uint8_t  kind;
    bool     name_ok;    /* identificador válido no ksas (não é registrador) */
    bool     label_ok;   /* name_ok e não é constante: vira label no --raw */
    bool     emitted;    /* --raw: label já escrito */
    char    *name;
} ksdis_sym;

typedef struct ksdis {
    FILE              *out;
    const char        *path;
    const uint8_t     *img;
    uint32_t           img_size;
    ksvm_image_header  h;
    const uint8_t     *code;
    const uint8_t     *data;
    uint32_t           ram_base;       /* efetivo: cabeçalho ou código alinhado a 16 */
    ksdis_sym         *syms;           /* ordenados por endereço */
    size_t             nsyms;
    uint32_t          *labels;         /* --raw: alvos sem símbolo, ordenados */
    size_t             nlabels;
    size_t             cap_labels;
    char               namebuf[16];    /* "L_XXXXXXXX" devolvido pelo resolvedor */
} ksdis;

/* symtab.c */
bool ksdis_load_syms(ksdis *d);             /* false: tabela corrompida (aviso) */
void ksdis_free(ksdis *d);
/* Primeiro símbolo em addr (só os com label_ok se labels_only), ou NULL. */
const ksdis_sym *ksdis_sym_at(const ksdis *d, uint32_t addr, bool labels_only);
/* Índice do primeiro símbolo com endereço >= addr (pode ser nsyms). */
size_t ksdis_sym_lower(const ksdis *d, uint32_t addr);
bool ksdis_add_label(ksdis *d, uint32_t addr);
void ksdis_sort_labels(ksdis *d);
bool ksdis_has_label(const ksdis *d, uint32_t addr);
/* Menor endereço em [lo, hi) com label gerado ou símbolo label_ok; hi se
 * não houver. */
uint32_t ksdis_next_label(const ksdis *d, uint32_t lo, uint32_t hi);

/* Leitura little-endian de uma palavra do código. */
uint32_t ksdis_code_word(const ksdis *d, uint32_t off);

/* listing.c / raw.c */
void ksdis_listing(ksdis *d);
bool ksdis_raw(ksdis *d);

#endif /* KSDIS_H */
