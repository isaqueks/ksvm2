# 08 — Portabilidade: regras concretas

Cada item abaixo corresponde a um defeito real da v1 ou a um requisito das plataformas-alvo.

## 1. Representação de dados

| Regra | Por quê |
|-------|---------|
| Todo valor de 16/32 bits em memória do guest é lido/escrito por `bytes.h`: `ksvm_ld32le(const uint8_t *p)` = `p[0] \| p[1]<<8 \| p[2]<<16 \| (uint32_t)p[3]<<24` | Elimina casts desalinhados (UB) e dependência de endianness. `(uint32_t)` antes do shift de 24 evita promoção a `int` com overflow |
| Registradores são `uint32_t` e só isso. Larguras menores são máscaras (`& 0xFF`) e extensões de sinal explícitas | `regSet16` da v1 escrevia via punning dependente de endianness |
| Extensão de sinal: `(int32_t)(x << (32-n)) >> (32-n)` é implementation-defined; usar `x & mask; if (x & sign) x \|= ~mask` ou `(x ^ sign) - sign` | C99 não garante shift aritmético de negativos |
| Nunca `int`/`unsigned`/`long` para dados do guest. `int` é 16 bits no AVR, `long` é 32 no Win64 e 64 no Linux x64 | Overflow silencioso e tamanhos de struct diferentes |
| Aritmética: fazer em `uint32_t` (wrap definido), converter para `int32_t` só para testar sinal via bit 31 | Overflow de `int32_t` é UB; de `uint32_t` é módulo 2³² |
| Flags C/V de ADD: `c = (res < a)`, `v = ((a ^ res) & (b ^ res)) >> 31`. SUB: `c = (a >= b)`, `v = ((a ^ b) & (a ^ res)) >> 31` | Sem `uint64_t`, sem `pow()` |
| `MULH`: usar `uint64_t` (C99 obrigatório); se `KSVM_NO_INT64`, implementar por quatro produtos de 16 bits | Alguns compiladores embarcados antigos |
| Divisão: testar `rb == 0` e `(ra == INT32_MIN && rb == -1)` antes de dividir | Ambos são UB em C e SIGFPE em x86 |
| Shifts: contagem sempre `& 31`; shift de `uint32_t` por 32 é UB | |
| Nenhum `float`/`double` no núcleo | AVR sem FPU: libgcc float custa KBs; Cortex-M0 idem |

## 2. Memória

| Regra | Por quê |
|-------|---------|
| Núcleo não chama `malloc`/`free`. O host fornece `uint8_t ram[]` e `ksvm vm` estáticos ou alocados como preferir | MCUs sem heap; determinismo |
| Todo acesso do guest passa pelo barramento com verificação `off < size` usando aritmética sem overflow: `addr >= base && addr - base < size && size - (addr-base) >= len` | v1 não verificava nada |
| Região CODE em Harvard tem `read8` callback; o núcleo nunca derreferencia `backing` de uma ROM diretamente se `read8 != NULL` | Flash do AVR não está no espaço de dados |
| `struct ksvm` não contém ponteiros para dentro de si mesmo | Permite copiar/salvar estado (snapshot) com `memcpy` |
| Tamanho de `ksvm` ≤ ~250 bytes com `KSVM_MAX_REGIONS=3` | ATmega328P tem 2 KB; sobra RAM para o guest |

## 3. Código

| Regra | Por quê |
|-------|---------|
| `-std=c99` sem extensões: sem `typeof`, sem *statement expressions*, sem *computed goto* (exceto atrás de `#ifdef __GNUC__` na fase 5), sem VLAs, sem `#pragma pack`, sem `__attribute__` no núcleo (só nas HALs e portas: `hal_avr.c`, `hal_arm_cm.c`, `ports/`; o núcleo só vê o atributo via `KSVM_TABLE_ATTR`) | MSVC, compiladores embarcados proprietários (IAR, Keil) |
| Sem `inline` assembly no núcleo (a v1 tinha `asm("syscall")`) | Óbvio |
| Headers do núcleo: `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<string.h>` (`memcpy`/`memset`, substituíveis por `KSVM_MEMCPY`) | Freestanding: `-ffreestanding -nostdlib` deve linkar |
| `switch` com `default:` sempre; enums não usados como índice sem checar limites | `-Wswitch`, robustez |
| Sem recursão no núcleo | Pilha do AVR é a mesma SRAM de 2 KB |
| `static const` para tabelas, lidas pelo núcleo **só** por `ksvm_isa_get`/`ksvm_isa_format`/`ksvm_isa_len` e `KSVM_TABLE_READ_U8/U16/PTR`. No AVR o `ksvm_config_user.h` da porta define `KSVM_TABLE_PROGMEM=1`, `KSVM_TABLE_ATTR=PROGMEM` e as leituras como `pgm_read_*`; `KSVM_ISA_COMPACT=1` troca `ksvm_isa[]` (3 bytes/opcode) por `ksvm_isa_compact[]` (1 byte: formato + flags; `len` derivado) | Tabela de 256 × 3 bytes = 768 B de SRAM se ficasse na RAM (o firmware chegava a 2045 de 2048 bytes de SRAM); compacta na flash: 256 B |
| Nomes de instruções/condições/faults atrás de `KSVM_ISA_NAMES`/`KSVM_FAULT_NAMES`, como arrays com `KSVM_TABLE_ATTR` (não literais soltos, que iriam para a RAM no AVR); com `KSVM_TABLE_PROGMEM` os ponteiros devolvidos são endereços de flash | Strings que o AVR não precisa; se usadas, não ocupam SRAM |
| Argumento de macro X que vai para `#`/`##` é usado direto no corpo de `KSVM_OP`, nunca repassado a outra macro | Argumentos repassados são expandidos antes: `ADC` é macro em `<avr/io.h>` (registrador do conversor A/D) |
| Nenhuma variável global mutável | Reentrância; múltiplas VMs; `-Wunused` honesto |

## 4. Matriz de compilação (CI)

| Alvo | Compilador | Flags | Verifica |
|------|------------|-------|----------|
| x86-64 Linux | gcc, clang | `-std=c99 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` | baseline + testes + golden |
| x86-64 Linux | clang | `-fsanitize=address,undefined -O1` | UB/overflow/OOB nos testes |
| x86 32 bits | gcc `-m32` | idem | `size_t` 32 bits, `long` 32 |
| x86-64 Windows | MSVC `cl /W4 /WX /std:c11` (modo C), MinGW gcc | | `long` 32, `hal_win32` |
| ARM64 | gcc/clang nativo (macOS/Linux) ou `aarch64-linux-gnu-gcc` + qemu-user | | big registers, alinhamento |
| Cortex-M (`make check-arm`, `make test-arm`) | `arm-none-eabi-gcc -mcpu=cortex-m0 -mthumb -Os -ffreestanding`; porta linkada com `-nostartfiles --specs=nano.specs` | compila; e a porta `ports/arm_cm` roda no `qemu-system-arm -M mps2-an385` (Cortex-M3 executando código ARMv6-M) com saída na UART CMSDK | Sem divisão em hardware (libgcc), Thumb-1. **Limite**: o QEMU 8.2 não gera falha em `LDR` desalinhado nem com `CCR.UNALIGN_TRP` nem no `microbit` (M0), verificado; a prova contra acesso desalinhado é o UBSan (`-fsanitize=alignment`) do `test-asan` |
| AVR (`make check-avr`, `make test-avr`, `make size-avr`) | `avr-gcc 7.3 -mmcu=atmega328p -Os -std=c99` + flags de tamanho da porta (abaixo) | compila; porta `ports/avr` roda no `simavr`; `size-avr` falha se núcleo + `hal_avr` + libgcc > 12 KB (RNF-04) | `int` 16 bits, PROGMEM, SRAM de 2 KB |
| Big-endian | `powerpc-linux-gnu-gcc` ou `s390x` + qemu-user, ou `mips` | testes | Qualquer dependência de endianness do host |

Um alvo big-endian na matriz é a única forma honesta de provar que `bytes.h` é usado em
todo lugar.

### 4.1 Portas embarcadas: o que roda e como é verificado

Os dois alvos rodam os mesmos quatro programas (gerados por `ports/common/genprogs.c`) e
o teste confere a saída da UART com `ports/common/expect_run.sh` (timeout; falha se a
string esperada não aparecer):

| Programa | Exercita | Saída esperada |
|----------|----------|----------------|
| `hello` | PUTS de string na ROM (flash), HALT | `KSVM on AVR` / `KSVM on ARM`, `KSVM halt 00000000` |
| `blink` | PIN_MODE/PIN_WRITE/PIN_READ no pino 13, SLEEP_MS, MILLIS (≥ 600 ms medidos) | AVR `101010 time ok`; ARM `000000 time ok` (o QEMU não modela o GPIO do AN385) |
| `selftest` | 91 verificações: ADD/SUB/MUL/MULH/MULHU/DIV/REM/shifts/rotações com valores de borda, CLZ/CTZ/POPCNT, carry, imediatos, `.data`/`.bss`, little-endian, CALL/RET/PUSH/POP, VERSION, RANDOM, syscall inexistente | `selftest ok`; o esperado é calculado no host pela ALU do próprio núcleo, então divergência entre o binário de 8/32 bits e o desktop aparece como `selftest FAIL <n>` |
| `fault` | LDW fora do mapa | `KSVM FAULT 04 pc=00000008 addr=7FFFFFF0` |

simavr: `simavr -m atmega328p -f 16000000 avr-*.elf`; a UART0 aparece no stderr do
simavr (uma linha por `\n`), e o firmware termina com `cli; sleep`, o que encerra a
simulação. QEMU: `qemu-system-arm -M mps2-an385 -nographic -semihosting -kernel
arm-*.elf`; termina por semihosting `SYS_EXIT`.

### 4.2 Tamanho no ATmega328P (`make size-avr`)

Medido do mapa do linker após `--gc-sections`, firmware `hello` (o núcleo é o mesmo em
todos; só a imagem do guest muda). Configuração da porta: tabela compacta na flash, sem
nomes, `KSVM_MAX_REGIONS=3`, MULH com `uint64_t`.

| Módulo | Flash antes da fase 4 | Flash agora | RAM agora |
|--------|----------------------:|------------:|----------:|
| alu | 2810 | 1816 | 0 |
| bus | 1474 | 1444 | 0 |
| cpu | 414 | 208 | 0 |
| exec | 2892 | 2814 | 0 |
| isa (tabela + decode) | 1066 (tabela de 768 B também na **SRAM**) | 496 | 0 |
| loader | 1936 | 1848 | 0 |
| syscall | 1700 | 1586 | 0 |
| vm | 534 | 528 | 14 |
| **núcleo** | **12826** | **10740** | **14** |
| hal_avr | 1256 | 990 | 5 |
| libgcc (div/mul) | 316 | 452 | 0 |
| **núcleo + hal_avr + libgcc** | **14398** | **12182** (meta ≤ 12288) | |

"Antes" = primeira medição linkada da porta (só `-Os -mcall-prologues`, tabela na RAM).
`ksx_write`, `encode` e `disasm` nunca entram (biblioteca estática + `--gc-sections`); os
nomes também não, porque o firmware não chama `ksvm_op_name`/`ksvm_fault_name`.
`sizeof(struct ksvm)` = 215 bytes no AVR (RNF-04: ≤ 300). Firmware completo `hello`:
12844 B de flash, 1277 B de SRAM estática (dos quais 1024 são a RAM do guest).

De onde veio a redução:

| Medida (em sequência, cada uma sobre a anterior) | Ganho |
|--------|------:|
| Flags `-mrelax -mstrict-X -fno-inline-small-functions` (sobre `-Os -mcall-prologues`) | ~500 B |
| `KSVM_ISA_COMPACT` (256 B em vez de 768 B) | 510 B |
| GPIO da HAL sem tabela (blocos PINx/DDRx/PORTx contíguos), argumentos em 8 bits | 266 B |
| ADD/SUB/ADC/SBC num só `add_with_carry` + CTZ via CLZ do bit isolado (`a & -a`) | 316 B |
| `ksvm_cond_holds` por pares de condição e flags em 8 bits | 220 B |
| SHL/SHR/SAR/ROL/ROR num só `shift` | 346 B |
| MULH com `uint64_t` em vez de `KSVM_NO_INT64` | 54 B |
| `-fno-jump-tables` (obrigatório, ver abaixo) | −48 B |

Todas as reescritas do núcleo foram conferidas contra a versão anterior por teste
diferencial (centenas de milhões de entradas, inclusive todas as 16 condições × 256 flags e
todas as contagens de shift) além da suíte unitária.

Variantes (`size-avr` imprime): tabela completa na flash +510 B; `KSVM_NO_INT64=1` **+50 B**
(com avr-gcc 7.3 o `__umulsidi3` da libgcc é menor que quatro `__mulsi3` + somas, por isso a
porta usa `uint64_t`); sem PWM −186 B; tabela na RAM: +768 B de SRAM (firmware com 2045 de
2048 bytes: inviável).

Armadilha do toolchain: com `-mrelax`, o avr-ld 2.26 (binutils do Ubuntu 24.04) corrompe
tabelas de salto de `switch` quando o `jmp __tablejump2__` imediatamente anterior é
encurtado para `rjmp`: o rótulo da tabela, no fim dos bytes removidos, não é ajustado e cada
`case` salta para o código do anterior (observado em `exec_move`: MOVI executava como MOV).
Dependia do layout e só o `test-avr` detectou. A porta compila com `-fno-jump-tables`
(+48 B).

Margem e alavancas se o núcleo crescer (medidas): ~100 B de folga hoje.
`KSVM_HAL_AVR_PWM=0` −186 B, `KSVM_ENABLE_BITOPS=0` −512 B, `KSVM_ENABLE_MULDIV=0`
−1036 B (perde instruções da ISA: o programa precisa não usá-las), `KSVM_CHECK_STACK=0`
só −34 B (não vale a perda de proteção). Sem contar a libgcc, núcleo + `hal_avr` = 11730 B.

### 4.3 Tamanho no Cortex-M0 (`ports/arm_cm`, config de MCU)

Núcleo 5380 B, `hal_arm_cm` 416 B, libgcc 432 B (divisão), firmware `hello` completo
7184 B de flash; `sizeof(struct ksvm)` = 244 bytes. Com os padrões de desktop
(`KSVM_ISA_NAMES=1`, tabela completa) `isa` sobe de 404 para 2589 B.

## 5. Checklist de revisão por módulo

- [ ] Nenhum cast `(uint32_t *)`, `(uint16_t *)` sobre `uint8_t *`.
- [ ] Nenhum `memcpy` para/de `struct` como forma de serialização.
- [ ] Nenhum `int` em assinatura que carregue valor do guest.
- [ ] Toda divisão guardada.
- [ ] Todo shift com contagem mascarada.
- [ ] Todo índice de array vindo do guest verificado antes.
- [ ] Compila com `-Wconversion` sem warnings (força casts explícitos conscientes).
- [ ] Compila com `avr-gcc` e `arm-none-eabi-gcc` (`make check-avr check-arm`) e na
      configuração das portas (`make test-avr`, que também compila o núcleo com nomes na
      flash, tabela completa e tabela na RAM: `avr-config-check`).
- [ ] Tabelas constantes lidas só por `ksvm_isa_get`/`ksvm_isa_format`/`ksvm_isa_len` ou
      `KSVM_TABLE_READ_*`; nunca `ksvm_isa[op]` direto no núcleo (não existe com
      `KSVM_ISA_COMPACT`, e é flash com `KSVM_TABLE_PROGMEM`).
- [ ] Strings do núcleo como arrays com `KSVM_TABLE_ATTR`, não literais soltos (no AVR um
      literal vai para a SRAM).
- [ ] Nenhum argumento de X-macro repassado a outra macro antes de `#`/`##` (`ADC` etc.
      são macros nos headers do AVR).
- [ ] Caminho que só precisa de 8 bits (flags, número de pino) usa `uint8_t`: cada
      comparação/operação de 32 bits custa ~10 bytes de flash no AVR.
- [ ] Mudança no núcleo: `make size-avr` continua dentro da meta (RNF-04) e
      `make test-avr test-arm` verdes (o selftest compara o binário embarcado com a ALU do
      desktop).
