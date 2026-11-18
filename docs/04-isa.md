# 04 — ISA KSVM v2 (conjunto de instruções)

Versão da ISA: **2.0**. Guest little-endian, 32 bits.

## 1. Registradores

| Nome | Índice | Papel | Convenção de chamada (ABI sugerida) |
|------|--------|-------|-------------------------------------|
| `zr` | R0 | Sempre 0. Escritas descartadas | — |
| `r1`–`r4` | R1–R4 | Gerais | Argumentos 1–4 e valor de retorno em `r1` (`r2` = segundo retorno / errno de syscall) |
| `r5`–`r15` | R5–R15 | Gerais | *Caller-saved* (temporários) |
| `r16`–`r29` | R16–R29 | Gerais | *Callee-saved* |
| `fp` | R30 | Frame pointer | *Callee-saved* |
| `sp` | R31 | Stack pointer | Cresce para baixo, sempre múltiplo de 4 |

Registradores especiais (fora do banco, acessados por `MFSR`/`MTSR`):

| SR | Nome | Acesso |
|----|------|--------|
| 0 | `PC` | Leitura (`MFSR`); escrita via `JMPR` |
| 1 | `FLAGS` | Leitura/escrita |
| 2 | `ISA_VERSION` | Leitura: `0x0200` |
| 3 | `INSN_COUNT` | Leitura |

A ABI é convenção do assembler/compilador; a VM não a impõe (exceto `zr`, `sp` em
PUSH/POP/CALL/RET).

## 2. Flags

| Bit | Flag | Significado |
|-----|------|-------------|
| 0 | `Z` | Resultado zero |
| 1 | `N` | Bit 31 do resultado (negativo em complemento de dois) |
| 2 | `C` | Carry (ADD) / **não-borrow** (SUB, CMP: `C=1` se `a >= b` sem sinal). Último bit deslocado para fora (shifts) |
| 3 | `V` | Overflow com sinal |
| 4–31 | — | Reservado, lido como 0 |

Convenção de `C` em subtração é a do ARM (não a do x86), o que faz as condições
`HS/LO/HI/LS` saírem direto das flags. `FCMP` (extensão F, §5.7) também segue o ARM (VFP):
menor = N, igual = ZC, maior = C, unordered = CV.

## 3. Formatos de codificação

Toda instrução tem 4 bytes (uma palavra), exceto o formato **L** que tem 8 bytes
(palavra + imediato de 32 bits). O comprimento depende **só do opcode**. Palavras são
armazenadas little-endian; o PC deve ser múltiplo de 4.

```
bit:   31                    23 22      18 17      13 12       8 7         0
      ┌────────────────────────┬──────────┬──────────┬──────────┬───────────┐
R     │ func9 (0)              │ rc       │ rb       │ ra       │ opcode    │
      ├────────────────────────┴──────────┼──────────┼──────────┼───────────┤
I     │ imm14 (com sinal)                 │ rb       │ ra       │ opcode    │
      ├───────────────────────────────────┼──────────┼──────────┼───────────┤
L     │ reservado (0)                     │ rb       │ ra       │ opcode    │ + imm32 (palavra seguinte)
      ├───────────────────────────────────┴──────────┴───┬──────┼───────────┤
B     │ off20 (com sinal, em palavras)                   │ cond4│ opcode    │
      ├──────────────────────────────────────────────────┴──────┼───────────┤
J     │ off24 (com sinal, em palavras)                          │ opcode    │
      └─────────────────────────────────────────────────────────┴───────────┘
```

| Formato | Campos | Alcance do imediato |
|---------|--------|---------------------|
| R | ra, rb, rc | — |
| I | ra, rb, imm14 | −8192 … +8191 |
| L | ra, rb, imm32 | 32 bits completos |
| B | cond, off20 | ±512 K palavras = ±2 MiB, relativo a `PC+4` |
| J | off24 | ±8 M palavras = ±32 MiB, relativo a `PC+4` |

Extração dos campos (sem UB, só máscaras e shifts):
```c
op   =  w        & 0xFF;
ra   = (w >>  8) & 0x1F;
rb   = (w >> 13) & 0x1F;
rc   = (w >> 18) & 0x1F;
imm14 = sext14(w >> 18);          /* (int32_t)(w & 0xFFFC0000) >> 18 — ver bytes.h */
cond = (w >>  8) & 0x0F;
off20 = sext20(w >> 12) * 4;
off24 = sext24(w >>  8) * 4;
```
Desvios relativos em palavras dão 4× mais alcance que em bytes e garantem alinhamento.

## 4. Códigos de condição (campo `cond` de `Bcc`)

| cond | Sufixo | Testa | Significado após `CMP a, b` |
|------|--------|-------|-----------------------------|
| 0 | `AL` | sempre | incondicional |
| 1 | `EQ` | Z=1 | a == b |
| 2 | `NE` | Z=0 | a != b |
| 3 | `HS`/`CS` | C=1 | a >= b (unsigned) |
| 4 | `LO`/`CC` | C=0 | a < b (unsigned) |
| 5 | `MI` | N=1 | resultado negativo |
| 6 | `PL` | N=0 | resultado não negativo |
| 7 | `VS` | V=1 | overflow |
| 8 | `VC` | V=0 | sem overflow |
| 9 | `HI` | C=1 ∧ Z=0 | a > b (unsigned) |
| 10 | `LS` | C=0 ∨ Z=1 | a <= b (unsigned) |
| 11 | `GE` | N=V | a >= b (signed) |
| 12 | `LT` | N≠V | a < b (signed) |
| 13 | `GT` | Z=0 ∧ N=V | a > b (signed) |
| 14 | `LE` | Z=1 ∨ N≠V | a <= b (signed) |
| 15 | `NV` | nunca | reservado (comporta-se como NOP) |

## 5. Lista de instruções

Notação: `rd` destino, `ra`/`rb` fontes, `imm` imediato com sinal estendido para 32 bits,
`[x]` memória no endereço `x`. `→` indica flags afetadas. Operações aritméticas são módulo 2³².

### 5.1 Movimentação e constantes

| Opcode | Mnemônico | Fmt | Semântica | Flags |
|--------|-----------|-----|-----------|-------|
| 0x00 | `NOP` | R | — | — |
| 0x01 | `MOV rd, rb` | R | rd = rb | — |
| 0x02 | `MOVI rd, imm14` | I | rd = imm | — |
| 0x03 | `MOVIL rd, imm32` | L | rd = imm32 | — |
| 0x04 | — | — | **Removido** (era `MOVHI`; redundante com `MOVIL`). Opcode inválido → `FAULT_BAD_OPCODE` | — |
| 0x05 | `ADR rd, imm14` | I | rd = PC_next + imm (PC-relativo curto) | — |
| 0x06 | `ADRL rd, imm32` | L | rd = PC_next + imm32 (PC-relativo longo; código independente de posição) | — |
| 0x07 | `MFSR rd, sr` | I | rd = registrador especial `imm`. `MFSR rd, PC` devolve o endereço **da própria instrução MFSR** (não `PC_next`) | — |
| 0x08 | `MTSR sr, rb` | I | registrador especial `imm` = rb (só FLAGS gravável) | (FLAGS) |

### 5.2 Load / store

Endereço efetivo: `EA = rb + imm14` (formato I) ou `EA = rb + rc` (formato R, sufixo `X`).
Sem exigência de alinhamento no guest (o barramento lê byte a byte).

| Opcode | Mnemônico | Fmt | Semântica |
|--------|-----------|-----|-----------|
| 0x10 | `LDB rd, [rb+imm]` | I | rd = zext8([EA]) |
| 0x11 | `LDBS rd, [rb+imm]` | I | rd = sext8([EA]) |
| 0x12 | `LDH rd, [rb+imm]` | I | rd = zext16([EA]) |
| 0x13 | `LDHS rd, [rb+imm]` | I | rd = sext16([EA]) |
| 0x14 | `LDW rd, [rb+imm]` | I | rd = [EA] (32 bits) |
| 0x15 | `STB [rb+imm], ra` | I | [EA] = ra & 0xFF |
| 0x16 | `STH [rb+imm], ra` | I | [EA] = ra & 0xFFFF |
| 0x17 | `STW [rb+imm], ra` | I | [EA] = ra |
| 0x18 | `LDBX rd, [rb+rc]` | R | idem LDB, índice em registrador |
| 0x19 | `LDBSX rd, [rb+rc]` | R | |
| 0x1A | `LDHX rd, [rb+rc]` | R | |
| 0x1B | `LDHSX rd, [rb+rc]` | R | |
| 0x1C | `LDWX rd, [rb+rc]` | R | |
| 0x1D | `STBX [rb+rc], ra` | R | |
| 0x1E | `STHX [rb+rc], ra` | R | |
| 0x1F | `STWX [rb+rc], ra` | R | |

Nos stores, `ra` é a fonte (mesmo campo de bits do `rd` nos loads), mantendo a
codificação uniforme. Falhas: endereço fora de região → `FAULT_MEM_RANGE`; escrita em ROM
→ `FAULT_MEM_PERM`.

### 5.3 Pilha

| Opcode | Mnemônico | Fmt | Semântica |
|--------|-----------|-----|-----------|
| 0x20 | `PUSH ra` | R | sp -= 4; [sp] = ra. `PUSH sp` empilha o valor **antigo** de sp |
| 0x21 | `POP rd` | R | rd = [sp]; sp += 4. `POP sp` termina com sp = valor lido (o `+= 4` não se aplica) |
| 0x22 | `PUSHM imm14` | I | empilha r16..r29/fp conforme máscara de 14 bits (prólogo compacto) — **opcional**, fase 5 |
| 0x23 | `POPM imm14` | I | inverso de PUSHM — **opcional**, fase 5 |

`PUSH`/`CALL` verificam `sp - 4 >= stack_limit` quando `KSVM_CHECK_STACK=1`.

### 5.4 Aritmética inteira

| Opcode | Mnemônico | Fmt | Semântica | Flags |
|--------|-----------|-----|-----------|-------|
| 0x30 | `ADD rd, ra, rb` | R | rd = ra + rb | ZNCV |
| 0x31 | `ADDI rd, ra, imm14` | I | rd = ra + imm | ZNCV |
| 0x32 | `ADDL rd, ra, imm32` | L | rd = ra + imm32 | ZNCV |
| 0x33 | `SUB rd, ra, rb` | R | rd = ra − rb | ZNCV |
| 0x34 | `SUBI rd, ra, imm14` | I | rd = ra − imm | ZNCV |
| 0x35 | `ADC rd, ra, rb` | R | rd = ra + rb + C | ZNCV |
| 0x36 | `SBC rd, ra, rb` | R | rd = ra − rb − (1−C) | ZNCV |
| 0x37 | `NEG rd, rb` | R | rd = 0 − rb | ZNCV |
| 0x38 | `CMP ra, rb` | R | flags(ra − rb), sem destino | ZNCV |
| 0x39 | `CMPI ra, imm14` | I | flags(ra − imm) | ZNCV |
| 0x3A | `CMPL ra, imm32` | L | flags(ra − imm32) | ZNCV |
| 0x3B | `MUL rd, ra, rb` | R | rd = (ra × rb) mod 2³² | ZN |
| 0x3C | `MULI rd, ra, imm14` | I | | ZN |
| 0x3D | `MULH rd, ra, rb` | R | rd = bits 63..32 de (int64)ra × (int64)rb | ZN |
| 0x3E | `MULHU rd, ra, rb` | R | rd = bits 63..32 de (uint64)ra × (uint64)rb | ZN |
| 0x3F | `DIV rd, ra, rb` | R | rd = ra ÷ rb (com sinal, trunca para zero) | ZN |
| 0x40 | `DIVU rd, ra, rb` | R | rd = ra ÷ rb (sem sinal) | ZN |
| 0x41 | `REM rd, ra, rb` | R | rd = ra mod rb (sinal do dividendo) | ZN |
| 0x42 | `REMU rd, ra, rb` | R | rd = ra mod rb (sem sinal) | ZN |

`DIV*/REM*` com `rb = 0` → `FAULT_DIV_ZERO`. `DIV` de `INT32_MIN / −1` → resultado
`INT32_MIN`, sem fault (evita UB no host: tratado explicitamente antes de dividir).
`MULH`/`MULHU` usam `uint64_t` no host; em AVR o compilador gera a rotina de 64 bits
(ou desligue com `KSVM_ENABLE_MULDIV=0`).

### 5.5 Lógica e deslocamentos

| Opcode | Mnemônico | Fmt | Semântica | Flags |
|--------|-----------|-----|-----------|-------|
| 0x50 | `AND rd, ra, rb` | R | rd = ra & rb | ZN |
| 0x51 | `ANDI rd, ra, imm14` | I | | ZN |
| 0x52 | `ANDL rd, ra, imm32` | L | | ZN |
| 0x53 | `OR rd, ra, rb` | R | rd = ra \| rb | ZN |
| 0x54 | `ORI rd, ra, imm14` | I | | ZN |
| 0x55 | `ORL rd, ra, imm32` | L | | ZN |
| 0x56 | `XOR rd, ra, rb` | R | rd = ra ^ rb | ZN |
| 0x57 | `XORI rd, ra, imm14` | I | | ZN |
| 0x58 | `XORL rd, ra, imm32` | L | | ZN |
| 0x59 | `NOT rd, rb` | R | rd = ~rb | ZN |
| 0x5A | `SHL rd, ra, rb` | R | rd = ra << (rb & 31) | ZNC |
| 0x5B | `SHLI rd, ra, imm14` | I | rd = ra << (imm & 31) | ZNC |
| 0x5C | `SHR rd, ra, rb` | R | lógico à direita | ZNC |
| 0x5D | `SHRI rd, ra, imm14` | I | | ZNC |
| 0x5E | `SAR rd, ra, rb` | R | aritmético à direita (replica bit 31) | ZNC |
| 0x5F | `SARI rd, ra, imm14` | I | | ZNC |
| 0x60 | `ROL rd, ra, rb` | R | rotação à esquerda | ZNC |
| 0x61 | `ROR rd, ra, rb` | R | rotação à direita | ZNC |
| 0x62 | `TST ra, rb` | R | flags(ra & rb) | ZN |
| 0x63 | `TSTI ra, imm14` | I | flags(ra & imm) | ZN |
| 0x64 | `SXB rd, rb` | R | rd = sext8(rb) | ZN |
| 0x65 | `SXH rd, rb` | R | rd = sext16(rb) | ZN |
| 0x66 | `ZXB rd, rb` | R | rd = rb & 0xFF | ZN |
| 0x67 | `ZXH rd, rb` | R | rd = rb & 0xFFFF | ZN |
| 0x68 | `CLZ rd, rb` | R | zeros à esquerda (32 se rb=0) — `KSVM_ENABLE_BITOPS` | ZN |
| 0x69 | `CTZ rd, rb` | R | zeros à direita (32 se rb=0) | ZN |
| 0x6A | `POPCNT rd, rb` | R | bits em 1 | ZN |

`SAR` é implementado sem depender do comportamento de `>>` em `int32_t` negativo
(implementation-defined em C): usa máscara explícita.
Shift por `>= 32` não existe: a contagem é mascarada com `& 31` (como ARM/x86).
Com contagem efetiva 0 (`n & 31 == 0`) o valor não muda, Z/N refletem o valor e **`C = 0`**
(nenhum bit saiu); vale para `SHL/SHR/SAR/ROL/ROR` e as formas `I`.

### 5.6 Controle de fluxo

| Opcode | Mnemônico | Fmt | Semântica |
|--------|-----------|-----|-----------|
| 0x70 | `JMP off24` | J | pc = PC_next + off |
| 0x71 | `JMPR rb` | R | pc = rb (deve ser múltiplo de 4) |
| 0x72 | `Bcc off20` | B | if cond(flags): pc = PC_next + off. Mnemônicos: `BEQ BNE BHS BLO BMI BPL BVS BVC BHI BLS BGE BLT BGT BLE BAL` |
| 0x73 | `CALL off24` | J | sp −= 4; [sp] = PC_next; pc = PC_next + off |
| 0x74 | `CALLR rb` | R | sp −= 4; [sp] = PC_next; pc = rb (`rb` lido **antes** do ajuste de sp) |
| 0x75 | `RET` | R | pc = [sp]; sp += 4 |
| 0x76 | `HALT` | R | status = HALTED; exit_code = r1; chama `hal->on_halt` |
| 0x77 | `TRAP imm14` | I | status = FAULT, fault = `FAULT_TRAP`, fault_addr = imm (assert/abort do programa) |
| 0x78 | `SYSCALL imm14` | I | chama o host com número `imm` (args r1–r6, retorno r1, erro r2). Ver doc 06 |
| 0x79 | `BRK` | R | ponto de parada: se trace habilitado, chama o hook e continua; senão NOP |
| 0x7A | `WFI` | R | *wait for interrupt*: status = YIELD (devolve controle ao host; retomado no próximo `ksvm_run`). Base para interrupções na fase 5 |

`PC_next` é o endereço da instrução seguinte (`pc + 4`, ou `pc + 8` em formato L).

**Limites do fetch.** Toda a instrução (as duas palavras, no formato L) deve estar dentro de
uma única região com permissão EXEC, e `PC_next` não pode ultrapassar `0xFFFFFFFF`. Caso
contrário o fetch gera `FAULT_PC_RANGE` antes de executar qualquer coisa. Não há wrap do PC.

**Bits reservados.** O campo `func9` do formato R e os bits 18–31 da primeira palavra do
formato L devem ser zero. A v2.0 **ignora** esses bits (não gera fault); versões futuras podem
dar-lhes significado, então assembladores devem sempre emiti-los zerados.
Saltos para endereços desalinhados ou fora da região EXEC → fault no **próximo** fetch.

### 5.7 Extensão F (ponto flutuante binary32)

Opcional, atrás de `KSVM_ENABLE_FLOAT` (padrão 1 no desktop e no Cortex-M; **0 na porta
AVR**, que não comporta o soft-float na meta de 12 KB, doc 08 §4.2). Um programa descobre se
a VM tem a extensão por `CAPS(0x4000)`, bit 2 (doc 06 §3).

**Representação.** Um float é um **IEEE 754 binary32** guardado como padrão de bits num
registrador inteiro comum `rN` (sinal no bit 31, expoente nos bits 30..23, fração nos
22..0). Não há banco de registradores separado, `double` nem pares de registradores:
`MOV`, `LDW`/`STW`, `PUSH`/`POP` e a convenção de chamada (doc 04 §1) valem sem mudança.
Constantes vêm de `fli rd, 1.5` (= `MOVIL rd, 0x3FC00000`) ou de `.float` na memória
(doc 07).

**Operandos** seguem a convenção de campos das inteiras (topo de `isa.def`): três operandos
`rd, ra, rb` nos campos `ra, rb, rc`; dois operandos `rd, rb` nos campos `ra, rb` (como
`NEG`/`NOT`; `fneg rd` = `fneg rd, rd`); `FCMP ra, rb` sem destino, como `CMP`. Todas são
formato R, 4 bytes; não há forma com imediato.

| Opcode | Mnemônico | Fmt | Semântica | Flags |
|--------|-----------|-----|-----------|-------|
| 0xA0 | `FADD rd, ra, rb` | R | rd = ra + rb | — |
| 0xA1 | `FSUB rd, ra, rb` | R | rd = ra − rb | — |
| 0xA2 | `FMUL rd, ra, rb` | R | rd = ra × rb | — |
| 0xA3 | `FDIV rd, ra, rb` | R | rd = ra ÷ rb; `x ÷ ±0` = ±inf (x ≠ 0), sem fault | — |
| 0xA4 | `FMIN rd, ra, rb` | R | menor; um NaN → o outro operando; `FMIN(−0, +0) = −0` | — |
| 0xA5 | `FMAX rd, ra, rb` | R | maior; um NaN → o outro operando; `FMAX(−0, +0) = +0` | — |
| 0xA6 | `FSQRT rd, rb` | R | rd = √rb; `√(−0) = −0`; negativo (inclusive −inf) → NaN | — |
| 0xA7 | `FNEG rd, rb` | R | rd = rb ^ 0x80000000 (só o sinal, inclusive em NaN) | — |
| 0xA8 | `FABS rd, rb` | R | rd = rb & 0x7FFFFFFF (só o sinal, inclusive em NaN) | — |
| 0xA9 | `FRINT rd, rb` | R | inteiro mais próximo, empate para o par (resultado float) | — |
| 0xAA | `FFLOOR rd, rb` | R | inteiro em direção a −inf (float) | — |
| 0xAB | `FCEIL rd, rb` | R | inteiro em direção a +inf (float) | — |
| 0xAC | `FTRUNC rd, rb` | R | inteiro em direção a zero (float) | — |
| 0xAD | `FTOI rd, rb` | R | (int32) rb: trunca para zero, satura em `INT32_MIN/MAX`; NaN → 0 | — |
| 0xAE | `FTOU rd, rb` | R | (uint32) rb: trunca para zero, satura em `0/UINT32_MAX`; NaN → 0 | — |
| 0xAF | `ITOF rd, rb` | R | (float) de rb com sinal, RNE | — |
| 0xB0 | `UTOF rd, rb` | R | (float) de rb sem sinal, RNE | — |
| 0xB1 | `FCMP ra, rb` | R | compara ra com rb, sem destino (tabela abaixo) | ZNCV |

**Regras comuns** (idênticas em todos os hosts, bit a bit):

- **Arredondamento** sempre para o mais próximo, empate para o par (RNE). Não há registrador
  de controle, modos de arredondamento, exceções IEEE nem flags sticky; **nenhuma instrução
  F gera fault** (só `FAULT_BAD_OPCODE` com a extensão desligada).
- **Subnormais** são suportados na entrada e na saída (sem flush-to-zero); underflow dá o
  subnormal (ou zero) corretamente arredondado, overflow dá ±inf.
- **NaN canônico.** Todo NaN produzido ou propagado por `FADD FSUB FMUL FDIV FSQRT FRINT
  FFLOOR FCEIL FTRUNC` e por `FMIN/FMAX` com os dois operandos NaN é exatamente
  **`0x7FC00000`** (sinal 0, NaN quieto, payload 0), qualquer que seja o NaN de entrada
  (quieto ou sinalizador, com qualquer sinal e payload) — como no RISC-V. Isso garante o
  mesmo padrão de bits em qualquer host. Operações inválidas dão o NaN canônico:
  `inf − inf`, `0 × inf`, `0 ÷ 0`, `inf ÷ inf` e a raiz de negativo.
- **Zeros**: `x − x = +0`; `+0 + −0 = +0`; `−0 + −0 = −0`; o sinal de produtos e quocientes
  é o XOR dos sinais. `FRINT/FFLOOR/FCEIL/FTRUNC` preservam o sinal de zero
  (`FCEIL(−0.5) = −0`, `FRINT(−0.5) = −0`, `FTRUNC(−0.7) = −0`), devolvem ±inf como está e
  NaN como o canônico; |x| ≥ 2²³ já é inteiro.
- **Flags**: só `FCMP` altera FLAGS (as quatro, sempre); as demais preservam ZNCV.
- **Conversões**: `FTOI` satura `≥ 2³¹` (inclusive +inf) em `0x7FFFFFFF` e `≤ −2³¹`
  (inclusive −inf) em `0x80000000`; `FTOU` dá 0 para todo negativo (−0.5 trunca para 0,
  −1 satura em 0) e `0xFFFFFFFF` a partir de 2³²; NaN dá 0 em ambas. `ITOF/UTOF` arredondam
  RNE: `ITOF(16777217) = 16777216.0`, `UTOF(0xFFFFFFFF) = 4294967296.0` (`0x4F800000`).

**FCMP** segue a convenção do ARM VFP, para que os `Bcc` existentes sirvam sem
instruções novas:

| Relação de `FCMP a, b` | N | Z | C | V | FLAGS |
|------------------------|---|---|---|---|-------|
| a < b | 1 | 0 | 0 | 0 | `0x2` |
| a == b (inclusive `+0 == −0`) | 0 | 1 | 1 | 0 | `0x5` |
| a > b | 0 | 0 | 1 | 0 | `0x4` |
| unordered (algum operando NaN) | 0 | 0 | 1 | 1 | `0xC` |

Mapeamento para o compilador (resultado exatamente o do C, inclusive com NaN, onde só `!=`
é verdadeiro):

| C | Desvio depois de `FCMP a, b` | Verdadeiro quando |
|---|------------------------------|-------------------|
| `a == b` | `BEQ` | igual |
| `a != b` | `BNE` | menor, maior ou unordered |
| `a < b` | `BMI` (ou `BLO`) | menor |
| `a <= b` | `BLS` | menor ou igual |
| `a > b` | `BGT` | maior |
| `a >= b` | `BGE` | maior ou igual |
| `isunordered(a, b)` | `BVS` (`BVC`: ordenado) | algum NaN |

As negações saem das condições complementares: `!(a < b)` = `BPL`, `!(a <= b)` = `BHI`
(maior ou unordered), `!(a > b)` = `BLE`, `!(a >= b)` = `BLT` (menor ou unordered).

**Implementação.** Soft-float portável em `src/core/softfloat.c` (`include/ksvm/softfloat.h`),
só com inteiros de 32 bits e um caminho com `uint64_t` em `FMUL`/`FDIV` que
`KSVM_NO_INT64=1` troca por produto em 16 bits e divisão longa; sem `float`/`double` do
host, sem libm, sem UB. Desenho do Berkeley SoftFloat 3 (representação intermediária e
rotina de arredondamento), código próprio. Desligada (`KSVM_ENABLE_FLOAT=0`), os opcodes
`0xA0–0xB1` continuam na tabela da ISA (o assembler e o disassembler os aceitam) mas geram
`FAULT_BAD_OPCODE` sem efeito (fault_addr = a palavra), `softfloat.c` compila vazio e
`CAPS(0x4000)` não liga o bit F.

### 5.8 Reservados

- `0x80–0x9F`: livres, reservados para extensões futuras do núcleo.
- `0xB2–0xBF`: reservados para a extensão F (ex.: `FMADD`, conversões de/para 64 bits).
- `0xC0–0xEF`: extensões específicas de plataforma (ex.: `AVR_IO`), nunca no núcleo.
- `0xF0–0xFF`: inválidos permanentemente; `0xFF` = `FAULT_BAD_OPCODE` garantido (uma imagem
  cheia de `0xFF`, estado de flash apagada, falha imediatamente).

Total na v2.0: **85 opcodes** na base, cobrindo o que a v1 tinha (menos as variantes 8/16)
mais comparação com sinal, lógica, deslocamentos, rotações, extensão de sinal, mul/div com e
sem sinal, halt/trap/break; e **18** da extensão F (`0xA0–0xB1`), 103 no total. A extensão
não muda `ISA_VERSION` (continua `0x0200`): ela é detectada por `CAPS`, como `MUL/DIV` e
`CLZ/CTZ/POPCNT`.

## 6. Turing-completude e suficiência para compiladores

- Memória endereçável de leitura/escrita (`LDW`/`STW` + índice em registrador).
- Desvio condicional sobre resultado de comparação (`CMP` + `Bcc`) e incondicional (`JMP`).
- Aritmética e lógica completas em 32 bits com e sem sinal.
- Pilha com `CALL`/`RET`/`PUSH`/`POP` + `fp` → recursão e frames.
- Salto indireto (`JMPR`, `CALLR`) → tabelas de salto (`switch`), ponteiros de função, vtables.
- `ADR`/`ADRL` → código relocável.
- Acesso a 8/16 bits com extensão de sinal e zero → `char`, `short`, `uint8_t` do C.
- `MULH`/`DIVU`/`REMU` + `ADC`/`SBC` → aritmética de 64 bits em software.
- Extensão F (§5.7) → `float` do C: as quatro operações, `sqrtf`, `fminf/fmaxf`,
  `rintf/floorf/ceilf/truncf`, conversões `int ↔ float` e comparações (inclusive NaN) por
  `FCMP` + `Bcc`. `double` fica para software (ou uma extensão futura).

Isso cobre o que um backend de C precisa; sem a extensão F, `float` também sai em software
(biblioteca no guest sobre `MULHU`, `CLZ`, shifts e `ADC/SBC`).

## 7. Pseudo-instruções (resolvidas pelo assembler, não existem no binário)

| Pseudo | Expande para |
|--------|--------------|
| `LI rd, valor` | `MOVI` se cabe em 14 bits com sinal, senão `MOVIL` |
| `LA rd, label` | `MOVIL rd, label` (absoluto) ou `ADRL rd, label−PC` com `.pic` |
| `FLI rd, valor` | `MOVIL rd, bits` do binary32 de `valor` (`fli r1, 1.5` = `MOVIL r1, 0x3FC00000`) |
| `CLR rd` | `MOV rd, zr` |
| `INC rd` / `DEC rd` | `ADDI rd, rd, 1` / `SUBI rd, rd, 1` |
| `NOT rd` | `NOT rd, rd` |
| `B label` | `BAL label` |
| `J label` | `JMP label` |
| `SUBI rd, ra, imm` com `imm < −8192` | `ADDL rd, ra, −imm` |
| `CMP ra, imm` | `CMPI` ou `CMPL` conforme tamanho |
| `PUSH r1, r2, r3` | sequência de `PUSH` |
