# 01 — Análise da KSVM v1

Leitura completa de `ksvm/` (≈3.300 linhas: `main.c`, `src/*.c`, `include/*.h`,
`assembler/*.py`, `examples/*.ksasm`).

## 1. O que a v1 é

| Aspecto | v1 |
|---------|----|
| Palavra | 32 bits |
| Registradores | 32 × `uint32_t`. R0–R25 gerais (R0–R3 apelidados EAX/EBX/ECX/EDX), R26=ESP, R27=EBP, R28=EIP, R29=FLAGS, R30–R31 reservados |
| Flags | ZERO, ERROR, SIGN, OVERFLOW, CARRY definidas; **só ZERO e CARRY são escritas** |
| Codificação | Tamanho variável: opcode `u16` + operando registrador `u16` + imediato de 1/2/4 bytes conforme a largura |
| Nomenclatura | `NOME` + `8|16|32` + `R|I` por operando (ex.: `MOV32RI`, `LOAD8RR`, `JG32I`) |
| Memória | `uint8_t*` flat de 2048 bytes, programa carregado no endereço 0, pilha começa em `memory_size - 1` |
| Controle | JMP, JZ, JNZ, JG, JGE, JL, JLE (só em 32 bits), CALL/RET via pilha |
| Término | Executar até `PC >= memory_size` ou encontrar opcode inválido (`$ xFF xFF` é usado como "halt" nos exemplos) |
| Syscall | `SYSCALL` copia R0–R3 para RAX/RDI/RSI/RDX e executa `asm("syscall")` do Linux x64, somando o ponteiro base da memória aos argumentos marcados como ponteiro |
| Assembler | Python, uma passada + fixup de labels; tokens por espaço; `$` para dados brutos; `:label` para referência |
| Debug | `backlog.txt` com cada instrução; modo passo a passo com dump de memória colorido |

A estrutura em módulos (`register`, `cpu`, `instruction`, `vm`) já estava no caminho certo.
A ideia de uma tabela `InstructionDef` com tamanhos de operando também.

## 2. Problemas encontrados

### 2.1 Portabilidade (impedem rodar fora de Linux x64)

| Onde | Problema | Consequência |
|------|----------|--------------|
| `cpu.c` `cpu_syscall` | `asm("syscall")` com registradores x64 | Não compila em x86, ARM, AVR, MSVC |
| `cpu.c` | `#include <sys/syscall.h>`, `<unistd.h>` | Só POSIX |
| `main.c` | `fork()`, `waitpid()`, `system("clear")` | Só POSIX; o `fork` existe só para sobreviver a um crash do syscall |
| `cpu.c` load/store/push/pop | `*(uint32_t*)(&memory[addr])` | Acesso desalinhado: UB em C, *bus error* em ARM antigo, resultado errado em AVR |
| `register.c` `regSet16/regSet8` | Escreve nos 2/1 primeiros bytes do `uint32_t` via punning | Assume host little-endian; em big-endian escreve nos bytes altos |
| `vm.c` | `*CAST(uint16_t*, &operands[0])` em bytes do programa | Mesmo problema de alinhamento e endianness |
| `cpu.c` `cpu_add_*` | `pow(2, size)` de `math.h` | Puxa libm e ponto flutuante para uma soma de inteiros; proibitivo em AVR |
| `main.c` | `calloc`, `malloc`, `FILE*` no caminho principal | Não há separação entre núcleo e host |

### 2.2 Corretude

| Onde | Problema |
|------|----------|
| `vm.c` | **`STORE*` nunca foi ligado ao dispatcher** (comentado); `cpu_store_*` só tem protótipo. A VM não consegue escrever na memória exceto por PUSH |
| `cpu.c` | `JG/JGE/JL/JLE` usam só CARRY → são comparações **unsigned**. Não existe comparação com sinal; SIGN e OVERFLOW nunca são calculados |
| `cpu.c` | `MUL` e `DIV` não setam flags; `DIV` por zero → SIGFPE no host |
| `cpu.c` | `SUB`/`CMP` com `size=8/16`: o carry é calculado sobre `uint32_t` (`sub > a`), funciona por acidente |
| `vm.c` `vm_run` | Sem *bounds check* em PC, endereços de load/store ou SP. Qualquer programa pode corromper o heap do host |
| `vm.c` | Pilha inicia em `memory_size - 1` (desalinhada) |
| `vm.c` | Não existe `HALT`/`NOP`. O programa termina lendo um opcode inválido, que seta `FLAG_ERROR` e retorna 1 (erro) mesmo em saída normal |
| `instruction.c` | `LOAD16RI`/`LOAD8RI` têm imediato de 4 bytes (endereço) mas `STORE16RI`/`STORE8RI` têm 2/1 bytes: inconsistente |
| `assembler.py` | `apply_labels` sempre escreve 4 bytes no fixup, mesmo se a instrução tem imediato de 1 ou 2 bytes (sobrescreve a próxima instrução) |
| `assembler.py` `binary.py` | `makebytes` gera LSB-first, faz `insert(0, 0)` para padding (coloca zeros nos bytes *baixos*) e depois `little_endian()` inverte. O resultado é certo por dupla inversão acidental |
| `asm_lexer.py` | Qualquer token começando com `x` ou `b` vira hex/bin (um label `buffer` referenciado como `:buffer` funciona, mas `b1` como registrador não) |

### 2.3 Manutenibilidade

- A ISA está descrita **três vezes** em C (`instruction.c` tabela, `vm.c` print, `vm.c`
  compute) e uma quarta em Python (`opcodes.py`). Adicionar uma instrução exige tocar 4 lugares.
- As variantes 8/16/32 × R/I multiplicam cada operação por 6 opcodes. Isso gerou 80+ opcodes
  para ~20 operações reais.
- Formatação de debug (`vm_print_instruction`, `vm_dump_program`) está dentro do núcleo da VM.
- Instruções de tamanho variável tornam o disassembler ambíguo: `vm_dump_program` tenta
  "ressincronizar" procurando um opcode válido a cada 2 bytes.
- Nenhum teste automatizado.

## 3. O que vale preservar

- A ideia de **tabela de definição de instrução** (opcode → tamanhos) — vai virar a fonte
  única via X-macro.
- O **modelo de pilha** com CALL/RET empilhando o endereço de retorno (compiladores adoram).
- O conceito de **flags + desvio condicional** (familiar para quem vem de x86); na v2 vira
  Z/N/C/V completo.
- A **separação em módulos** (registrador, CPU, instrução, VM) — vai ser aprofundada com
  barramento de memória e HAL.
- A ferramenta de **passo a passo com dump** — sai do núcleo e vira um módulo de trace opcional.

## 4. Lições que moldam a v2

1. Todo acesso a múltiplos bytes passa por `load_u32_le(const uint8_t*)` /
   `store_u32_le(uint8_t*, uint32_t)` implementadas com shifts. Nunca cast de ponteiro.
2. A VM nunca executa nada "nativo". Syscalls são números despachados para funções do host.
3. O núcleo não inclui `stdio.h`, `stdlib.h` (exceto `stddef`/`stdint`), `math.h`.
4. Opcodes com largura fixa: o tamanho é função do opcode, nunca do conteúdo.
5. Toda operação de memória passa por um barramento com verificação de limites e permissão.
6. Uma única tabela da ISA gera tudo (interpretador, assembler, disassembler, documentação).
7. Testes desde a primeira instrução.
