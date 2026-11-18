# 06 — Syscalls e camada de abstração de host (HAL)

## 1. Princípio

A v1 executava `syscall` nativo do Linux x64 somando o ponteiro base da memória aos
argumentos. Isso é intrinsecamente não portável e inseguro. Na v2:

- `SYSCALL n` é uma instrução que **pausa a VM e chama uma função C do host** com o número `n`.
- O host lê argumentos dos registradores `r1`–`r6` e escreve o retorno em `r1`, erro em `r2`.
- Ponteiros do guest (`r1` = buffer, etc.) são **endereços do guest**; o host os acessa
  somente via `ksvm_bus_read8/write8` ou helpers `ksvm_copy_from_guest/to_guest`, que
  validam limites. O host nunca recebe um `uint8_t*` cru para dentro da RAM do guest
  (exceto por um helper explícito `ksvm_guest_ptr()` que retorna `NULL` se a faixa não for
  contígua/RAM — útil no desktop para `write(2)` sem cópia).

## 2. Convenção de chamada

| Registrador | Papel |
|-------------|-------|
| imediato de `SYSCALL` | número da syscall |
| `r1`–`r6` | argumentos 1–6 |
| `r1` (retorno) | resultado (≥ 0) ou −1 em erro |
| `r2` (retorno) | código de erro (`KSVM_ERR_*`), 0 se ok |
| demais | preservados pela VM (o host não deve tocar) |

Syscall desconhecida: `r1 = −1`, `r2 = KSVM_ERR_NOSYS`. **Não** é fault: permite que um
programa teste capacidades em tempo de execução (`SYSCALL 0x01` CAPS, abaixo).

Uma syscall pode retornar `KSVM_SYS_YIELD` para o núcleo, que coloca a VM em `KSVM_YIELD`
e devolve o controle ao host (ex.: `SLEEP_MS` cooperativo em MCU, `READ` bloqueante no
desktop que o host quer tratar fora do `ksvm_run`). O PC já aponta para a instrução
seguinte; o host retoma com `ksvm_run`.

## 3. Números padrão

Grupos de 16. Todas opcionais exceto `EXIT`; a ausência é reportada por `CAPS`.

### Grupo 0 — núcleo
| N | Nome | Args | Retorno |
|---|------|------|---------|
| 0x00 | `EXIT` | r1 = código | não retorna (status HALTED) |
| 0x01 | `CAPS` | r1 = número de syscall a consultar, ou `0x4000` (extensões da ISA) | r1 = 1 se suportada, 0 senão; com `0x4000`, máscara abaixo |
| 0x02 | `VERSION` | — | r1 = versão da VM (`major<<16 \| minor<<8 \| patch`) |
| 0x03 | `YIELD` | — | devolve controle ao host (status YIELD) |
| 0x04 | `ABORT` | r1 = código | status FAULT, fault = TRAP |

**`CAPS` de extensões da ISA.** Números de syscall vão só até `0x3FFF` (imm14), então
consultas acima dessa faixa nunca colidem com uma syscall. `CAPS` com `r1 = 0x4000`
(`KSVM_CAPS_ISA` em `syscall.h`) devolve em `r1` a máscara das extensões opcionais que esta
VM executa (`r2 = 0`); qualquer outro valor acima de `0x3FFF` responde 0:

| Bit | Máscara | Extensão | Chave de compilação |
|-----|---------|----------|---------------------|
| 0 | `0x1` | M: `MUL MULI MULH MULHU DIV DIVU REM REMU` | `KSVM_ENABLE_MULDIV` |
| 1 | `0x2` | B: `CLZ CTZ POPCNT` | `KSVM_ENABLE_BITOPS` |
| 2 | `0x4` | **F**: ponto flutuante binary32, `FADD..FCMP` (doc 04 §5.7) | `KSVM_ENABLE_FLOAT` |
| 3–31 | — | reservados, lidos como 0 | |

Desktop e Cortex-M respondem `0x7`; a porta AVR responde `0x3` (sem F). Um programa que usa
float testa o bit 2 antes e, sem ele, recorre a uma biblioteca em software ou avisa, em vez de
cair em `FAULT_BAD_OPCODE` (exemplo em `examples/float.ksasm`):

```asm
        li      r1, 0x4000
        syscall CAPS
        andi    r1, r1, 4           ; bit F
        beq     sem_float
```

### Grupo 1 — console
| N | Nome | Args | Retorno |
|---|------|------|---------|
| 0x10 | `PUTC` | r1 = byte | 0 |
| 0x11 | `GETC` | — | r1 = byte ou −1 se não há (não bloqueia) |
| 0x12 | `PUTS` | r1 = ptr, r2 = len | bytes escritos |
| 0x13 | `GETS` | r1 = ptr, r2 = max | bytes lidos até `\n` (incluso) ou max; 0 se nada; −1 EOF |

### Grupo 2 — tempo
| N | Nome | Args | Retorno |
|---|------|------|---------|
| 0x20 | `MILLIS` | — | r1 = ms desde o boot (wrap em 2³²) |
| 0x21 | `SLEEP_MS` | r1 = ms | 0 (pode YIELD) |
| 0x22 | `MICROS` | — | r1 = µs (opcional) |

### Grupo 3 — arquivos (desktop; `KSVM_ENABLE_SYSCALL_FS`)
| N | Nome | Args |
|---|------|------|
| 0x30 | `OPEN` | r1 = ptr caminho, r2 = len, r3 = flags (RD=1, WR=2, CREAT=4, APPEND=8) → r1 = fd |
| 0x31 | `CLOSE` | r1 = fd |
| 0x32 | `READ` | r1 = fd, r2 = ptr, r3 = len → bytes lidos |
| 0x33 | `WRITE` | r1 = fd, r2 = ptr, r3 = len → bytes escritos |
| 0x34 | `SEEK` | r1 = fd, r2 = offset, r3 = whence |

fd 0/1/2 = stdin/stdout/stderr pré-abertos.

### Grupo 4 — GPIO / hardware (MCU)
| N | Nome | Args |
|---|------|------|
| 0x40 | `PIN_MODE` | r1 = pino, r2 = modo (0 in, 1 out, 2 in_pullup) |
| 0x41 | `PIN_WRITE` | r1 = pino, r2 = valor |
| 0x42 | `PIN_READ` | r1 = pino → r1 = valor |
| 0x43 | `ANALOG_READ` | r1 = canal → r1 = 0..1023 |
| 0x44 | `PWM_WRITE` | r1 = pino, r2 = 0..255 |

### Grupo 5 — memória
| N | Nome | Args |
|---|------|------|
| 0x50 | `MEM_INFO` | → r1 = ram_base, r2 = ram_size, r3 = heap_start (fim de .bss), r4 = stack_limit |
| 0x51 | `RANDOM` | → r1 = 32 bits pseudoaleatórios |

### `0x100`–`0x3FFF` — definidos pelo host
Encaminhados sem interpretação para `hal->syscall`.

## 4. `struct ksvm_hal` e portas

Definição no doc 03 §3.3. As syscalls dos grupos 0–2 e 5 são implementadas **no núcleo**
(`syscall.c`) sobre `hal->putc/getc/millis/sleep_ms`. Os grupos 3, 4 e usuário caem em
`hal->syscall`, que recebe o `ksvm*` e usa os helpers públicos para ler argumentos.

| Porta | `putc/getc` | `millis` | `syscall` extra | Observações |
|-------|-------------|----------|-----------------|-------------|
| `hal_null` | descarta / −1 | contador de instruções | nenhum | Testes unitários determinísticos |
| `hal_posix` | `write(1)`/`read(0)` não bloqueante | `clock_gettime` | grupo 3 | Linux, macOS, *BSD |
| `hal_win32` | `WriteFile`/`ReadConsole` | `GetTickCount64` | grupo 3 | MSVC e MinGW |
| `hal_avr` | UART0 por registradores (sem Arduino core), 115200 8N1 com U2X; `getc` não bloqueante | Timer0 em CTC, 1 ms; `sleep_ms` em modo IDLE | grupo 4: pinos na numeração do Uno (0–7 PD, 8–13 PB, 14–19 PC), `ANALOG_READ` canais 0–7 (AVcc), `PWM_WRITE` nos pinos 3/11 (Timer2) e 9/10 (Timer1) se `KSVM_HAL_AVR_PWM` | `ports/avr/` (ATmega328P, simavr). Variante com `Serial`/core do Arduino (`hal_avr_arduino.cpp`) ainda não existe |
| `hal_arm_cm` | UART CMSDK APB por registradores (MPS2); semihosting só para `SYS_EXIT` | SysTick, 1 ms; `sleep_ms` com WFI | grupo 4: `PIN_MODE` (0/1; sem pull-up → INVAL), `PIN_WRITE`, `PIN_READ` nos GPIO CMSDK AHB (pinos 0–63); `ANALOG_READ`/`PWM_WRITE` → NOSYS | `ports/arm_cm/` (QEMU `mps2-an385`). Placas STM32/RP2040 precisam trocar UART/GPIO |

## 5. Helpers do núcleo para o host

```c
uint32_t ksvm_arg(const ksvm *vm, unsigned n);            /* n = 1..6 → r[n] */
void     ksvm_ret(ksvm *vm, uint32_t value, uint32_t err);
int      ksvm_copy_from_guest(ksvm *vm, void *dst, uint32_t gaddr, uint32_t len);
int      ksvm_copy_to_guest  (ksvm *vm, uint32_t gaddr, const void *src, uint32_t len);
uint8_t *ksvm_guest_ptr(ksvm *vm, uint32_t gaddr, uint32_t len, uint8_t perm); /* NULL se inválido ou não-RAM */
```
Cópias falham com `KSVM_E_RANGE` sem alterar o estado da VM.

## 6. Segurança do host

- Nunca `exec`, `system`, syscalls nativas cruas em nome do guest.
- Comprimentos vêm do guest: sempre `min(len, limite_do_host)` antes de qualquer buffer de pilha.
- `OPEN` em `hal_posix` recebe caminho relativo ao diretório atual; o runner pode receber
  `--no-fs` para desligar o grupo 3 inteiro.
