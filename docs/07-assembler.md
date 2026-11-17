# 07 — Assembler (`ksas`) e disassembler (`ksdis`)

Ambos em C, ambos usando a tabela da ISA gerada de `isa.def`. O assembler Python da v1 é
descartado: a sintaxe muda e manter duas descrições da ISA foi a principal fonte de
inconsistência.

## 1. Sintaxe

```asm
; comentário até o fim da linha      (também aceita '#' e '//')
        .text
        .global _start               ; símbolo exportado para a tabela
_start:
        li      r1, 0                ; pseudo: MOVI ou MOVIL conforme o valor
        la      r2, msg              ; pseudo: MOVIL r2, msg
loop:
        ldb     r3, [r2]             ; [rb] == [rb + 0]; loads NÃO alteram flags
        cmpi    r3, 0
        beq     done
        syscall 0x10                 ; PUTC r1  (o assembler aceita o nome: syscall PUTC)
        addi    r2, r2, 1
        b       loop
done:
        halt

        .data
msg:    .asciz  "Hello, KSVM!\n"
buf:    .space  64
        .align  4
tbl:    .word   1, 2, 3, 0xFFFF_FFFF
        .half   7, 8
        .byte   'A', 0x41, 65
```

Regras:
- Mnemônicos e registradores são *case-insensitive*; labels são *case-sensitive*.
- Operandos separados por vírgula. Memória: `[rb]`, `[rb + imm]`, `[rb - imm]`, `[rb + rc]`,
  `[label]` (= `[zr + label]` se label < 8192, senão erro orientando a usar `la`).
- Números: decimal, `0x` hex, `0b` binário, `'c'` caractere, `_` como separador visual,
  `-` unário. Expressões: `+ - * / % & | ^ << >> ( )` sobre inteiros e labels, avaliadas
  em 32 bits com wrap. `label - outro_label` é permitido (diferença constante).
- Registradores: `zr`, `r0`–`r31`, `fp`, `sp`. Opcional: aliases `a0–a3` (= r1–r4),
  `t0–t10` (= r5–r15), `s0–s13` (= r16–r29).
- Condições de desvio: `beq bne bhs/bcs blo/bcc bmi bpl bvs bvc bhi bls bge blt bgt ble bal`
  e também `b.eq`, `b.ne`, …

### Diretivas
| Diretiva | Efeito |
|----------|--------|
| `.text` / `.rodata` / `.data` / `.bss` | Seleciona seção. `.rodata` vai junto com `.text` na região CODE |
| `.global sym` | Marca símbolo na tabela do `.ksx` (debug) |
| `.equ nome, expr` / `nome = expr` | Constante simbólica |
| `.byte` `.half` `.word` | Dados de 8/16/32 bits (listas) |
| `.ascii "…"` / `.asciz "…"` | String sem / com `\0`. Escapes `\n \t \r \0 \\ \" \xHH` |
| `.space n [, fill]` | `n` bytes (em `.bss` só reserva) |
| `.align n` | Alinha a `n` bytes (potência de 2) |
| `.org addr` | Posição absoluta dentro da seção (só para frente) |
| `.entry sym` | Define `entry` do cabeçalho (padrão: `_start`, senão 0) |
| `.stack n` | `stack_size` do cabeçalho (padrão 256) |
| `.include "arquivo"` | Inclusão textual |
| `.pic` | Transforma `la` em `ADRL` PC-relativo e marca flag PIC |

Macros (`.macro`/`.endm` com parâmetros) ficam para a fase 5.

## 2. Arquitetura do `ksas`

```
fonte.ksasm ──► lexer ──► parser ──► IR (lista de itens por seção) ──► passo 1: layout
                                                                     ──► passo 2: emissão
                                                                     ──► writer .ksx
```

| Módulo | Responsabilidade |
|--------|------------------|
| `lexer.c` | Texto → tokens (`IDENT`, `NUM`, `STR`, `CHAR`, `REG`, `LBRACK`, `COMMA`, `PLUS`, …, `NEWLINE`, `EOF`). Guarda linha/coluna |
| `parser.c` | Tokens → `asm_item` (`LABEL`, `INSN`, `DIRECTIVE`). Faz a validação sintática por formato usando a tabela da ISA (quantos operandos, de que tipo) |
| `expr.c` | Avaliador de expressões com precedência; referências a símbolos ainda não definidos ficam pendentes |
| `symtab.c` | Tabela de símbolos (array ordenado + busca binária; sem `malloc` obrigatório — buffers com limite configurável e erro claro ao estourar) |
| `layout.c` | Passo 1: calcula endereço de cada item. Como toda instrução tem tamanho fixo pelo opcode, **as pseudo-instruções são a única fonte de tamanho variável**: `li` decide 4 ou 8 bytes se o valor já é conhecido; se depende de label não resolvido, assume a forma longa (8 bytes). Isso elimina o problema de relaxamento iterativo |
| `emit.c` | Passo 2: codifica cada instrução (`encode_R/I/L/B/J`), verifica alcance de imediatos e desvios, gera erro com linha |
| `ksx_writer.c` | Monta cabeçalho, seções, tabela de símbolos, checksum |
| `main.c` | CLI: `ksas in.ksasm -o out.ksx [-l listing.txt] [-g] [-DNAME=val]` |

Erros no formato `arquivo:linha:coluna: erro: mensagem` (compatível com editores).
O listing (`-l`) imprime endereço, bytes e fonte lado a lado — substitui o
`vm_dump_program` da v1 como ferramenta de inspeção estática.

## 3. Codificadores (compartilhados com testes)

```c
uint32_t ksvm_enc_r(uint8_t op, uint8_t ra, uint8_t rb, uint8_t rc);
uint32_t ksvm_enc_i(uint8_t op, uint8_t ra, uint8_t rb, int32_t imm14);
uint32_t ksvm_enc_b(uint8_t cond, int32_t off_bytes);
uint32_t ksvm_enc_j(uint8_t op, int32_t off_bytes);
/* formato L = enc_i(op, ra, rb, 0) seguido de imm32 */
```
Vivem em `src/core/encode.c` (opcional no núcleo, usado pelos testes para montar programas
em C sem passar pelo assembler).

## 4. Disassembler `ksdis`

- Lê `.ksx`, imprime cabeçalho, depois cada seção.
- Para CODE: percorre de 4 em 4 bytes; o tamanho de cada instrução sai da tabela
  (`len = 4 ou 8`). Não há ambiguidade nem necessidade de "ressincronizar" como na v1.
- Usa a tabela de símbolos (se presente) para mostrar `call print` em vez de `call +0x40`.
- Saída reaproveitável pelo `ksas` (round-trip: `ksas → ksdis → ksas` produz o mesmo binário;
  isso é um dos golden tests).
- A função central `ksvm_disasm_one(uint32_t pc, uint32_t w, uint32_t imm32, char *buf, size_t n)`
  é usada também pelo módulo de trace do runner.

## 5. Runner `ksvm`

```
ksvm prog.ksx [--ram N] [--trace] [--step] [--regs] [--max-insns N] [--no-fs] [-- args...]
```
- `--trace`: imprime `pc: bytes  mnemônico  (flags)` por instrução.
- `--step`: trace + espera Enter (sem `system("clear")`, sem cores obrigatórias).
- `--regs`: dump de registradores ao final ou a cada passo.
- Código de saída do processo = `exit_code` do guest; faults imprimem
  `fault: NOME at pc=0x… addr=0x…` e retornam 70+código.

## 6. `ksimg`

`ksimg prog.ksx -o program_image.h [--name ksvm_image] [--progmem]` gera o array C.
