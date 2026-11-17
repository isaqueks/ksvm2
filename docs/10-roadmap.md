# 10 — Roadmap de implementação

Ordem pensada para que cada fase termine com algo executável e testado, e para que a
portabilidade seja verificada cedo (não como "pass final").

## Fase 0 — Especificação (este diretório) ✅
Critério: docs 01–10 revisados. Qualquer mudança de ISA a partir daqui edita o doc 04
**antes** do código.

## Fase 1 — Fundação do núcleo ✅
Entregas:
- `include/ksvm/{config,types,isa,isa.def,cpu,bus,vm}.h`
- `src/core/{bytes,isa,cpu,alu,bus,decode}.c`
- `src/core/exec.c` com: NOP, MOV, MOVI, MOVIL, ADD/ADDI/ADDL, SUB/SUBI, CMP/CMPI/CMPL,
  AND/OR/XOR (+I/L), NOT, SHL/SHR/SAR (+I), JMP, Bcc, HALT
- `hal_null`, `encode.c`, harness de teste, `test_bytes/alu/bus/decode/exec_*`
- `Makefile` + `CMakeLists.txt`; CI com gcc/clang + sanitizers + `-m32`

Critério: um programa em C (via `ksvm_enc_*`) que calcula fatorial por laço roda e os
testes passam com ASan/UBSan. **Já nesta fase**: `avr-gcc -mmcu=atmega328p -c src/core/*.c`
e `arm-none-eabi-gcc -mcpu=cortex-m0 -c` compilam sem warning (sem HAL ainda).

> **Concluída.** Entregue: headers e `src/core/{isa,cpu,alu,bus,exec,vm,fault,encode}.c`,
> `hal_null`, harness e suítes unitárias, fatorial por laço; `make test`/`test-asan`,
> `check-clang`, `check-m32`, `check-freestanding`, `check-avr`, `check-arm`.
> Divergências do plano: `bytes.c` virou `bytes.h` (inline) e `decode.c` foi absorvido por
> `isa.c` (`ksvm_decode`); só GNU make (sem `CMakeLists.txt`) e sem CI hospedado — o
> equivalente é `make check-all` local.

## Fase 2 — Memória, pilha, chamadas, syscalls, loader ✅
Entregas:
- Loads/stores (8/16/32, I e X), PUSH/POP, CALL/CALLR/RET/JMPR, ADR/ADRL, MFSR/MTSR,
  TRAP, BRK, WFI, SXB/SXH/ZXB/ZXH, ADC/SBC/NEG, ROL/ROR, TST/TSTI
- MUL/MULI/MULH/MULHU/DIV/DIVU/REM/REMU atrás de `KSVM_ENABLE_MULDIV`
- CLZ/CTZ/POPCNT atrás de `KSVM_ENABLE_BITOPS`
- `syscall.c` com grupos 0, 1, 2, 5; `hal_posix`, `hal_win32`
- `loader.c` + `ksx` writer mínimo em C (usado pelos testes para gerar imagens)
- Runner `ksvm` com `--trace/--step/--regs`
- Testes de faults, loader, syscall; fuzz básico

Critério: todas as instruções do doc 04 implementadas e testadas; `hello.ksx` gerado por
um teste roda no runner em Linux e Windows e imprime pela syscall `PUTS`.

> **Concluída.** Entregue: todas as instruções do doc 04 (menos `PUSHM/POPM`, opcionais),
> MUL/DIV e CLZ/CTZ/POPCNT atrás das chaves (testados também desligados, config "min"),
> syscalls dos grupos 0–5 (arquivos só no desktop, GPIO nas HALs embarcadas), `hal_posix`/`hal_win32` (+ `hal_fs` compartilhado),
> loader + `ksx_write`, runner `ksvm` (`--trace/--step/--regs/--no-fs`), testes de faults,
> atomicidade, loader e syscalls, fuzz de execução (`make fuzz`, `fuzz-quick` no
> `check-all`). `MOVHI` foi removido da ISA depois (opcode 0x04 inválido; doc 04 §5.1).

## Fase 3 — Toolchain ✅
Entregas:
- `ksas` completo (lexer, parser, expressões, symtab, layout, emit, writer, listing)
- `ksdis` + `ksvm_disasm_one` usado no trace do runner
- `ksimg`
- Exemplos portados da v1 e novos; golden tests; round-trip test
- `docs/04-isa.md` passa a ser conferido contra `isa.def` por script (nomes/opcodes)

Critério: `make test` verde; `examples/*.ksasm` todos montam e rodam com saída esperada.

> **Concluída.** Entregue: `ksas` completo (com `-l` listing, `-g`, `-D`, `.include`),
> `ksdis` (+ `--raw` para round-trip), `ksimg`, `ksvm_disasm_one` no trace do runner,
> exemplos novos e portados da v1, round-trip `ksas → ksdis --raw → ksas` bit a bit
> (`make test-ksas`, `make test-tools`). Pendente: macros do assembler (`.macro`, fase 5)
> e a conferência automática de `docs/04-isa.md` contra `isa.def` por script.

## Fase 4 — Portas embarcadas ✅
Entregas:
- `hal_avr.c` (UART0 por registradores + millis por Timer0) e `ports/arduino/` (sketch
  usando `Serial`)
- `blink.ksasm`, `hello_uart.ksasm` rodando em ATmega328P real ou `simavr`
- `hal_arm_cm.c` + `ports/stm32/` ou RP2040; teste em QEMU (`mps2-an385`)
- Medição de flash/RAM do núcleo em AVR; ajustes (`PROGMEM` nas tabelas, remover strings)
- Big-endian no CI (qemu-user ppc/s390x/mips)

Critério: RNF-04 satisfeito; mesmos golden tests (os que não usam fs) passam em simavr e
QEMU-ARM com a saída pela UART/semihosting.

> **Concluída.** Entregue: `hal_avr.c` por registradores (UART0, millis/Timer0, GPIO,
> ANALOG_READ, PWM) e `hal_arm_cm.c` (UART CMSDK, SysTick, semihosting); firmwares
> hello/blink/selftest/fault rodando no `simavr` (ATmega328P) e no QEMU `mps2-an385`
> (`make test-ports`), programa próprio com `make avr-custom/arm-custom KSX=...`; tabela
> da ISA compacta e na flash; `make size-avr` mede a meta RNF-04 (núcleo + HAL + libgcc
> ≤ 12 KB: cumprida); big-endian com `make test-bigendian` (PowerPC/qemu-user).
> Pendente: variante com o core Arduino (`ports/arduino/`, `Serial`), syscall `MICROS`
> nas HALs embarcadas (só o desktop tem), `PWM_WRITE`/`ANALOG_READ` no Cortex-M (hoje
> NOSYS), e porta para placa real (STM32/RP2040) além do QEMU.

## Fase 5 — Extensões e otimizações (cada uma opcional e independente)
- `PUSHM/POPM`
- Interrupções: tabela de vetores, `WFI` acordado por `ksvm_irq(vm, n)`, bit I em FLAGS,
  `RETI`. Permite que um programa no AVR reaja a timer/UART sem polling
- Dispatch por *computed goto* (`KSVM_DISPATCH_GOTO`, só GCC/Clang) + benchmark
- Macros no assembler; linker de múltiplos objetos (`.kso`) se houver compilador
- Snapshot/restore de `struct ksvm` + RAM (checkpoint, debug reverso)
- Depurador interativo no runner (breakpoints por endereço, watch de memória)
- Extensão F (ponto flutuante): primeiro uma `libfloat.ksasm` em assembly do guest como
  prova de conceito e oráculo; depois opcodes `FADD/FSUB/FMUL/FDIV/FCMP/F2I/I2F` em
  soft-float C portável (`KSVM_ENABLE_FLOAT`); ambos devem dar bits idênticos
- Geração do doc 04 e da tabela do assembler a partir de `isa.def` por script

## Riscos e mitigação

| Risco | Mitigação |
|-------|-----------|
| Núcleo não cabe no ATmega328P | Compilar para AVR desde a fase 1 e medir a cada PR; features atrás de `#if` |
| Decisões de ISA mudarem após o assembler existir | Toda mudança passa por `isa.def` + doc 04; `ksas`/`ksdis` leem a tabela, não hardcodes |
| UB escondido que só aparece em ARM/AVR | Sanitizers + `-Wconversion` + alvo big-endian + Cortex-M0 em QEMU (que faz *hard fault* em acesso desalinhado) |
| Performance ruim por byte-a-byte no barramento | Caminho rápido para região RAM com índice direto; medir com benchmark `fib(30)`; só então considerar leitura de palavra alinhada com `memcpy` (que é portável) |
| Escopo crescer | Fases 1–3 são o produto; 4 prova a portabilidade; 5 é opcional |

## Estimativa de tamanho de código (ordem de grandeza)

| Componente | Linhas de C |
|------------|-------------|
| Núcleo (`src/core`) | 1.800–2.500 |
| HALs (4) | 600 |
| `ksas` | 1.500–2.000 |
| `ksdis` + `ksimg` + runner | 600 |
| Testes | 2.500+ |
