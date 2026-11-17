#!/bin/sh
# test_runner.sh — testes de ponta a ponta de ksvm, ksdis e ksimg.
#
#   sh tests/tools/test_runner.sh BIN_DIR MKIMG [EXE]
#
# BIN_DIR tem ksvm/ksdis/ksimg (com sufixo EXE, ex.: .exe para os binários
# MinGW, no Git Bash do Windows ou pelo interop do WSL); MKIMG é o gerador
# de imagens (tests/tools/mkimg.c). Variáveis opcionais: HOSTCC (compilador
# para o teste do ksimg, padrão cc) e KSAS (assembler: liga o round-trip
# ksdis --raw -> ksas). No Windows sem WSL: sh tests/tools/test_runner.sh
# build/win build/win/mkimg.exe .exe (Git Bash), depois de make tools-win.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=$(cd "$1" && pwd)
MKIMG=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
EXE=${3:-}
KSVM=$BIN/ksvm$EXE
KSDIS=$BIN/ksdis$EXE
KSIMG=$BIN/ksimg$EXE
WORK=$BIN/runner-tmp
HOSTCC=${HOSTCC:-cc}

total=0
failed=0

fail() {
    failed=$((failed + 1))
    echo "FALHA [$name] $*"
    if [ -s _stderr ]; then sed 's/^/    stderr: /' _stderr | head -5; fi
}

# begin NOME: inicia um caso (o nome aparece nas falhas).
begin() {
    name=$1
    total=$((total + 1))
}

# run ENTRADA CMD...: executa com stdin de ENTRADA; captura stdout, stderr, rc.
run() {
    input=$1
    shift
    "$@" <"$input" >_stdout 2>_stderr
    rc=$?
}

expect_rc() {
    [ "$rc" -eq "$1" ] || fail "codigo de saida $rc, esperado $1"
}

# expect_stdout TEXTO (printf-escapes permitidos)
expect_stdout() {
    printf "$1" >_want
    cmp -s _want _stdout || fail "stdout '$(cat _stdout)', esperado '$(cat _want)'"
}

expect_in() {   # expect_in ARQUIVO TEXTO
    grep -qF -- "$2" "$1" || fail "'$2' ausente em $1"
}

expect_re() {   # expect_re ARQUIVO REGEX
    grep -qE -- "$2" "$1" || fail "/$2/ ausente em $1"
}

rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK" || exit 1
for p in hello halt42 fault trap loop echo files stack rawedge; do
    "$MKIMG" "$p" "$p.ksx" || { echo "mkimg $p falhou"; exit 1; }
done
: >empty

# ---- ksvm: saída, código de saída, faults ---------------------------------

begin hello
run empty "$KSVM" hello.ksx
expect_rc 3
expect_stdout 'Hello, KSVM!\n!\n'

begin halt42
run empty "$KSVM" halt42.ksx
expect_rc 42
expect_stdout ''

begin fault
run empty "$KSVM" fault.ksx
expect_rc 74
expect_in _stderr 'fault: MEM_RANGE at pc=0x00000008 addr=0x80000000'

begin trap
run empty "$KSVM" trap.ksx
expect_rc 78
expect_in _stderr 'fault: TRAP at pc=0x00000000 addr=0x00000005'

begin max-insns
run empty "$KSVM" --max-insns 1000 loop.ksx
expect_rc 124
expect_in _stderr 'limite de 1000 instrucoes'

begin stack-ok
run empty "$KSVM" stack.ksx
expect_rc 0

begin stack-override
run empty "$KSVM" --stack 8 stack.ksx
expect_rc 77
expect_in _stderr 'fault: STACK at pc=0x00000008'

begin small-ram
run empty "$KSVM" --ram 64 hello.ksx
expect_rc 65
expect_in _stderr 'NO_RAM'

begin missing-file
run empty "$KSVM" nao_existe.ksx
expect_rc 66

begin bad-image
head -c 50 hello.ksx >bad.ksx
run empty "$KSVM" bad.ksx
expect_rc 65
expect_in _stderr 'imagem invalida'

begin corrupt-checksum
cp hello.ksx corrupt.ksx
printf 'X' | dd of=corrupt.ksx bs=1 seek=100 conv=notrunc 2>/dev/null
run empty "$KSVM" corrupt.ksx
expect_rc 65
expect_in _stderr 'IMAGE_CHECKSUM'

begin usage
run empty "$KSVM" --ram
expect_rc 64
run empty "$KSVM" --bogus hello.ksx
expect_rc 64

# Bordas: halt42 executa exatamente 4 instruções (a 4a é o HALT).
begin max-insns-exact
run empty "$KSVM" --max-insns 4 halt42.ksx
expect_rc 42
run empty "$KSVM" --max-insns 3 halt42.ksx
expect_rc 124
expect_in _stderr 'limite de 3 instrucoes atingido (pc=0x0000000C)'

begin bad-numbers
for args in "--ram 0" "--ram 2g" "--ram 1x" "--ram -1" "--max-insns abc" "--stack 0x1_0000_0000"; do
    # shellcheck disable=SC2086
    run empty "$KSVM" $args halt42.ksx
    expect_rc 64
done
run empty "$KSVM" --ram 1k --max-insns 0x10 halt42.ksx
expect_rc 42

begin empty-and-two-files
run empty "$KSVM" empty
expect_rc 65
run empty "$KSVM" halt42.ksx hello.ksx
expect_rc 64

# ---- ksvm: entrada --------------------------------------------------------

printf 'abc\nde' >input.txt
begin echo-block
run input.txt "$KSVM" echo.ksx
expect_rc 0
expect_stdout 'abc\nde'

begin echo-poll-pipe
printf 'xyz' | "$KSVM" --stdin-poll echo.ksx >_stdout 2>_stderr
rc=$?
expect_rc 0
expect_stdout 'xyz'

# ---- ksvm: arquivos (grupo 3) ---------------------------------------------

begin files
rm -f out.txt
run empty "$KSVM" files.ksx
expect_rc 3                       # OPEN "../x" recusado com INVAL (3)
expect_stdout 'file!\n'
printf 'hello file!\n' >_want
cmp -s _want out.txt || fail "conteudo de out.txt"

begin files-no-fs
run empty "$KSVM" --no-fs files.ksx
expect_rc 11                      # 10 + KSVM_ERR_NOSYS

# ---- ksvm: trace, registradores, step -------------------------------------

begin trace
run empty "$KSVM" --trace halt42.ksx
expect_rc 42
expect_re _stderr '^00000000: 02 01 A8 00 +movi r1, 42 +----'
expect_re _stderr '^00000004: 39 01 A8 00 +cmpi r1, 42 +Z-C-'
expect_re _stderr '^0000000C: 76 00 00 00 +halt'
run empty "$KSVM" --trace hello.ksx
expect_re _stderr '^00000000: 03 01 00 00 30 00 00 00 +movil r1, 0x30'
expect_re _stderr '^00000010: 73 02 00 00 +call \+0x8'
expect_stdout 'Hello, KSVM!\n!\n'

begin regs
run empty "$KSVM" --regs halt42.ksx
expect_rc 42
expect_in _stderr 'brk at pc=0x00000008'
expect_in _stderr 'r1=0x0000002A'
expect_in _stderr 'pc=0x00000010 flags=Z-C- insns=4'

if [ -z "$EXE" ] && command -v setsid >/dev/null 2>&1; then
    begin step                    # sem terminal: o Enter vem do stdin
    printf '\n\nq\n' >keys
    setsid -w "$KSVM" --step halt42.ksx <keys >_stdout 2>_stderr
    rc=$?
    expect_rc 130
    expect_in _stderr '[step]'
    expect_re _stderr '00000004: 39 01 A8 00 +cmpi r1, 42'
fi

# ---- ksdis ----------------------------------------------------------------

begin ksdis-listing
run empty "$KSDIS" hello.ksx
expect_rc 0
expect_in _stdout '; checksum     0x'
expect_in _stdout '; simbolos     off 0x00000080  39 bytes  (4 registros)'
expect_re _stdout '^_start:'
expect_re _stdout '^ +00000000:  03 01 00 00 30 00 00 00  movil r1, 0x30'
expect_re _stdout 'call sub +; -> 0x0000001C'
expect_re _stdout '^ +00000030:  48 65 6C .*\|Hello, KSVM!\.\|'
expect_re _stdout '^buf:'

begin ksdis-raw
run empty "$KSDIS" --raw hello.ksx
expect_rc 0
expect_re _stdout '^ +\.entry _start'
expect_re _stdout '^ +\.global msg'
grep -qE '^ +\.global sub' _stdout && fail "sub e local (kind sem bit global)"
expect_in _stdout 'remonte com ksas -g'
expect_re _stdout '^sub:'
expect_re _stdout '^ +call sub'
expect_re _stdout '^ +\.byte 0x48, 0x65'
expect_re _stdout '^ +\.space 16'
grep -qE '^ +[0-9A-F]{8}:' _stdout && fail "--raw contem enderecos"
run empty "$KSDIS" --raw echo.ksx
expect_re _stdout '^L_0014:'
expect_re _stdout '^ +beq L_0014'
expect_re _stdout '^ +bal L_0000'
expect_re _stdout '^ +\.entry L_0000'

begin ksdis-errors
run empty "$KSDIS" bad.ksx
expect_rc 1
run empty "$KSDIS"
expect_rc 2

# ---- ksimg ----------------------------------------------------------------

begin ksimg
run empty "$KSIMG" hello.ksx -o img.h --name hello_img
expect_rc 0
expect_in img.h 'static const uint8_t hello_img[] = {'
expect_in img.h "#define hello_img_SIZE $(wc -c <hello.ksx | tr -d ' ')u"
cat >img_main.c <<'EOF'
#include <stdio.h>
#include "img.h"
int main(void)
{
    return fwrite(hello_img, 1, hello_img_SIZE, stdout) == hello_img_SIZE ? 0 : 1;
}
EOF
if command -v "$HOSTCC" >/dev/null 2>&1; then
    "$HOSTCC" -std=c99 -Wall -Wextra -Wpedantic -Werror img_main.c -o img_main 2>_stderr ||
        fail "header gerado nao compila"
    ./img_main >img_copy.ksx && cmp -s img_copy.ksx hello.ksx || fail "array difere do .ksx"
    run empty "$KSIMG" hello.ksx --progmem -o img.h --name hello_img
    expect_in img.h 'static const uint8_t PROGMEM hello_img[] = {'
    "$HOSTCC" -std=c99 -Wall -Wextra -Wpedantic -Werror img_main.c -o img_main 2>_stderr ||
        fail "header --progmem nao compila no host"
fi
run empty "$KSIMG" hello.ksx --name 9abc     # nome invalido: erro de uso
expect_rc 2
run empty "$KSIMG" bad.ksx -o img2.h
expect_rc 1

# ---- round-trip com o ksas (opcional) -------------------------------------
# Imagens do mkimg: x -> ksdis --raw -> ksas = y; y -> ksdis --raw -> ksas =
# z; y == z byte a byte (parte de y porque o layout de .data é escolha do
# ksas: ele alinha .data a 4, o mkimg não). Exemplos do ksas: ksas(a) = x,
# ksdis --raw x -> ksas = y, x == y, com e sem -g.

roundtrip() {   # roundtrip ENTRADA.ksx SAIDA.ksx
    "$KSDIS" --raw "$1" >"$1.s" || return 1
    if grep -q 'remonte com ksas -g' "$1.s"; then
        "$KSAS" "$1.s" -g -o "$2" 2>_stderr
    else
        "$KSAS" "$1.s" -o "$2" 2>_stderr
    fi
}

if [ -n "${KSAS:-}" ]; then
    for p in hello halt42 fault echo files loop stack rawedge; do
        begin "roundtrip-$p"
        roundtrip "$p.ksx" "$p.y.ksx" || { fail "ksas recusou $p.ksx.s"; continue; }
        roundtrip "$p.y.ksx" "$p.z.ksx" || { fail "ksas recusou $p.y.ksx.s"; continue; }
        cmp -s "$p.y.ksx" "$p.z.ksx" || fail "ksas(ksdis(y)) != y"
        run empty "$KSVM" --max-insns 100000 "$p.y.ksx"
        [ "$p" != hello ] || expect_stdout 'Hello, KSVM!\n!\n'
    done

    # rawedge: desvios com alvo fora de [0, code_size) saem como .word, "R1"
    # (registrador para o ksas) não vira label, e o código sai idêntico.
    begin roundtrip-rawedge-code
    expect_in rawedge.ksx.s '.word 0x00000370'
    expect_in rawedge.ksx.s '.word 0x00000072'
    grep -q '^R1:' rawedge.ksx.s && fail "R1 emitido como label"
    dd if=rawedge.ksx bs=1 skip=64 count=32 2>/dev/null >_code_x
    dd if=rawedge.y.ksx bs=1 skip=64 count=32 2>/dev/null >_code_y
    cmp -s _code_x _code_y || fail "codigo de rawedge mudou no round-trip"

    # Labels em endereços de fronteira entre seções (fim do código = início
    # da RAM, fim de .data = início de .bss, .bss vazio): com -g o kind de
    # seção de cada símbolo tem de sobreviver ao round-trip.
    begin roundtrip-boundary-labels
    printf '_start: halt\ntext_end:\n.data\ns: .asciz "abc"\ns_end:\n.bss\nbuf: .space 4\nbss_end:\n' >edge1.ksasm
    printf '_start: halt\n.data\nd: .word 1\n.bss\nb0:\n' >edge2.ksasm
    printf '_start: nop\nnop\nnop\nhalt\ntext_end:\n.data\nd0: .word 5\nd_end:\n' >edge3.ksasm
    for e in edge1 edge2 edge3; do
        "$KSAS" -g "$e.ksasm" -o "$e.x.ksx" 2>_stderr || { fail "ksas recusou $e"; continue; }
        roundtrip "$e.x.ksx" "$e.y.ksx" || { fail "ksas recusou $e.x.ksx.s"; continue; }
        cmp -s "$e.x.ksx" "$e.y.ksx" || fail "ksas(ksdis($e)) != $e"
    done

    # ---- ksas: linha de comando ----------------------------------------------
    printf 'li r1, VAL\nhalt\n' >def.ksasm
    begin ksas-cli
    run empty "$KSAS" def.ksasm -DVAL=7 -l def.lst
    expect_rc 0
    [ -f def.ksx ] || fail "saida padrao def.ksx ausente"
    expect_in def.lst 'li r1, VAL'
    run empty "$KSVM" def.ksx
    expect_rc 7
    run empty "$KSAS" def.ksasm -o def2.ksx
    expect_rc 1
    expect_in _stderr "def.ksasm:1:8: erro: simbolo 'VAL' nao definido"
    [ -f def2.ksx ] && fail "saida gerada apesar do erro"
    run empty "$KSAS" def.ksasm -o def3.ksx -l def3.lst
    expect_rc 1
    [ -e def3.lst ] && fail "listing criado apesar do erro"
    [ -e def3.ksx ] && fail "saida gerada apesar do erro (com -l)"
    run empty "$KSAS" nao_existe.ksasm
    expect_rc 2
    run empty "$KSAS" def.ksasm -x
    expect_rc 2

    # .include aninhado resolve relativo ao arquivo que inclui, mesmo com o
    # fonte principal fora do diretório corrente.
    begin ksas-nested-include
    mkdir -p inc/sub
    printf '.include "sub/a.inc"\nli r1, A\nhalt\n' >inc/main.ksasm
    printf '.include "b.inc"\nA = B + 1\n' >inc/sub/a.inc
    printf 'B = 41\n' >inc/sub/b.inc
    run empty "$KSAS" inc/main.ksasm -o inc.ksx
    expect_rc 0
    run empty "$KSVM" inc.ksx
    expect_rc 42
    printf 'nop\n  frob\n' >inc/sub/b.inc
    run empty "$KSAS" inc/main.ksasm -o inc.ksx
    expect_rc 1
    expect_in _stderr 'inc/sub/b.inc:2:3: erro: mnemonico desconhecido'

    for src in "$ROOT"/examples/*.ksasm "$ROOT"/examples/*/*.ksasm; do
        [ -f "$src" ] || continue
        for g in "" -g; do
            ex=$(basename "$src" .ksasm)$g
            begin "roundtrip-example-$ex"
            (cd "$(dirname "$src")" && "$KSAS" "$src" $g -o "$WORK/$ex.x.ksx") 2>_stderr ||
                { fail "ksas recusou o exemplo"; continue; }
            roundtrip "$ex.x.ksx" "$ex.y.ksx" || { fail "ksas recusou $ex.x.ksx.s"; continue; }
            cmp -s "$ex.x.ksx" "$ex.y.ksx" || fail "ksas(ksdis(x)) != x"
        done
    done
else
    echo "test_runner: KSAS nao definido, round-trip pulado"
fi

echo "test_runner ($KSVM): $total casos, $failed falhas"
[ "$failed" -eq 0 ]
