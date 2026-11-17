/*
 * trace.h — hook opcional chamado a cada instrução (KSVM_ENABLE_TRACE=1).
 *
 * Com a chave em 0 os campos nem existem em struct ksvm: custo zero em AVR.
 */
#ifndef KSVM_TRACE_H
#define KSVM_TRACE_H

#include "types.h"

struct ksvm;

typedef enum ksvm_trace_event {
    KSVM_TRACE_STEP = 0,   /* após executar a instrução em "pc" */
    KSVM_TRACE_BRK  = 1    /* instrução BRK em "pc" (antes de continuar) */
} ksvm_trace_event;

typedef void (*ksvm_trace_fn)(void *ctx, const struct ksvm *vm,
                              ksvm_trace_event ev, uint32_t pc, uint32_t word);

#if KSVM_ENABLE_TRACE
/* Instala (ou remove, com fn == NULL) o hook. */
void ksvm_set_trace(struct ksvm *vm, ksvm_trace_fn fn, void *ctx);
#endif

#endif /* KSVM_TRACE_H */
