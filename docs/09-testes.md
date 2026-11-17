# 09 — Estratégia de testes

A v1 não tinha teste algum, e `STORE` ficou sem implementação sem ninguém notar. Na v2 cada
instrução nasce com teste.

## 1. Harness

Sem dependências: `tests/unit/test.h` com `TEST(nome)`, `ASSERT_EQ_U32(a, b)`,
`ASSERT_TRUE`, contador de falhas e `main` que lista e executa. Compila com o mesmo
`-std=c99 -Wall -Wextra -Werror` do núcleo, mais `-fsanitize=address,undefined` no CI.

Cada teste monta um programa com `ksvm_enc_*` (doc 07 §3) em um array, mapeia RAM, roda
`ksvm_step` N vezes e inspeciona `vm.cpu`. Exemplo da forma:

```c
TEST(add_sets_carry_and_zero) {
    uint32_t code[] = {
        ksvm_enc_l(KSVM_OP_MOVIL, 1, 0), 0xFFFFFFFFu,
        ksvm_enc_i(KSVM_OP_ADDI,  2, 1, 1),
        ksvm_enc_r(KSVM_OP_HALT,  0, 0, 0),
    };
    ksvm vm; fixture_load(&vm, code, sizeof code);
    ASSERT_EQ(ksvm_run(&vm, 0), KSVM_HALTED);
    ASSERT_EQ_U32(vm.cpu.r[2], 0);
    ASSERT_EQ_U32(vm.cpu.flags, KSVM_FLAG_Z | KSVM_FLAG_C);
}
```

## 2. Níveis

| Nível | Arquivo(s) | O que cobre |
|-------|------------|-------------|
| Bytes | `test_bytes.c` | `ld/st 16/32 le`, extensão de sinal 8/14/16/20/24 bits, valores extremos |
| ALU | `test_alu.c` | Cada operação × casos: 0, 1, −1, `INT32_MIN/MAX`, `UINT32_MAX`; **tabela de flags esperada** por operação (gerada à mão a partir do doc 04 §2). Comparar com resultado de referência calculado em `uint64_t` quando aplicável |
| Barramento | `test_bus.c` | Leitura/escrita nas bordas de cada região, cruzando bordas (fault), perm, região com `read8` callback, MMIO, sobreposição rejeitada |
| Decode | `test_decode.c` | Round-trip `enc → decode` para todos os formatos, todos os campos nos limites |
| Exec | `test_exec_mov.c`, `_mem.c`, `_stack.c`, `_arith.c`, `_logic.c`, `_branch.c`, `_sys.c` | Uma função de teste por instrução (≥ 90 testes). Para `Bcc`: todas as 16 condições × flags que satisfazem/não satisfazem |
| Faults | `test_faults.c` | Cada `KSVM_FAULT_*` provocado deliberadamente; estado da VM após o fault é inspecionável e `ksvm_step` não avança |
| Loader | `test_loader.c` | Cabeçalhos válidos/inválidos (magic, versão, checksum, ranges), `.data` copiado, `.bss` zerado, `sp`/`stack_limit` |
| Syscall | `test_syscall.c` | `hal_null` com buffers capturáveis; `PUTS` com ponteiro inválido retorna erro sem fault; `CAPS`; `YIELD` |
| Golden | `tests/golden/*.ksasm` + `.expected` | Programas inteiros montados com `ksas` e rodados com `ksvm` em `hal_posix`; stdout comparado byte a byte. Inclui os exemplos da v1 portados (`hello`, `ask_name`, `jg_test`, `strlen`) e novos: `fib`, `sieve`, `quicksort`, `recursion_depth`, `bitops`, `div_signed`, `str_funcs` |
| Round-trip | script | `ksas a.ksasm → a.ksx → ksdis → b.ksasm → ksas → b.ksx`; `a.ksx == b.ksx` |
| Fuzz | `fuzz_exec.c` | Preenche CODE com bytes pseudoaleatórios (semente fixa + modo libFuzzer opcional), roda 10⁶ passos; único resultado aceitável é HALTED/FAULT/limite de instruções. Nunca crash, nunca leitura fora de `ram[]` (ASan) |
| Tamanho | CI | `avr-size` do núcleo compilado para atmega328p comparado a um limite; falha se crescer acima de RNF-04 |
| Simulação | CI (opcional) | `simavr` rodando `blink`/`hello_uart` por N ciclos e checando a saída da UART; `qemu-system-arm` com `hal_arm_cm` semihosting |

## 3. Oráculo de referência para a ALU

Para evitar que teste e implementação compartilhem o mesmo erro, `test_alu.c` calcula
o esperado de forma independente: soma e subtração em `uint64_t`, sinais via comparação
de `int64_t`, e confere resultado **e** cada flag. A tabela de verdade de `Bcc` é
escrita literalmente (16 linhas) copiada do doc 04 §4.

## 4. Integração contínua

`make test` roda os testes unitários em três configurações (padrão; "alt": trace, MULH sem
`uint64_t`, 3 regiões; "min": sem MUL/DIV e BITOPS, sem nomes, tabela compacta da ISA).
`make check-all` é a definição de "verde": adiciona sanitizers, `-m32`, clang,
freestanding, cross-compiles AVR/ARM, big-endian (qemu-ppc), fuzz curto (`fuzz-quick`),
testes do assembler (`test-ksas`, com round-trip), das ferramentas (`test-tools`) e das
portas no simavr/QEMU (`test-ports`). Não há CI hospedado: `make check-all` verde é
pré-requisito para declarar uma etapa pronta (ver `ksvm2/BUILD.md`).

## 5. Cobertura

`gcov`/`llvm-cov` no alvo x86-64; meta: 100% das linhas de `exec.c` e `alu.c` (cada
`case` tem teste), ≥ 90% no resto do núcleo.
