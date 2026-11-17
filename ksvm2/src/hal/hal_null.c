/*
 * hal_null.c — HAL determinística para testes (ver hal_null.h).
 */
#include <string.h>

#include "ksvm/hal_null.h"
#include "ksvm/vm.h"

static int null_putc(void *ctx, uint8_t c)
{
    ksvm_hal_null *nh = (ksvm_hal_null *)ctx;

    if (nh->out_len < KSVM_HAL_NULL_OUT_MAX) {
        nh->out[nh->out_len] = c;
        nh->out_len++;
    }
    nh->out_total++;
    return 0;                 /* descartar o excedente não é erro */
}

static int null_getc(void *ctx)
{
    ksvm_hal_null *nh = (ksvm_hal_null *)ctx;

    if (nh->in_pos < nh->in_len) {
        uint8_t c = nh->in[nh->in_pos];
        nh->in_pos++;
        return c;
    }
    return nh->in_eof ? KSVM_HAL_EOF : KSVM_HAL_NODATA;
}

/* Relógio = instruções executadas + tempo "dormido": determinístico. */
static uint32_t null_millis(void *ctx)
{
    const ksvm_hal_null *nh = (const ksvm_hal_null *)ctx;
    uint32_t insns = (nh->vm != NULL) ? nh->vm->insn_count : 0u;

    return insns + nh->slept_ms;
}

static void null_sleep_ms(void *ctx, uint32_t ms)
{
    ksvm_hal_null *nh = (ksvm_hal_null *)ctx;
    nh->slept_ms += ms;
}

static void null_on_halt(void *ctx, uint32_t code)
{
    ksvm_hal_null *nh = (ksvm_hal_null *)ctx;
    nh->halted = true;
    nh->halt_code = code;
}

void ksvm_hal_null_init(ksvm_hal_null *nh, ksvm_hal *hal, const struct ksvm *vm)
{
    memset(nh, 0, sizeof *nh);
    memset(hal, 0, sizeof *hal);
    nh->vm = vm;
    hal->ctx = nh;
    hal->putc = null_putc;
    hal->getc = null_getc;
    hal->millis = null_millis;
    hal->sleep_ms = null_sleep_ms;
    hal->syscall = NULL;
    hal->on_halt = null_on_halt;
}

void ksvm_hal_null_set_input(ksvm_hal_null *nh, const uint8_t *in,
                             uint32_t len, bool eof)
{
    nh->in = in;
    nh->in_len = len;
    nh->in_pos = 0u;
    nh->in_eof = eof;
}
