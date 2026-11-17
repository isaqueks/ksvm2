#!/bin/sh
# expect_run.sh — roda um simulador com timeout e confere a saída da UART.
#
#   expect_run.sh <nome> <log> <timeout_s> <esperado>... -- <comando> [args]
#
# Junta stdout e stderr do comando, remove sequências de cor ANSI (o simavr
# imprime a UART em verde) e grava em <log>. Passa se cada <esperado>
# aparecer literalmente (grep -F) na saída; senão mostra a saída e falha.
# O código de saída do simulador é ignorado de propósito: o simavr devolve
# não-zero ao encerrar por "sleep com interrupções desligadas", e quem
# decide o resultado é o que saiu na UART.
set -u

name=$1; log=$2; secs=$3
shift 3
n=0
while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do
    n=$((n + 1))
    eval "exp_$n=\$1"
    shift
done
if [ "$#" -eq 0 ]; then
    echo "expect_run: faltou '--' antes do comando" >&2
    exit 2
fi
shift

timeout "$secs" "$@" < /dev/null > "$log.raw" 2>&1
rc=$?
sed 's/\x1b\[[0-9;]*m//g' "$log.raw" > "$log"
rm -f "$log.raw"

ok=1
i=1
while [ "$i" -le "$n" ]; do
    eval "want=\$exp_$i"
    if ! grep -F -q -- "$want" "$log"; then
        echo "  FAIL $name: faltou \"$want\"" >&2
        ok=0
    fi
    i=$((i + 1))
done
if [ "$rc" -eq 124 ]; then
    echo "  ($name: timeout de ${secs}s)" >&2
fi
if [ "$ok" -eq 1 ]; then
    echo "  PASS $name"
    exit 0
fi
echo "  --- saida de $name ---" >&2
cat "$log" >&2
exit 1
