# Build e testes (ambiente de desenvolvimento)

O toolchain fica no WSL (Ubuntu 24.04). Não há compilador C no Windows.

Caminho do projeto dentro do WSL: `/mnt/c/Users/isaque.kanitz/KSVM2/ksvm2`

Executar qualquer comando de build a partir do Windows (Git Bash / PowerShell):

    wsl.exe -e bash -lc 'cd /mnt/c/Users/isaque.kanitz/KSVM2/ksvm2 && make test'
    wsl.exe -e bash -lc 'cd /mnt/c/Users/isaque.kanitz/KSVM2/ksvm2 && make check-all'

Disponível no WSL: gcc 13 (+ `-m32`), clang 18, make, cmake, avr-gcc + avr-libc + simavr,
arm-none-eabi-gcc + newlib, powerpc-linux-gnu-gcc + qemu-user (`qemu-ppc`, big-endian),
python3. `x86_64-w64-mingw32-gcc` (MinGW) e `qemu-system-arm` se a instalação terminou.

Alvos do Makefile:
- `make test` — testes unitários (configs padrão, "alt" e "min")
- `make test-asan` — ASan + UBSan, `-fno-sanitize-recover=all`
- `make check-freestanding` / `check-clang` / `check-m32` / `check-avr` / `check-arm`
- `make test-bigendian` — suíte inteira em PowerPC via qemu
- `make check-all` — definição de "verde": tudo acima mais `fuzz-quick`, `test-ksas`,
  `test-tools`, `test-ports` e `size-avr` (cada fragmento de `mk/` fornece os seus)
- Fragmentos `mk/*.mk` são incluídos automaticamente: cada ferramenta/porta adiciona
  seus alvos no próprio fragmento, NUNCA editando o Makefile principal.
- Portas embarcadas (`mk/ports.mk`, docs/08 §4.1–4.3): `make test-avr` (ATmega328P no
  simavr), `make test-arm` (Cortex-M0 no `qemu-system-arm -M mps2-an385`),
  `make test-ports` (ambos), `make size-avr` (flash/RAM por módulo e meta RNF-04 de 12 KB),
  `make ports-avr` / `make ports-arm` (só os `.elf` em `build/ports/`),
  `make avr-custom KSX=prog.ksx` / `make arm-custom KSX=prog.ksx` (firmware com um programa
  próprio). Pulam com aviso se o toolchain faltar. `size-avr` falha se a meta RNF-04
  (12 KB) estourar e faz parte do `check-all`.

Regras:
- Arquivos sempre com terminação LF (nunca CRLF), UTF-8 sem BOM.
- Flags obrigatórias do núcleo e das HALs: -std=c99 -Wall -Wextra -Wpedantic -Wconversion
  -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror
- Antes de declarar uma etapa pronta: `make check-all` verde.
