/*
 * conform.h — executor de UMA instrução para os testes de conformidade
 * (test_conform_*.c) e de registrador zero.
 *
 * Cada caso monta {w0, w1, HALT, HALT} na fixture, preenche os registradores
 * com sentinelas (r2 = a, r3 = b, sp = fim da RAM), define FLAGS, executa um
 * único ksvm_step e confere: status RUNNING, pc = len, insn_count = 1,
 * destino e flags iguais ao oráculo, TODOS os outros registradores e a RAM
 * intactos. Em falha imprime o caso (nota) antes de registrar o erro.
 */
#ifndef KSVM_TEST_CONFORM_H
#define KSVM_TEST_CONFORM_H

#include "fixture.h"
#include "oracle.h"
#include "test.h"

#define CF_RD   1u     /* destino padrão */
#define CF_RA   2u     /* primeiro operando (recebe "a") */
#define CF_RB   3u     /* segundo operando (recebe "b") */
#define CF_NONE 32u    /* instrução sem destino */

/* Valores obrigatórios + vizinhos e padrões de bits. */
extern const uint32_t cf_vals[];
extern const unsigned cf_nvals;
/* Flags de entrada: 0, ZNCV, Z|C, N|V (C = 0 e C = 1 nos dois sentidos). */
extern const uint32_t cf_flag_sets[];
extern const unsigned cf_nflag_sets;

typedef struct cf_case {
    const char *what;   /* nome da instrução, para a nota de falha */
    uint32_t w0, w1;    /* palavra(s) da instrução; w1 só no formato L */
    uint32_t a, b;      /* valores iniciais de r2 e r3 */
    uint32_t fin;       /* FLAGS inicial */
    unsigned rd;        /* destino esperado ou CF_NONE */
    uint32_t v;         /* valor esperado no destino */
    uint32_t fl;        /* FLAGS esperado */
} cf_case;

/* Sentinela inicial do registrador i (1..30); sp = FIX_RAM_END. */
uint32_t cf_sentinel(unsigned i);

/* Monta a fixture para o caso (sem executar). */
void cf_prepare(fixture *f, uint32_t w0, uint32_t w1, uint32_t a, uint32_t b, uint32_t fin);

/* Executa e confere um caso que deve completar normalmente. */
void cf_run(test_ctx *t_, const cf_case *c);

/* Executa e confere um caso que deve entrar em fault sem efeito colateral
 * (cpu, insn_count e RAM idênticos; pc na instrução). */
void cf_run_fault(test_ctx *t_, const cf_case *c, ksvm_fault fault, uint32_t addr);

/* Comprimento de uma instrução pelo opcode (4 ou 8). */
unsigned cf_len(uint32_t w0);

#endif /* KSVM_TEST_CONFORM_H */

/* ---- Verificação em massa de operações binárias/unárias --------------- */

enum { CF_FMT_R, CF_FMT_I, CF_FMT_L };

#define CF_DIV 1u   /* divisor 0 -> KSVM_FAULT_DIV_ZERO sem efeito */
#define CF_CMP 2u   /* sem destino: campos ra, rb|imm (CMP*, TST*) */

/* Oráculo: resultado e flags finais a partir de a, b e FLAGS de entrada. */
typedef o_res (*cf_bin_fn)(uint32_t a, uint32_t b, uint32_t fin);

/* Imediatos de 14 bits padrão: 0, ±1, limites, contagens de shift... */
extern const int32_t cf_imms[];
extern const unsigned cf_nimms;

typedef struct cf_bin {
    const char     *name;
    uint8_t         op;
    int             fmt;       /* CF_FMT_* */
    cf_bin_fn       fn;
    unsigned        opts;      /* CF_DIV | CF_CMP */
    const uint32_t *bset;      /* valores de b (R/L); NULL = cf_vals */
    unsigned        nb;
    const int32_t  *iset;      /* imediatos (I); NULL = cf_imms */
    unsigned        ni;
} cf_bin;

/* Todas as combinações a x b x FLAGS de entrada. R: "rd, ra, rb" nos campos
 * ra=1, rb=2, rc=3; I: "rd, ra, imm"; L: "rd, ra, imm32". */
void cf_check_binary(test_ctx *t_, const cf_bin *d);

/* Formato R: aliasing rd=ra, rd=rb, ra=rb, rd=ra=rb e zr como fonte. */
void cf_check_aliasing(test_ctx *t_, const cf_bin *d);

/* "rd, rb" (campos ra, rb): rd = fn(b). */
typedef o_res (*cf_un_fn)(uint32_t b, uint32_t fin);
void cf_check_unary(test_ctx *t_, const char *name, uint8_t op, cf_un_fn fn);
