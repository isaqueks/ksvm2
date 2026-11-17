/*
 * ir.h — representação intermediária e contexto do assembler.
 *
 * Fluxo (doc 07 §2): parser -> itens (IR) por seção -> layout (passo 1,
 * endereços) -> emit (passo 2, bytes) -> writer .ksx. Todos os arrays
 * crescem sob demanda até um limite configurável; estourar o limite é um
 * erro de montagem com mensagem clara, nunca um crash.
 */
#ifndef KSAS_IR_H
#define KSAS_IR_H

#include <stdarg.h>

#include "ksas.h"
#include "lexer.h"

#define KSAS_NONE  UINT32_MAX   /* índice ausente (nó, símbolo, item) */
#define KSAS_NO_OP 0xFFu        /* opcode ausente (forma longa inexistente) */

/* ---- Limites (sobrescrevíveis com -D na compilação) ------------------- */
#ifndef KSAS_MAX_ERRORS
#define KSAS_MAX_ERRORS 20u
#endif
#ifndef KSAS_MAX_SYMBOLS
#define KSAS_MAX_SYMBOLS 65536u
#endif
#ifndef KSAS_MAX_ITEMS
#define KSAS_MAX_ITEMS (1u << 20)
#endif
#ifndef KSAS_MAX_NODES
#define KSAS_MAX_NODES (1u << 21)
#endif
#ifndef KSAS_MAX_LINES
#define KSAS_MAX_LINES (1u << 20)
#endif
#ifndef KSAS_MAX_POOL
#define KSAS_MAX_POOL (16u << 20)        /* bytes de strings + nomes */
#endif
#ifndef KSAS_MAX_SECTION
#define KSAS_MAX_SECTION (16u << 20)     /* .text+.rodata e .data, em bytes */
#endif
#ifndef KSAS_MAX_INCLUDE_DEPTH
#define KSAS_MAX_INCLUDE_DEPTH 16u
#endif
#ifndef KSAS_MAX_EXPR_DEPTH
#define KSAS_MAX_EXPR_DEPTH 64u
#endif
#define KSAS_MAX_NAME 255u                /* u8 len na tabela de símbolos */
#define KSAS_DEFAULT_STACK 256u           /* doc 07: .stack padrão */

/* ---- Seções ----------------------------------------------------------- */
/* .text e .rodata compõem a região CODE (rodata depois de todo o text);
 * .data e .bss compõem a RAM. */
typedef enum ksas_section {
    KSAS_SEC_TEXT,
    KSAS_SEC_RODATA,
    KSAS_SEC_DATA,
    KSAS_SEC_BSS,
    KSAS_SEC_COUNT
} ksas_section;

/* ---- Expressões ------------------------------------------------------- */
typedef enum ksas_node_op {
    KSAS_EN_NUM,     /* a = valor */
    KSAS_EN_SYM,     /* a = índice do símbolo */
    KSAS_EN_DOT,     /* endereço do item que contém a expressão */
    KSAS_EN_NEG, KSAS_EN_CPL,                       /* unários: a */
    KSAS_EN_ADD, KSAS_EN_SUB, KSAS_EN_MUL, KSAS_EN_DIV, KSAS_EN_MOD,
    KSAS_EN_AND, KSAS_EN_OR, KSAS_EN_XOR, KSAS_EN_SHL, KSAS_EN_SHR  /* a, b */
} ksas_node_op;

typedef struct ksas_node {
    uint8_t  op;
    uint32_t col;
    uint32_t a, b;
} ksas_node;

/* ---- Símbolos --------------------------------------------------------- */
typedef enum ksas_sym_kind {
    KSAS_SYM_UNDEF,  /* só referenciado até agora */
    KSAS_SYM_LABEL,
    KSAS_SYM_EQU
} ksas_sym_kind;

typedef struct ksas_symbol {
    uint32_t name;       /* deslocamento em ksas_symtab.names */
    uint8_t  name_len;
    uint8_t  kind;       /* ksas_sym_kind */
    uint8_t  section;    /* LABEL */
    bool     global;
    bool     placed;     /* LABEL: endereço já atribuído pelo layout */
    bool     busy;       /* EQU: em avaliação (detecta ciclo) */
    bool     bad;        /* EQU: erro já reportado */
    uint32_t offset;     /* LABEL: deslocamento dentro da seção */
    uint32_t expr;       /* EQU: nó raiz */
    uint32_t item;       /* item que define (LABEL/EQU) */
    uint32_t line, col;  /* definição, ou primeira referência se UNDEF */
} ksas_symbol;

typedef struct ksas_symtab {
    ksas_symbol *syms;
    uint32_t     count, cap;
    uint32_t    *order;          /* índices em syms ordenados por nome */
    uint32_t     order_cap;
    char        *names;
    uint32_t     names_len, names_cap;
} ksas_symtab;

/* ---- Itens ------------------------------------------------------------ */
typedef enum ksas_item_kind {
    KSAS_IT_LABEL,
    KSAS_IT_EQU,
    KSAS_IT_INSN,
    KSAS_IT_DATA,    /* .byte/.half/.word: lista de expressões */
    KSAS_IT_BYTES,   /* .ascii/.asciz ou string em .byte: bytes no pool */
    KSAS_IT_SPACE,
    KSAS_IT_ALIGN,
    KSAS_IT_ORG
} ksas_item_kind;

/* Como o imediato/deslocamento é validado e codificado. */
typedef enum ksas_imm_kind {
    KSAS_IMM_NONE,
    KSAS_IMM_S14,      /* -8192..8191 */
    KSAS_IMM_U14,      /* 0..16383 (SYSCALL, MFSR, MTSR, PUSHM, POPM) */
    KSAS_IMM_SHIFT,    /* 0..31 (SHLI/SHRI/SARI) */
    KSAS_IMM_32,
    KSAS_IMM_PCREL14,  /* ADR: alvo - PC_next em imm14 */
    KSAS_IMM_PCREL32,  /* ADRL: alvo - PC_next em imm32 */
    KSAS_IMM_BR20,     /* BCOND */
    KSAS_IMM_J24       /* JMP, CALL */
} ksas_imm_kind;

#define KSAS_INSN_VAR      0x01u  /* curta (I) ou longa (L) decidida no layout */
#define KSAS_INSN_LONG_UNK 0x02u  /* valor desconhecido no passo 1 => longa */
#define KSAS_INSN_NEG_LONG 0x04u  /* forma longa usa -imm (SUBI -> ADDL) */
#define KSAS_INSN_NEGATE   0x08u  /* (decidido no layout) emitir -imm */
#define KSAS_INSN_LA       0x10u  /* LA: vira ADRL PC-relativo com .pic */
#define KSAS_INSN_MEMABS   0x20u  /* [expr] sem base: erro sugere "la" */

typedef struct ksas_insn {
    uint8_t  op;         /* opcode KSVM (final após o layout) */
    uint8_t  long_op;    /* forma longa (KSAS_NO_OP se não há) */
    uint8_t  ra, rb, rc;
    uint8_t  cond;
    uint8_t  imm_kind;   /* ksas_imm_kind */
    uint8_t  flags;      /* KSAS_INSN_* */
    uint32_t expr;       /* KSAS_NONE = imediato 0 */
} ksas_insn;

typedef struct ksas_item {
    uint8_t  kind;       /* ksas_item_kind */
    uint8_t  section;    /* ksas_section */
    uint32_t line;       /* índice em ctx->lines */
    uint32_t col;
    uint32_t offset;     /* layout: deslocamento dentro da seção */
    uint32_t size;
    union {
        uint32_t  sym;                                        /* LABEL, EQU */
        ksas_insn insn;
        struct { uint32_t first, count; uint8_t width; } data; /* em ctx->elems */
        struct { uint32_t off, len; } bytes;                   /* em ctx->pool */
        struct { uint32_t count, fill; } space;                /* fill NONE = 0 */
        uint32_t  expr;                                        /* ALIGN, ORG */
    } u;
} ksas_item;

/* ---- Fontes e linhas -------------------------------------------------- */
typedef struct ksas_file {
    const char *name;
    char       *owned_name;   /* liberado no fim (NULL se não é nosso) */
    char       *owned_text;
    bool        listed;       /* false: linhas sintéticas (-D) */
} ksas_file;

typedef struct ksas_line {
    uint32_t    file;
    uint32_t    lineno;
    const char *text;
    uint32_t    len;
    uint32_t    first_item;   /* itens da linha: [first_item, próxima linha) */
} ksas_line;

/* ---- Contexto --------------------------------------------------------- */
typedef struct ksas_ctx {
    const ksas_options *opt;
    unsigned  errors;
    bool      aborted;       /* limite de erros atingido */
    bool      line_failed;   /* já há erro de sintaxe na linha corrente */

    ksas_file *files;  uint32_t nfiles,  files_cap;
    ksas_line *lines;  uint32_t nlines,  lines_cap;
    ksas_item *items;  uint32_t nitems,  items_cap;
    ksas_node *nodes;  uint32_t nnodes,  nodes_cap;
    uint32_t  *elems;  uint32_t nelems,  elems_cap;   /* nós de .byte/.half/.word */
    uint8_t   *pool;   uint32_t npool,   pool_cap;    /* bytes de strings */
    ksas_symtab sym;

    /* Estado do parser */
    uint32_t cur_line;
    uint8_t  section;
    unsigned include_depth;

    /* Diretivas globais */
    bool     pic;
    uint32_t entry_expr, entry_line;
    uint32_t stack_expr, stack_line;

    /* Layout */
    bool     final;          /* passo 2: todos os endereços conhecidos */
    uint32_t layout_next;    /* item em layout (itens <= este já têm offset) */
    uint32_t sec_size[KSAS_SEC_COUNT];
    uint32_t sec_align[KSAS_SEC_COUNT];
    uint32_t sec_base[KSAS_SEC_COUNT];
    uint32_t code_size, data_size, bss_size, ram_base;

    /* Emissão */
    uint8_t *code;
    uint8_t *data;
    uint32_t entry, stack_size;
} ksas_ctx;

/* ---- Utilidades (ctx.c) ----------------------------------------------- */

/* Reporta "arquivo:linha:coluna: erro: ..." para o item/linha "line"
 * (índice em ctx->lines; KSAS_NONE = sem posição). col 0 = sem coluna. */
void ksas_error(ksas_ctx *c, uint32_t line, uint32_t col, const char *fmt, ...);
void ksas_verror(ksas_ctx *c, uint32_t line, uint32_t col, const char *fmt, va_list ap);

/* Erro de sintaxe: só o primeiro de cada linha é reportado (o parser
 * abandona a linha e evita erros em cascata). Devolve false. */
bool ksas_perr(ksas_ctx *c, uint32_t col, const char *fmt, ...);

/* Avança o cursor e reporta TK_ERROR (como erro de sintaxe). */
void ksas_advance(ksas_ctx *c, ksas_cursor *cur);

/* Garante capacidade para "need" elementos de "elem" bytes, até "limit".
 * Devolve o novo ponteiro ou NULL (erro já reportado; data intacto). */
void *ksas_grow(ksas_ctx *c, void *data, uint32_t *cap, uint32_t need,
                size_t elem, uint32_t limit, const char *what);

/* Novo item na seção corrente, ligado à linha corrente. KSAS_NONE se falhou. */
uint32_t ksas_item_new(ksas_ctx *c, ksas_item_kind kind, uint32_t col);

/* Copia bytes para o pool (bytes NULL: reserva zerada); devolve o
 * deslocamento ou KSAS_NONE. */
uint32_t ksas_pool_add(ksas_ctx *c, const uint8_t *bytes, uint32_t len);

/* Nome legível da seção (".text"...). */
const char *ksas_section_name(uint8_t sec);

/* Converte para inteiro com sinal sem comportamento definido pela
 * implementação. */
int32_t ksas_s32(uint32_t v);

#endif /* KSAS_IR_H */
