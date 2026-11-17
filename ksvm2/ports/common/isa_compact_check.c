/*
 * isa_compact_check.c — confere, no host, que a tabela compacta da ISA
 * (KSVM_ISA_COMPACT=1, usada pela porta AVR) devolve por ksvm_isa_get o
 * mesmo formato, tamanho, flags e nome que isa.def define, para os 256
 * opcodes. Compilado com -DKSVM_ISA_COMPACT=1 (mk/ports.mk).
 */
#include <stdio.h>
#include <string.h>

#include "ksvm/isa.h"

#if !KSVM_ISA_COMPACT
#error "compile com -DKSVM_ISA_COMPACT=1"
#endif

typedef struct expect {
    uint8_t     format, len, flags;
    const char *name;
} expect;

int main(void)
{
    static expect want[256];
    unsigned op, bad = 0;

#define KSVM_OP(nm, code, fmt, fl, desc)                                    \
    want[code].format = KSVM_FMT_##fmt;                                     \
    want[code].len = (KSVM_FMT_##fmt == KSVM_FMT_L) ? 8u : 4u;              \
    want[code].flags = (uint8_t)KSVM_AFFECTS_##fl;                          \
    want[code].name = #nm;
#include "ksvm/isa.def"
#undef KSVM_OP

    for (op = 0; op < 256u; op++) {
        ksvm_isa_info got = ksvm_isa_get((uint8_t)op);
        const char *name = ksvm_op_name((uint8_t)op);
        int name_ok = (want[op].name == NULL) ? name == NULL
                      : (name != NULL && strcmp(name, want[op].name) == 0);
        if (got.format != want[op].format || got.len != want[op].len ||
            got.flags != want[op].flags || !name_ok ||
            ksvm_isa_format((uint8_t)op) != want[op].format ||
            ksvm_isa_len((uint8_t)op) != want[op].len) {
            printf("opcode 0x%02X diverge\n", op);
            bad++;
        }
    }
    printf("isa_compact_check: %u opcodes, %u divergencias\n", op, bad);
    return bad == 0u ? 0 : 1;
}
