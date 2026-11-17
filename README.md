# KSVM v2

Máquina virtual de 32 bits em C99 portável (x86/x64, ARM, Cortex-M0, ATmega328P), com assembler,
disassembler e runner.

- `docs/` — especificação completa (análise da v1, requisitos, arquitetura, ISA, formato binário,
  syscalls/HAL, assembler, portabilidade, testes, roadmap). Comece por `docs/README.md`.
- `ksvm2/` — código-fonte, testes e portas. Build e verificação: `ksvm2/README.md` e `ksvm2/BUILD.md`.
