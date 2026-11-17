# 02 — Requisitos e restrições

## 1. Objetivo

Uma máquina virtual de 32 bits, Turing-completa, com conjunto de instruções suficiente
para servir de alvo de um compilador de linguagem de uso geral (C, Pascal, uma linguagem
própria), implementada em C limpo e modular, capaz de rodar em:

| Classe | Exemplos | Restrição dominante |
|--------|----------|---------------------|
| Desktop/servidor 64 bits | x86-64 Windows/Linux/macOS, ARM64 (Apple Silicon, Raspberry Pi 4) | Nenhuma; é o ambiente de desenvolvimento |
| Desktop 32 bits | x86 (`-m32`), ARMv7 | `size_t` 32 bits, acesso desalinhado pode falhar em ARMv5/v6 |
| Microcontrolador 32 bits | ARM Cortex-M0/M3/M4 (STM32, RP2040), ESP32, RISC-V | Sem SO, sem `stdio`, RAM de 8–512 KB, acesso desalinhado falha em M0 |
| Microcontrolador 8 bits Harvard | ATmega328P (2 KB SRAM, 32 KB flash), ATmega2560 (8 KB SRAM, 256 KB flash) | `int` é 16 bits, código em flash é lido com `pgm_read_byte`, RAM minúscula, ponteiros de 16 bits |

O ATmega328P é o **alvo mínimo**. Se a VM cabe nele, cabe em qualquer outro.

## 2. Requisitos funcionais

### 2.1 Núcleo da VM (`libksvm`)
- RF-01 Executar programas no formato binário `.ksx` (ver doc 05).
- RF-02 Suportar a ISA completa do doc 04: movimentação, aritmética inteira com e sem sinal,
  lógica e deslocamentos, load/store de 8/16/32 bits, pilha, chamadas, desvios condicionais,
  syscalls, halt.
- RF-03 Execução em passos: `ksvm_step()` executa uma instrução; `ksvm_run(n)` executa até
  `n` instruções ou até parar. Permite loop cooperativo em microcontroladores.
- RF-04 Detectar e reportar falhas sem derrubar o host: opcode inválido, acesso fora dos
  limites, escrita em região só-leitura, divisão por zero, estouro de pilha, syscall inválida,
  PC desalinhado.
- RF-05 Várias instâncias simultâneas (nenhum estado global).
- RF-06 Memória fornecida pelo host (buffer estático ou alocado), nunca alocada pelo núcleo.
- RF-07 Hook opcional de trace por instrução (compilado só se `KSVM_ENABLE_TRACE`).

### 2.2 Ferramentas (host apenas)
- RF-10 `ksas`: assembler de dois passos, em C, gera `.ksx`.
- RF-11 `ksdis`: disassembler de `.ksx` para texto.
- RF-12 `ksvm`: runner de linha de comando com HAL POSIX/Win32, opções de trace e dump.
- RF-13 `ksimg`: converte `.ksx` em array C (`const uint8_t PROGMEM image[]`) para embarcar em firmware.

### 2.3 Portas (HAL)
- RF-20 `hal_null` (sem I/O, para testes), `hal_posix`, `hal_win32`, `hal_avr`
  (UART + GPIO), `hal_arm_cm` (semihosting ou UART).

## 3. Requisitos não funcionais

- RNF-01 **C99 estrito**. Compila sem warnings com `-std=c99 -Wall -Wextra -Wpedantic
  -Wconversion -Wshadow -Wstrict-prototypes` em GCC e Clang, e com `/W4` no MSVC (modo C).
- RNF-02 **Zero comportamento indefinido** verificado por `-fsanitize=undefined,address` nos
  testes do host.
- RNF-03 Núcleo **freestanding**: só `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<string.h>`
  (apenas `memcpy`/`memset`, substituíveis). Nenhum `malloc`, `printf`, `math.h`, `float`.
- RNF-04 Núcleo cabe em ATmega328P: ≤ 12 KB de flash, ≤ 300 bytes de RAM para o estado da VM
  (sem contar a RAM do programa hóspede).
- RNF-05 Tamanho do `struct ksvm` conhecido em tempo de compilação (alocação estática).
- RNF-06 Desempenho no desktop: ≥ 50 M instruções/s em x86-64 com `-O2` (meta, não bloqueante).
- RNF-07 Nenhuma dependência externa (sem bibliotecas de terceiros). Build com CMake **e**
  Makefile simples; Arduino/PlatformIO só precisa copiar `src/core` + `include`.
- RNF-08 Todo módulo tem um `.h` com API documentada e um `.c`; símbolos públicos com
  prefixo `ksvm_`; internos `static`.
- RNF-09 ISA e formato binário versionados; mudanças incompatíveis incrementam a versão major.

## 4. Fora de escopo (v2.0)

- Ponto flutuante (reservar espaço de opcodes para uma extensão `F` futura).
- JIT, threads, MMU/paginação, proteção de memória por processo.
- Interrupções assíncronas (planejadas como extensão na fase 5, doc 10).
- Compilador de linguagem de alto nível (a ISA é desenhada para ser alvo fácil, mas o
  compilador é outro projeto).

## 5. Premissas assumidas

- Guest é sempre **little-endian** e 32 bits, independente do host.
- Endereços do guest são `uint32_t`; o host mapeia para seus buffers via barramento.
- Programas hóspedes são **confiáveis o bastante para não serem maliciosos**, mas a VM
  **não pode** corromper memória do host por causa de bug no programa hóspede
  (bounds check obrigatório).
