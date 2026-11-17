# KSVM v2 — Plano de projeto

Documentação de estudo e planejamento da segunda versão da KSVM, uma máquina virtual
de 32 bits escrita em C portável, projetada para rodar em qualquer CPU (x86, x64,
ARM, RISC-V, microcontroladores AVR/ATmega com arquitetura Harvard).

Nada aqui é código ainda. Estes documentos são a especificação que o código deve seguir.

| # | Documento | Conteúdo |
|---|-----------|----------|
| 01 | [Análise da v1](01-analise-v1.md) | O que a ksvm antiga faz, o que está quebrado e as lições aprendidas |
| 02 | [Requisitos e restrições](02-requisitos.md) | Objetivos, plataformas-alvo, regras de código, o que está fora do escopo |
| 03 | [Arquitetura de software](03-arquitetura.md) | Módulos, API pública, camada de abstração de host (HAL), barramento de memória, build |
| 04 | [ISA — conjunto de instruções](04-isa.md) | Registradores, flags, formatos de codificação, lista completa de instruções |
| 05 | [Formato binário e modelo de memória](05-formato-binario.md) | Arquivo `.ksx`, cabeçalho, seções, carregamento em von Neumann e Harvard |
| 06 | [Syscalls e HAL](06-syscalls-hal.md) | Interface com o host, números de syscall padrão, portas por plataforma |
| 07 | [Assembler e disassembler](07-assembler.md) | Sintaxe, diretivas, pseudo-instruções, arquitetura do `ksas` e do `ksdis` |
| 08 | [Portabilidade](08-portabilidade.md) | Regras concretas para compilar e rodar em x86/x64/ARM/AVR sem UB |
| 09 | [Testes](09-testes.md) | Estratégia de testes unitários, golden tests, fuzz e matriz de compiladores |
| 10 | [Roadmap](10-roadmap.md) | Fases de implementação, ordem, critérios de conclusão |

## Decisões-chave (resumo executivo)

1. **Instruções de largura fixa de 32 bits**, com uma forma "longa" de 64 bits (palavra extra
   com imediato de 32 bits). O tamanho de cada instrução é determinado só pelo opcode.
2. **32 registradores de 32 bits**: `R0` é sempre zero, `R30 = FP`, `R31 = SP`. `PC` e `FLAGS`
   são registradores especiais fora do banco geral.
3. **ALU só em 32 bits**. Larguras 8/16 existem apenas em load/store e nas instruções de
   extensão de sinal. Isso reduz o número de opcodes em 3x em relação à v1.
4. **Flags Z, N, C, V** no estilo ARM e um único opcode de desvio condicional `Bcc` com
   campo de condição de 4 bits (16 condições).
5. **Acesso à memória sempre byte a byte**, compondo little-endian em software. Zero casts de
   ponteiro desalinhados, zero dependência do endianness do host.
6. **Barramento de memória com regiões** (código, RAM, MMIO). Em Harvard, a região de
   código é lida da flash via callback; em von Neumann, é um `uint8_t[]` comum.
7. **Núcleo freestanding**: sem `malloc`, sem `stdio`, sem `math.h`, sem floats. O host
   fornece buffers e um `struct ksvm_hal` com funções de I/O.
8. **Syscalls são despachadas para o host** por número, nunca executadas como syscalls
   nativas. Buffers passados ao host são validados pelo barramento.
9. **Tabela única de ISA** via X-macro (`isa.def`): gera enum, nomes, formatos e tamanhos
   usados pelo interpretador, pelo assembler e pelo disassembler. Nada duplicado.
10. **Assembler e disassembler em C** (não mais Python), compartilhando a tabela da ISA.
