/*
 * disasm.h — disassembler de uma instrução (doc 07 §4), usado pelo ksdis e
 * pelo trace do runner. Sem stdio: formatação própria em buffer.
 *
 * A saída é sintaxe que o ksas aceita (doc 07 §1), em minúsculas:
 *     addi r2, r1, 1        ldw r3, [r2 + 8]      stw [sp - 4], r1
 *     beq +0x40             call print            syscall 0x10
 *     movil r2, 0x1000      cmp r1, r2            ret
 * Regras:
 *   - Registradores: zr, r1..r29, fp, sp.
 *   - imm14 em decimal com sinal, exceto: o número do registrador especial
 *     de MFSR/MTSR, em decimal SEM sinal (0..16383, como o ksas exige), e
 *     SYSCALL/PUSHM/POPM, em hex sem sinal (número/máscara de 14 bits);
 *     imm32 (formato L) em hex sem sinal.
 *   - Desvios e ADR/ADRL: nome do alvo (se o resolvedor de símbolos o
 *     conhecer) ou deslocamento relativo a PC_next, com sinal: "+0x40",
 *     "-0x8". O alvo absoluto sai de ksvm_disasm_target.
 *   - Palavra que não é a codificação canônica de nenhuma instrução
 *     (opcode inválido, BCOND com cond NV, campos não usados != 0,
 *     SHLI/SHRI/SARI com contagem > 31, que o ksas não aceita) vira
 *     ".word 0x........" (formato L: ".word 0x........, 0x........"), o que
 *     preserva o round-trip ksas -> ksdis -> ksas bit a bit.
 */
#ifndef KSVM_DISASM_H
#define KSVM_DISASM_H

#include "types.h"

#if KSVM_ENABLE_DISASM

/* Tamanho recomendado de buffer (a maior saída possível cabe com folga). */
#define KSVM_DISASM_BUF 64u

/* Resolvedor de símbolos: nome do endereço "addr" do guest ou NULL. */
typedef const char *(*ksvm_disasm_sym_fn)(void *ctx, uint32_t addr);

/* Tamanho em bytes da instrução cuja primeira palavra é w: 8 no formato L,
 * 4 nos demais (inclusive opcode inválido, mostrado como .word). */
unsigned ksvm_disasm_len(uint32_t w);

/*
 * Desmonta a instrução em "pc" (primeira palavra w; imm32 = palavra seguinte,
 * usada só no formato L) para buf[0..n), sempre terminado em '\0' se n > 0;
 * saída longa demais é truncada. Devolve o tamanho da instrução (4 ou 8).
 */
unsigned ksvm_disasm_one(uint32_t pc, uint32_t w, uint32_t imm32, char *buf, size_t n);

/* Idem, consultando sym(ctx, alvo) nos desvios, CALL, JMP, ADR e ADRL. */
unsigned ksvm_disasm_one_ex(uint32_t pc, uint32_t w, uint32_t imm32,
                            ksvm_disasm_sym_fn sym, void *ctx, char *buf, size_t n);

/* Alvo absoluto (PC_next + deslocamento) de BCOND, JMP, CALL, ADR e ADRL
 * canônicos; false para as demais instruções. */
bool ksvm_disasm_target(uint32_t pc, uint32_t w, uint32_t imm32, uint32_t *target);

#endif /* KSVM_ENABLE_DISASM */

#endif /* KSVM_DISASM_H */
