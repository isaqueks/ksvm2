# KSVM v2

Máquina virtual de 32 bits em C99 portável: o mesmo núcleo compila para
desktop (x86-64, ARM64, x86 32 bits, PowerPC big-endian), Cortex-M0 e
ATmega328P. Especificação completa em `../docs/` (arquitetura no doc 03, ISA
no doc 04, formato `.ksx` no doc 05, syscalls/HAL no doc 06, assembler no
doc 07, portabilidade no doc 08, testes no doc 09, roadmap no doc 10).

Estado: fases 1–4 do roadmap concluídas — núcleo (`libksvm.a`), assembler
`ksas`, disassembler `ksdis`, conversor `ksimg`, runner `ksvm` com HALs
POSIX/Win32, e portas ATmega328P (simavr) e Cortex-M (QEMU `mps2-an385`).
Da fase 5: a **extensão F** (ponto flutuante IEEE 754 binary32 nos registradores
inteiros, soft-float só com inteiros, bits idênticos em todo host; doc 04 §5.7),
ligada no desktop e no Cortex-M e desligada no ATmega328P, onde não cabe nos 12 KB.

## Build

O toolchain fica no WSL (ver `BUILD.md`):

    wsl.exe -e bash -lc 'cd /mnt/c/Users/isaque.kanitz/KSVM2/ksvm2 && make check-all'

`make check-all` é a definição de "verde" do projeto: roda tudo da tabela
abaixo marcado com ✓. Alvos que dependem de um toolchain ausente pulam com
aviso.

| Alvo | O que faz | check-all |
|------|-----------|:---------:|
| `make` | `build/libksvm.a` (só `src/core`) | |
| `make test` | testes unitários em três configs: padrão, "alt" (trace, MULH e FMUL/FDIV sem `uint64_t`, 3 regiões) e "min" (sem MUL/DIV, BITOPS nem extensão F, sem nomes, tabela compacta da ISA) | ✓ |
| `make test-asan` | idem com `-fsanitize=address,undefined -fno-sanitize-recover=all` | ✓ |
| `make check-freestanding` | `src/core/*.c` com `-ffreestanding -nostdlib` | ✓ |
| `make check-clang` / `check-m32` | núcleo com clang / com `-m32` | ✓ |
| `make check-avr` / `check-arm` | núcleo com `avr-gcc` / `arm-none-eabi-gcc` | ✓ |
| `make test-bigendian` | suíte unitária em PowerPC via `qemu-ppc` | ✓ |
| `make fuzz-quick` | fuzzer de execução, 100 sementes × 4 variantes | ✓ |
| `make fuzz` / `fuzz-asan` | fuzzer longo (1000 sementes) / com ASan+UBSan (200) | |
| `make ksas` | `build/ksas` | |
| `make test-ksas` / `test-ksas-asan` | testes do assembler / com sanitizers | ✓ / |
| `make ksas-examples` | monta `examples/**/*.ksasm` com listing em `build/examples/` | |
| `make tools` | `build/tools/{ksvm,ksdis,ksimg}` | |
| `make test-tools` | disassembler, política de caminho do `hal_fs`, `test_runner.sh` (ksvm/ksdis/ksimg/ksas de ponta a ponta, round-trip `ksas → ksdis --raw → ksas`) | ✓ |
| `make tools-win` | ferramentas `.exe` com MinGW (e testes, sob WSL com interop) | |
| `make ports-avr` / `ports-arm` | firmwares de exemplo em `build/ports/{avr,arm}/` | |
| `make test-ports` | `test-avr` (simavr) + `test-arm` (qemu-system-arm), conferindo a UART | ✓ |
| `make size-avr` | flash/RAM por módulo no AVR; falha se passar da meta RNF-04 (12 KB) | ✓ |
| `make avr-custom KSX=…` / `arm-custom KSX=…` | firmware com um programa `.ksx` próprio | |
| `make clean` | remove `build/` | |

Flags do núcleo: `-std=c99 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
-Wstrict-prototypes -Wmissing-prototypes -Werror`.

## Exemplo de ponta a ponta

Montar e rodar no desktop (`ksas` → `.ksx` → `ksvm`):

    make ksas tools
    build/ksas examples/hello.ksasm -o build/hello.ksx -l build/hello.lst
    build/tools/ksvm build/hello.ksx            # imprime "Hello, KSVM!"
    build/tools/ksvm --trace build/hello.ksx    # trace instrução a instrução no stderr
    build/tools/ksdis build/hello.ksx           # disassembly com símbolos

O mesmo `.ksx` como firmware:

    make avr-custom KSX=build/hello.ksx         # build/ports/avr/avr-custom.elf
    simavr -m atmega328p -f 16000000 build/ports/avr/avr-custom.elf

    make arm-custom KSX=build/hello.ksx         # build/ports/arm/arm-custom.elf
    qemu-system-arm -M mps2-an385 -nographic -semihosting -kernel build/ports/arm/arm-custom.elf

O firmware valida a imagem no build (`ports/avr/mkimage.py`: magic, versão e
checksum), embute-a na flash e imprime pela UART; termina com
`KSVM halt <código>` ou `KSVM FAULT ...`. `make ports-avr` / `ports-arm`
geram os firmwares de exemplo (hello, blink, selftest, fselftest, fault) a partir de
programas montados por `ports/common/genprogs.c`.

## Estrutura

```
include/ksvm/      API pública (ksvm.h inclui o núcleo)
  config.h         chaves KSVM_* (MAX_REGIONS, ENABLE_TRACE, ENABLE_MULDIV, ENABLE_FLOAT,
                   ISA_COMPACT, ...)
  isa.def          fonte única da ISA: uma linha por instrução (X-macro)
  isa.h            opcodes, formatos, flags, condições, extratores de campo
  bytes.h          ld/st little-endian e extensão de sinal (static inline)
  cpu.h alu.h bus.h fault.h hal.h trace.h vm.h syscall.h loader.h
  encode.h disasm.h ksx_write.h types.h softfloat.h
  hal_null.h hal_posix.h hal_win32.h hal_avr.h hal_arm_cm.h
src/core/          núcleo freestanding: isa alu cpu bus exec vm fault syscall
                   loader ksx_write encode disasm softfloat
src/hal/           hal_null (testes), hal_posix, hal_win32, hal_fs (arquivos,
                   compartilhado pelas duas), hal_avr, hal_arm_cm
src/tools/         ksvm_main.c (runner), toolio, ksas/, ksdis/, ksimg/
tests/unit/        harness (test.h), fixture, oráculos, uma suíte por área
tests/ksas/        testes do assembler
tests/tools/       test_disasm, test_hal_fs, test_runner.sh, mkimg
tests/fuzz/        fuzz_exec.c
examples/          programas .ksasm (hello, fib, sieve, recursion, float, ...), lib/,
                   v1_ported/
ports/avr/         main.c, ksvm_config_user.h, mkimage.py, size_report.py
ports/arm_cm/      main.c, startup.c, linker.ld
ports/common/      genprogs.c, expect_run.sh, isa_compact_check.c
mk/                fragmentos do Makefile: ksas.mk tools.mk fuzz.mk ports.mk
```

## Uso mínimo da biblioteca

```c
#include "ksvm/ksvm.h"

static uint8_t ram[4096];
ksvm vm;

ksvm_init(&vm, &my_hal);                       /* ou NULL: sem serviços */
if (ksvm_load_image(&vm, image, image_size, ram, sizeof ram) != KSVM_OK) { ... }
while (ksvm_run(&vm, 1000) == KSVM_RUNNING || vm.status == KSVM_YIELD) {
    /* fatia de tempo: atender UART, timers, watchdog... */
}
if (vm.status == KSVM_FAULT) { /* ksvm_fault_name(vm.fault), vm.fault_addr */ }
```

## Decisões desta implementação

- **Operandos -> campos**: `rd` é sempre o campo `ra`; em `rd, ra, rb` os
  operandos ocupam `ra, rb, rc`; em `rd, ra, imm` ocupam `ra, rb`; CMP/TST usam
  `ra, rb|imm`; JMPR/CALLR/MTSR leem `rb` (detalhes no topo de `isa.def`).
- **Checksum `.ksx`**: Adler-32 padrão sobre `[0x38, fim)`.
- **SYSCALL**: número = imm14 lido **sem sinal** (0..0x3FFF).
- **Opcode 0x04** (era `MOVHI`) foi removido: é inválido e gera
  `KSVM_FAULT_BAD_OPCODE`; use `MOVIL`/`li`.
- **Limites do fetch** (doc 04 §5.6): a instrução inteira (as duas palavras
  do formato L) precisa estar numa única região EXEC e `PC_next` não passa de
  `0xFFFFFFFF`; senão `KSVM_FAULT_PC_RANGE` antes de qualquer efeito.
- **Não implementados** (geram `KSVM_FAULT_BAD_OPCODE`): `PUSHM`/`POPM`
  (opcionais, fase 5), e MUL/DIV, CLZ/CTZ/POPCNT ou a extensão F quando
  desligados na config.
- **Faults** são atômicos: a instrução não tem efeito e o `pc` fica nela.
- **Extensão F** (doc 04 §5.7): opcodes `0xA0–0xB1` (`FADD FSUB FMUL FDIV FMIN
  FMAX` com três operandos; `FSQRT FNEG FABS FRINT FFLOOR FCEIL FTRUNC FTOI FTOU
  ITOF UTOF` com dois; `FCMP ra, rb`), binary32 nos registradores inteiros,
  sempre RNE, subnormais, NaN canônico `0x7FC00000`, divisão por zero = ±inf,
  sem exceções. `FCMP` usa a convenção ARM VFP (`<` BMI, `<=` BLS, `>` BGT,
  `>=` BGE, `==` BEQ, `!=` BNE, unordered BVS). Soft-float em
  `src/core/softfloat.c`, só inteiros, com e sem `KSVM_NO_INT64`; atrás de
  `KSVM_ENABLE_FLOAT` (1 no desktop e Cortex-M, 0 no AVR: com F o firmware vai a
  16674 B e estoura a meta de 12288 B). Presença: `CAPS(0x4000)` bit 2 (doc 06).
  Assembler: `.float 1.5, -2e3, inf, nan, 0x1.8p1` e `fli rd, v` (= `movil rd,
  bits`); exemplo em `examples/float.ksasm`.
