# 03 — Arquitetura de software

## 1. Visão em camadas

```
┌──────────────────────────────────────────────────────────────┐
│  Ferramentas (host)      ksvm (runner)   ksas   ksdis  ksimg │
├──────────────────────────────────────────────────────────────┤
│  HAL (uma por plataforma) hal_posix hal_win32 hal_avr hal_null│
├──────────────────────────────────────────────────────────────┤
│  libksvm (núcleo freestanding)                               │
│   ┌────────┐ ┌────────┐ ┌───────┐ ┌────────┐ ┌───────────┐   │
│   │ loader │ │  exec  │ │  alu  │ │  bus   │ │  syscall  │   │
│   └────────┘ └────────┘ └───────┘ └────────┘ └───────────┘   │
│   ┌────────────────────────────────────────────────────────┐ │
│   │ isa (tabela única via X-macro)   bytes (LE helpers)    │ │
│   └────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

Regra de dependência: setas só apontam para baixo. O núcleo nunca inclui nada do HAL nem
das ferramentas; ele recebe o HAL por ponteiro de struct.

## 2. Layout de diretórios

Estrutura real de `ksvm2/` (fases 1–4 concluídas):

```
ksvm2/
├── include/ksvm/
│   ├── ksvm.h          API pública agregadora (inclui os demais)
│   ├── config.h        chaves KSVM_* com valores padrão (o host sobrescreve; ver §7)
│   ├── types.h         status, códigos de erro KSVM_E_*, tipos básicos
│   ├── bytes.h         ld/st little-endian e extensão de sinal (static inline, sem .c)
│   ├── isa.def         X-macro: UMA linha por instrução (fonte única da ISA)
│   ├── isa.h           enum ksvm_op, formatos, flags, condições, extratores de campo
│   ├── cpu.h alu.h     registradores/flags; operações da ALU
│   ├── bus.h           ksvm_region, ksvm_bus, leitura/escrita/fetch verificados
│   ├── fault.h         enum ksvm_fault, ksvm_fault_name
│   ├── hal.h           struct ksvm_hal (funções do host)
│   ├── vm.h            struct ksvm, init/map/reset/step/run
│   ├── syscall.h       números de syscall, ksvm_arg/ret, ksvm_copy_*
│   ├── softfloat.h     ksvm_f32_* (extensão F; doc 04 §5.7)
│   ├── loader.h        cabeçalho .ksx e ksvm_load_image
│   ├── ksx_write.h     gravação de .ksx (usada pelo ksas e pelos testes)
│   ├── encode.h        codificadores ksvm_enc_* (assembler, testes, genprogs)
│   ├── disasm.h        disassembler de uma instrução (ksdis e trace do runner)
│   ├── trace.h         hook de trace (opcional)
│   └── hal_null.h hal_posix.h hal_win32.h hal_avr.h hal_arm_cm.h
├── src/
│   ├── core/           núcleo freestanding (só <string.h>)
│   │   ├── isa.c       tabelas geradas de isa.def (completa ou compacta) e decodificação
│   │   ├── alu.c       add/sub/mul/div/shift/rot com cálculo de flags
│   │   ├── softfloat.c IEEE 754 binary32 só com inteiros (extensão F, KSVM_ENABLE_FLOAT)
│   │   ├── cpu.c       estado da CPU, condições de Bcc
│   │   ├── bus.c       regiões, lookup, acesso verificado, fetch
│   │   ├── exec.c      fetch-decode-execute (§5), um switch por grupo de opcodes
│   │   ├── vm.c        ciclo de vida: init, mapeamentos, reset, run
│   │   ├── fault.c     nomes dos faults (KSVM_FAULT_NAMES)
│   │   ├── syscall.c   validação de argumentos e despacho ao HAL
│   │   ├── loader.c    parse do cabeçalho .ksx, montagem das regiões
│   │   ├── ksx_write.c gravação de .ksx com checksum
│   │   ├── encode.c    codificação de instruções
│   │   └── disasm.c    texto de uma instrução (KSVM_ENABLE_DISASM)
│   ├── hal/
│   │   ├── hal_null.c  HAL determinística dos testes
│   │   ├── hal_posix.c hal_win32.c   HALs de desktop
│   │   ├── hal_fs.c hal_fs.h         grupo 3 (arquivos) compartilhado pelas HALs de desktop
│   │   ├── hal_avr.c   ATmega328P (UART0, Timer0, GPIO, ADC, PWM)
│   │   └── hal_arm_cm.c Cortex-M (UART CMSDK, SysTick, semihosting)
│   └── tools/
│       ├── ksvm_main.c runner `ksvm`
│       ├── toolio.c/.h E/S comum das ferramentas
│       ├── ksas/       assembler: lexer parser expr symtab layout insn mnemonic
│       │               emit listing ctx ksas main
│       ├── ksdis/      disassembler: main symtab listing raw
│       └── ksimg/      main.c (.ksx -> array C para embutir)
├── tests/
│   ├── unit/           harness test.h, fixture, miniasm, oráculos e uma suíte por área
│   ├── ksas/           testes do assembler (lexer, expressões, codificação, erros, execução)
│   ├── tools/          test_disasm.c, test_hal_fs.c, test_runner.sh (ksvm/ksdis/ksimg/ksas
│   │                   de ponta a ponta), mkimg.c
│   └── fuzz/           fuzz_exec.c
├── examples/           *.ksasm (hello, fib, sieve, recursion...), lib/io.ksasm, v1_ported/
├── ports/
│   ├── avr/            main.c, ksvm_config_user.h, mkimage.py, size_report.py
│   ├── arm_cm/         main.c, startup.c, linker.ld (QEMU mps2-an385)
│   └── common/         genprogs.c (programas de teste do guest), expect_run.sh,
│                       isa_compact_check.c
├── mk/                 fragmentos do Makefile: ksas.mk tools.mk fuzz.mk ports.mk
├── Makefile            núcleo, testes (configs padrão/alt/min) e check-all
├── BUILD.md            toolchain (WSL) e alvos
└── README.md
```

A especificação (este diretório) fica fora de `ksvm2/`, em `../docs/`. Não há
`CMakeLists.txt`: o build é só GNU make.

## 3. Estruturas de dados centrais

### 3.1 `ksvm_cpu`
```c
typedef struct ksvm_cpu {
    uint32_t r[32];      /* r[0] lido sempre como 0; escrita ignorada */
    uint32_t pc;         /* sempre múltiplo de 4 */
    uint32_t flags;      /* bits Z N C V (doc 04) */
} ksvm_cpu;
```
Tamanho: 136 bytes. Aceitável para o ATmega328P.

### 3.2 `ksvm_region` e `ksvm_bus`
```c
typedef enum { KSVM_MEM_READ = 1, KSVM_MEM_WRITE = 2, KSVM_MEM_EXEC = 4 } ksvm_mem_perm;

typedef struct ksvm_region {
    uint32_t base;       /* endereço inicial no espaço do guest */
    uint32_t size;
    uint8_t  perm;       /* máscara ksvm_mem_perm */
    uint8_t  kind;       /* KSVM_REGION_RAM | KSVM_REGION_ROM | KSVM_REGION_MMIO */
    void    *backing;    /* uint8_t* (RAM), const uint8_t* (ROM/flash), ou ctx (MMIO) */
    uint8_t (*read8)(struct ksvm_region*, uint32_t off);          /* usado por ROM-flash e MMIO */
    void    (*write8)(struct ksvm_region*, uint32_t off, uint8_t v); /* usado por MMIO */
} ksvm_region;

#define KSVM_MAX_REGIONS 4   /* config.h; 3 bastam: código, RAM, MMIO */

typedef struct ksvm_bus {
    ksvm_region regions[KSVM_MAX_REGIONS];
    uint8_t     count;
} ksvm_bus;
```

Caminho rápido: `bus.c` verifica primeiro a região RAM (a mais acessada) e, se
`kind == RAM`, lê direto do `backing` por índice. Só regiões `ROM`/`MMIO` chamam `read8`.
Em plataformas von Neumann o código também é uma região `RAM` com `perm = READ|EXEC`,
então o fetch é um acesso direto a array.

Todo acesso de 16/32 bits é decomposto em bytes **pelo barramento**, não pelo chamador:
`ksvm_bus_read32(bus, addr, &out)` lê 4 bytes consecutivos (passando por `read8` se
necessário) e compõe little-endian. Isso elimina qualquer problema de alinhamento e
endianness em um único lugar.

### 3.3 `ksvm_hal`
```c
typedef struct ksvm_hal {
    void *ctx;
    /* I/O básico */
    int  (*putc)(void *ctx, uint8_t c);              /* retorna <0 em erro */
    int  (*getc)(void *ctx);                         /* retorna -1 se não há dado */
    /* Tempo */
    uint32_t (*millis)(void *ctx);
    void     (*sleep_ms)(void *ctx, uint32_t ms);
    /* Syscalls não cobertas pelas básicas (fd, GPIO, usuário) */
    int  (*syscall)(struct ksvm *vm, uint32_t num);  /* retorna KSVM_SYS_OK / KSVM_SYS_UNKNOWN */
    /* Término */
    void (*on_halt)(void *ctx, uint32_t code);
} ksvm_hal;
```
Todo ponteiro pode ser `NULL`; o núcleo trata ausência como "não suportado" e retorna erro
ao programa hóspede (em `R2`), nunca falha.

### 3.4 `ksvm` (a instância)
```c
typedef enum {
    KSVM_RUNNING, KSVM_HALTED, KSVM_FAULT, KSVM_YIELD /* syscall pediu para devolver controle */
} ksvm_status;

typedef struct ksvm {
    ksvm_cpu        cpu;
    ksvm_bus        bus;
    const ksvm_hal *hal;
    ksvm_status     status;
    ksvm_fault      fault;        /* detalhe quando status == KSVM_FAULT */
    uint32_t        fault_addr;   /* endereço ou opcode envolvido */
    uint32_t        exit_code;
    uint32_t        stack_limit;  /* SP abaixo disso → KSVM_FAULT_STACK */
    uint32_t        insn_count;   /* estatística; 32 bits bastam */
#if KSVM_ENABLE_TRACE
    ksvm_trace_fn   trace; void *trace_ctx;
#endif
} ksvm;
```

## 4. API pública (`vm.h`)

```c
void        ksvm_init(ksvm *vm, const ksvm_hal *hal);
int         ksvm_map_ram (ksvm *vm, uint32_t base, uint8_t *mem, uint32_t size);
int         ksvm_map_rom (ksvm *vm, uint32_t base, const uint8_t *mem, uint32_t size,
                          uint8_t (*read8)(ksvm_region*, uint32_t)); /* read8 NULL = acesso direto */
int         ksvm_map_mmio(ksvm *vm, uint32_t base, uint32_t size, void *ctx,
                          uint8_t (*rd)(ksvm_region*, uint32_t),
                          void (*wr)(ksvm_region*, uint32_t, uint8_t));
int         ksvm_load_image(ksvm *vm, const uint8_t *image, uint32_t image_size,
                            uint8_t *ram, uint32_t ram_size);   /* conveniência: mapeia tudo */
void        ksvm_reset(ksvm *vm, uint32_t entry, uint32_t sp);
ksvm_status ksvm_step(ksvm *vm);
ksvm_status ksvm_run (ksvm *vm, uint32_t max_insns);           /* 0 = sem limite */
const char *ksvm_fault_name(ksvm_fault f);
```

Loop típico em microcontrolador:
```c
for (;;) {
    ksvm_run(&vm, 1000);          /* fatia de tempo */
    if (vm.status != KSVM_RUNNING && vm.status != KSVM_YIELD) break;
    hal_poll();                   /* UART, timers, watchdog */
}
```

## 5. Ciclo de execução (`exec.c`)

```
ksvm_step:
  1. if (pc & 3) → FAULT_PC_ALIGN
  2. fetch32(pc) via bus com perm EXEC → senão FAULT_PC_RANGE
  3. op = word & 0xFF; len = ksvm_isa_len(op)  (tabela: formato, tamanho, flags)
  4. if len == 8: imm32 = palavra em pc+4, exigindo [pc, pc+8) inteiro numa ÚNICA região
     EXEC (ksvm_bus_fetch_imm32) → senão FAULT_PC_RANGE (região vizinha não serve; sem wrap)
  5. if format == INVALID → FAULT_BAD_OPCODE
  6. decode campos conforme formato (ksvm_decode → struct ksvm_insn)
  7. next_pc = pc + len; se der a volta (pc + len < pc) → FAULT_PC_RANGE (doc 04 §5.6)
  8. switch (op) { ... }  — cada case chama um helper do alu.c/bus.c; desvios escrevem next_pc
  9. r[0] = 0 (após cada instrução, garante o registrador zero sem if em cada escrita)
 10. pc = next_pc; insn_count++
 11. trace hook (se habilitado)
```

Decisão: **`switch` simples**, sem *computed goto* na v2.0. GCC/Clang já geram jump table;
AVR-GCC também. *Computed goto* fica como otimização opcional atrás de `KSVM_DISPATCH_GOTO`
na fase 5, porque não é C padrão.

## 6. Tabela única da ISA (`isa.def`)

```c
/*      nome    opcode  formato  flags-afetadas  descrição                       */
KSVM_OP(NOP,    0x00,   R,       0,     "sem operação")
KSVM_OP(MOV,    0x01,   R,       0,     "rd = rs")
KSVM_OP(MOVI,   0x02,   I,       0,     "rd = sext(imm14)")
KSVM_OP(MOVIL,  0x03,   L,       0,     "rd = imm32")
KSVM_OP(ADD,    0x10,   R,       ZNCV,  "rd = ra + rb")
...
```

Cada consumidor define `KSVM_OP` do seu jeito e inclui o arquivo:
- `isa.h` → `enum ksvm_op { KSVM_OP_NOP = 0x00, ... }`
- `isa.c` → `const ksvm_isa_info ksvm_isa[256]` (formato, tamanho em bytes, nome)
- `ksas` → tabela mnemônico → opcode/formato
- `ksdis` → opcode → mnemônico/formato
- `docs/04-isa.md` → pode ser regenerado por script (fase 5)

Resultado: adicionar uma instrução = 1 linha no `.def` + 1 `case` no `exec.c`. Se a forma
dos operandos não é a padrão do formato (R padrão = `rd, ra, rb`), ela entra também na lista
de formas de `src/tools/ksas/mnemonic.c` e de `src/core/disasm.c`, como `NEG`/`CMP`: foi o
caso das instruções de dois operandos e do `FCMP` da extensão F.

## 7. Configuração (`config.h`)

| Macro | Padrão | Descrição |
|-------|--------|-----------|
| `KSVM_MAX_REGIONS` | 4 | Regiões no barramento |
| `KSVM_ENABLE_TRACE` | 0 | Hook por instrução |
| `KSVM_ENABLE_MULDIV` | 1 | MUL/DIV/REM (desligar economiza flash em AVR se o programa não usa) |
| `KSVM_ENABLE_BITOPS` | 1 | CLZ/CTZ/POPCNT |
| `KSVM_ENABLE_FLOAT` | 1 | Extensão F (binary32 em soft-float, doc 04 §5.7); a porta AVR usa 0 (não cabe nos 12 KB) |
| `KSVM_NO_INT64` | 0 | MULH/MULHU e FMUL/FDIV sem `uint64_t` |
| `KSVM_ENABLE_SYSCALL_FS` | 0 | OPEN/CLOSE/READ/WRITE em fd |
| `KSVM_CHECK_STACK` | 1 | Verifica `stack_limit` em PUSH/CALL |
| `KSVM_FAULT_NAMES` | 1 | Strings de nome de fault (desligar em AVR) |
| `KSVM_TABLE_PROGMEM` | 0 | Tabelas constantes em espaço separado (flash do AVR); exige `KSVM_TABLE_ATTR` e `KSVM_TABLE_READ_U8/U16/PTR` no `ksvm_config_user.h` (doc 08 §3) |
| `KSVM_ISA_COMPACT` | 0 | Tabela da ISA com 1 byte por opcode (`ksvm_isa_compact[]`; `ksvm_isa[]` deixa de existir); leitura só por `ksvm_isa_get`/`_format`/`_len` |

O host pode fornecer `ksvm_config_user.h` (incluído se `KSVM_HAVE_USER_CONFIG`).

## 8. Convenções de código

- Um módulo = um par `.h`/`.c`; `.h` só inclui o que precisa; guards `KSVM_<MOD>_H`.
- Tipos fixos: `uint8_t/uint16_t/uint32_t/int32_t`. Nunca `int`/`long` para dados do guest
  (`int` é 16 bits no AVR).
- Funções pequenas (≤ 40 linhas), um nível de abstração por função.
- Sem macros com efeitos colaterais; preferir `static inline` em `bytes.h`.
- Erros retornam `int` (0 = ok, negativo = `KSVM_E_*`) ou mudam `vm->status`.
- Comentários explicam o *porquê*; o *o quê* vem do nome da função.
- Sem variáveis globais mutáveis. Tabelas `static const`.
- Cada arquivo compila isoladamente (teste: `gcc -c arquivo.c` sem incluir outros `.c`).
