# 05 — Formato binário `.ksx` e modelo de memória

## 1. Modelo de memória do guest

Espaço de endereços de 32 bits, plano do ponto de vista do programa. O **barramento**
(doc 03 §3.2) o divide em regiões; a v2.0 usa três:

| Região | Permissão | Conteúdo | Backing em von Neumann | Backing em Harvard (AVR) |
|--------|-----------|----------|------------------------|--------------------------|
| `CODE` | R + X | `.text` + `.rodata` da imagem | `uint8_t[]` na RAM do host (ou o próprio buffer do arquivo) | `const uint8_t PROGMEM[]` na flash, lido por `pgm_read_byte` |
| `RAM` | R + W | `.data` (copiado da imagem), `.bss` (zerado), heap, pilha | `uint8_t[]` | `uint8_t[]` na SRAM |
| `MMIO` | R + W | Registradores de dispositivo (opcional) | callbacks | callbacks |

Layout padrão escolhido pelo loader (configurável no cabeçalho):

```
0x0000_0000  ┌──────────────┐
             │ CODE (.text) │  ← entry aponta aqui
             │ .rodata      │
             └──────────────┘
ram_base     ┌──────────────┐  ram_base = alinhar_para_cima(code_size, 16) por padrão
             │ .data        │  copiado da imagem no load
             │ .bss         │  zerado no load
             │ heap ↓       │
             │   ...        │
             │ pilha ↑      │  sp inicial = ram_base + ram_size (múltiplo de 4)
             └──────────────┘  stack_limit = sp_inicial − stack_size
0xFFFF_0000  ┌──────────────┐
             │ MMIO         │  (opcional, 64 KB)
             └──────────────┘
```

Motivos:
- Código em `0` deixa labels absolutos pequenos (cabem em `MOVI` 14 bits para programas
  até 8 KB).
- RAM imediatamente após o código mantém endereços curtos em MCUs. No desktop o host pode
  colocar a RAM em `0x1000_0000` sem mudar o código do guest se ele usar `ADRL`/`LA`.
- Ler constantes de `CODE` (strings, tabelas) é permitido; escrever não (`FAULT_MEM_PERM`).
  Em AVR isso evita copiar `.rodata` para a SRAM minúscula.

## 2. Arquivo `.ksx`

Todos os campos little-endian. Offsets relativos ao início do arquivo.

```
offset  tam  campo          descrição
0x00    4    magic          "KSX\0"  (0x4B 0x53 0x58 0x00)
0x04    1    ver_major      2
0x05    1    ver_minor      0
0x06    2    flags          bit0: PIC (código independente de posição)
                            bit1: tem seção de símbolos (debug)
0x08    4    entry          endereço inicial do PC (dentro de CODE)
0x0C    4    code_off       offset do .text+.rodata no arquivo
0x10    4    code_size      bytes
0x14    4    data_off       offset do .data inicializado no arquivo
0x18    4    data_size      bytes copiados para RAM em ram_base
0x1C    4    bss_size       bytes zerados após .data
0x20    4    ram_base       endereço da RAM no guest (0 = loader decide: code_size alinhado)
0x24    4    ram_min        RAM mínima necessária = data+bss+heap_min+stack_size (informativo)
0x28    4    stack_size     bytes reservados para pilha (loader define stack_limit)
0x2C    4    sym_off        offset da tabela de símbolos (0 se ausente)
0x30    4    sym_size       bytes
0x34    4    checksum       soma de 32 bits (Adler-32 simplificado ou CRC32) de [0x38, fim)
0x38    8    reservado      zeros
0x40         ... seções (code, data, sym) em qualquer ordem, alinhadas a 4
```

Cabeçalho fixo de 64 bytes. O loader lê cada campo com `load_u32_le()` — jamais `memcpy`
para um `struct` (padding e endianness).

Tabela de símbolos (opcional, para `ksdis` e trace): sequência de registros
`u32 addr, u8 kind, u8 len, char name[len]`.

## 3. Carregamento

### 3.1 Host com sistema de arquivos (POSIX/Win32)
```
1. ler arquivo inteiro em buffer `image`
2. ksvm_load_image(vm, image, size, ram, ram_size):
   a. validar magic/versão/checksum → KSVM_E_IMAGE
   b. map_rom(0, image + code_off, code_size)      ← aponta para dentro do buffer, sem cópia
   c. ram_base = header.ram_base ou align16(code_size)
   d. map_ram(ram_base, ram, ram_size)
   e. memcpy(ram, image + data_off, data_size); memset(ram + data_size, 0, bss_size)
   f. sp = ram_base + ram_size (alinhado a 4); stack_limit = sp − stack_size
   g. ksvm_reset(vm, entry, sp)
```
O buffer `image` deve viver enquanto a VM rodar (a região CODE aponta para ele).

### 3.2 Microcontrolador Harvard (AVR)
Implementado em `ports/avr/` (fase 4). O build (`mk/ports.mk`) converte um `.ksx` em
`program_image.h` com `ports/avr/mkimage.py`, que antes valida magic, versão e checksum
(imagem corrompida falha no build, não no dispositivo):
```c
/* gerado; KSVM_IMAGE_ATTR é definido por quem inclui (PROGMEM no AVR) */
#define KSVM_PROGRAM_IMAGE_SIZE 96u
static const uint8_t ksvm_program_image[KSVM_PROGRAM_IMAGE_SIZE] KSVM_IMAGE_ATTR = { 0x4B, 0x53, 0x58, 0x00, ... };
```
`ports/avr/main.c`:
```c
#define KSVM_IMAGE_ATTR PROGMEM
#include "program_image.h"
static uint8_t ram[1024];                  /* KSVM_AVR_RAM_SIZE */
...
ksvm_hal_avr_init(&hal_ctx, &hal);
ksvm_init(&vm, &hal);
ksvm_load_image_ex(&vm, ksvm_program_image, KSVM_PROGRAM_IMAGE_SIZE, ram, sizeof ram,
                   ksvm_hal_avr_flash_read8);
while (ksvm_run(&vm, 500) is KSVM_RUNNING or KSVM_YIELD) { /* tarefas do firmware */ }
```
`ksvm_hal_avr_flash_read8` (em `hal_avr.c`) é `pgm_read_byte(backing + off)`: lê só os
primeiros 64 KB de flash (suficiente no ATmega328P; um ATmega2560 com imagem acima disso
precisaria de `pgm_read_byte_far`). `ksvm_load_image_ex` usa esse leitor também para o
próprio cabeçalho e o checksum, e copia `.data` byte a byte por ele. A região CODE fica
mapeada como ROM com o mesmo `read8`: o código do guest roda da flash e nunca é copiado
para a SRAM. A SRAM guarda só `struct ksvm` (215 bytes), `ram[]` e a pilha do AVR.

Os programas de teste ainda não vêm do `ksas`: `ports/common/genprogs.c` (compilado para
o host) monta-os com `ksvm_enc_*` e grava com `ksvm_ksx_write`. Qualquer `.ksx` serve
como entrada do `mkimage.py`.

Nenhuma linha do núcleo inclui `<avr/pgmspace.h>`. A porta fornece
`ports/avr/ksvm_config_user.h` (`-DKSVM_HAVE_USER_CONFIG -Iports/avr`), que inclui o
header e define `KSVM_TABLE_PROGMEM`, `KSVM_TABLE_ATTR` e `KSVM_TABLE_READ_*`; com isso
a tabela da ISA também vai para a flash (doc 08 §3).

### 3.3 Microcontrolador 32 bits (Cortex-M)
Igual ao caso POSIX: a flash é mapeada no espaço de endereços, então `read8 = NULL`
(acesso direto) e `ksvm_load_image` comum. RAM é um array estático. Implementado em
`ports/arm_cm/` com o mesmo `program_image.h` (sem `KSVM_IMAGE_ATTR`).

## 4. Validações do loader

| Checagem | Erro |
|----------|------|
| magic ≠ "KSX\0" | `KSVM_E_IMAGE_MAGIC` |
| ver_major ≠ 2 | `KSVM_E_IMAGE_VERSION` |
| checksum inválido | `KSVM_E_IMAGE_CHECKSUM` |
| code_off+code_size > size, idem data | `KSVM_E_IMAGE_RANGE` |
| entry fora de [0, code_size) ou desalinhado | `KSVM_E_IMAGE_ENTRY` |
| data_size + bss_size + stack_size > ram_size | `KSVM_E_NO_RAM` |
| regiões se sobrepõem | `KSVM_E_OVERLAP` |

## 5. Por que não ELF

ELF resolveria tudo isso, mas um parser ELF mínimo tem mais código do que a VM inteira
cabe em AVR. O `.ksx` tem 64 bytes de cabeçalho e um parser de ~60 linhas. Se um dia houver
um backend LLVM/GCC para a KSVM, um conversor ELF→KSX no host é trivial.
