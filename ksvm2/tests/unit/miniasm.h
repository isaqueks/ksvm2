/*
 * miniasm.h — montador mínimo com rótulos para os testes de programas
 * inteiros (test_programs.c, test_snapshot.c). Gera palavras com os
 * codificadores ksvm_enc_* e resolve desvios/endereços na ligação, para
 * que os programas não dependam de deslocamentos calculados à mão.
 */
#ifndef KSVM_TEST_MINIASM_H
#define KSVM_TEST_MINIASM_H

#include "ksvm/ksvm.h"

#define MA_MAX_WORDS  1024u
#define MA_MAX_LABELS 64u
#define MA_MAX_FIXUPS 128u

typedef struct ma_fix {
    unsigned at;        /* índice da palavra a corrigir */
    unsigned lab;       /* rótulo alvo */
    unsigned base_lab;  /* MA_DIFF: rótulo de referência */
    uint32_t base;      /* MA_ABS: endereço de carga do código */
    int      kind;
} ma_fix;

typedef struct ma {
    uint32_t w[MA_MAX_WORDS];
    unsigned n;
    long     lab[MA_MAX_LABELS];   /* posição em palavras; -1 = indefinido */
    ma_fix   fix[MA_MAX_FIXUPS];
    unsigned nfix;
    int      err;                  /* != 0: estouro, rótulo indefinido/repetido, alcance */
} ma;

void ma_init(ma *a);
void ma_emit(ma *a, uint32_t w);
/* Formato L: palavra + imm32. */
void ma_emit_l(ma *a, uint32_t w0, uint32_t imm32);
void ma_label(ma *a, unsigned id);
/* Bcc cond, rótulo */
void ma_b(ma *a, uint8_t cond, unsigned id);
/* JMP/CALL rótulo */
void ma_j(ma *a, uint8_t op, unsigned id);
/* ADR rd, rótulo (imm14) / ADRL rd, rótulo (imm32), relativos a PC_next. */
void ma_adr(ma *a, uint8_t rd, unsigned id);
void ma_adrl(ma *a, uint8_t rd, unsigned id);
/* .word endereço absoluto do rótulo com o código carregado em "base". */
void ma_word_abs(ma *a, unsigned id, uint32_t base);
/* .word (rótulo - base_id) em bytes: tabelas relativas (PIC). */
void ma_word_diff(ma *a, unsigned id, unsigned base_id);
/* Resolve tudo; 0 se ok. */
int ma_link(ma *a);
/* Endereço em bytes (relativo ao início do código) de um rótulo. */
uint32_t ma_addr(const ma *a, unsigned id);

#endif /* KSVM_TEST_MINIASM_H */
