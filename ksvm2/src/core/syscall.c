/*
 * syscall.c — syscalls dos grupos 0, 1, 2 e 5 (doc 06 §3) sobre a HAL, e
 * encaminhamento das demais para hal->syscall.
 *
 * Ponteiros do guest são endereços do guest: toda faixa é validada pelo
 * barramento antes do primeiro byte ser tocado, então um ponteiro ruim vira
 * erro para o programa (r1 = -1, r2 = KSVM_ERR_FAULT), nunca fault da VM.
 *
 * Os membros putc/getc da HAL são chamados entre parênteses para não
 * expandir eventuais macros putc()/getc() da libc do host (ver hal.h).
 */
#include "ksvm/syscall.h"

/* ---- Helpers públicos ------------------------------------------------- */

uint32_t ksvm_arg(const ksvm *vm, unsigned n)
{
    return (n >= 1u && n <= 6u) ? vm->cpu.r[n] : 0u;
}

void ksvm_ret(ksvm *vm, uint32_t value, uint32_t err)
{
    vm->cpu.r[1] = value;
    vm->cpu.r[2] = err;
}

int ksvm_copy_from_guest(ksvm *vm, void *dst, uint32_t gaddr, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;

    if (len == 0u) {
        return KSVM_OK;
    }
    if (dst == NULL) {
        return KSVM_E_ARG;
    }
    if (ksvm_bus_check(&vm->bus, gaddr, len, KSVM_MEM_READ) != KSVM_FAULT_NONE) {
        return KSVM_E_RANGE;
    }
    for (i = 0; i < len; i++) {
        (void)ksvm_bus_read8(&vm->bus, gaddr + i, &d[i]);   /* faixa já validada */
    }
    return KSVM_OK;
}

int ksvm_copy_to_guest(ksvm *vm, uint32_t gaddr, const void *src, uint32_t len)
{
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;

    if (len == 0u) {
        return KSVM_OK;
    }
    if (src == NULL) {
        return KSVM_E_ARG;
    }
    if (ksvm_bus_check(&vm->bus, gaddr, len, KSVM_MEM_WRITE) != KSVM_FAULT_NONE) {
        return KSVM_E_RANGE;
    }
    for (i = 0; i < len; i++) {
        (void)ksvm_bus_write8(&vm->bus, gaddr + i, s[i]);
    }
    return KSVM_OK;
}

uint8_t *ksvm_guest_ptr(ksvm *vm, uint32_t gaddr, uint32_t len, uint8_t perm)
{
    ksvm_region *r;

    if (len == 0u) {
        return NULL;
    }
    r = ksvm_bus_find(&vm->bus, gaddr, len);
    if (r == NULL || r->kind != KSVM_REGION_RAM || r->read8 != NULL ||
        r->write8 != NULL || (r->perm & perm) != perm) {
        return NULL;
    }
    return (uint8_t *)r->backing + (gaddr - r->base);
}

/* ---- Grupo 0: núcleo -------------------------------------------------- */

static void ret_error(ksvm *vm, uint32_t err)
{
    ksvm_ret(vm, KSVM_SYS_RET_ERROR, err);
}

/* CAPS só conhece as syscalls do núcleo; números encaminhados ao HAL
 * respondem 0 (a struct ksvm_hal não tem como consultar capacidades). */
static bool core_supports(const ksvm *vm, uint32_t num)
{
    const ksvm_hal *h = vm->hal;

    switch (num) {
    case KSVM_SYSCALL_EXIT:     case KSVM_SYSCALL_CAPS:
    case KSVM_SYSCALL_VERSION:  case KSVM_SYSCALL_YIELD:
    case KSVM_SYSCALL_ABORT:    case KSVM_SYSCALL_SLEEP_MS:
    case KSVM_SYSCALL_MEM_INFO: case KSVM_SYSCALL_RANDOM:
        return true;
    case KSVM_SYSCALL_PUTC: case KSVM_SYSCALL_PUTS:
        return h->putc != NULL;
    case KSVM_SYSCALL_GETC: case KSVM_SYSCALL_GETS:
        return h->getc != NULL;
    case KSVM_SYSCALL_MILLIS:
        return h->millis != NULL;
    default:
        return false;
    }
}

/* r1 = número de syscall -> 1/0; r1 = KSVM_CAPS_ISA -> máscara das
 * extensões da ISA (syscall.h). */
static void sys_caps(ksvm *vm)
{
    uint32_t q = ksvm_arg(vm, 1u);

    ksvm_ret(vm, (q == KSVM_CAPS_ISA) ? KSVM_CAPS_ISA_BITS : (core_supports(vm, q) ? 1u : 0u),
             KSVM_ERR_OK);
}

static uint32_t version_word(void)
{
    return ((uint32_t)KSVM_VERSION_MAJOR << 16) |
           ((uint32_t)KSVM_VERSION_MINOR << 8) |
           (uint32_t)KSVM_VERSION_PATCH;
}

/* ---- Grupo 1: console ------------------------------------------------- */

static void sys_putc(ksvm *vm)
{
    const ksvm_hal *h = vm->hal;

    if (h->putc == NULL) {
        ret_error(vm, KSVM_ERR_NOSYS);
    } else if ((h->putc)(h->ctx, (uint8_t)(ksvm_arg(vm, 1u) & 0xFFu)) < 0) {
        ret_error(vm, KSVM_ERR_IO);
    } else {
        ksvm_ret(vm, 0u, KSVM_ERR_OK);
    }
}

static void sys_getc(ksvm *vm)
{
    const ksvm_hal *h = vm->hal;
    int c;

    if (h->getc == NULL) {
        ret_error(vm, KSVM_ERR_NOSYS);
        return;
    }
    c = (h->getc)(h->ctx);
    if (c >= 0) {
        ksvm_ret(vm, (uint32_t)c & 0xFFu, KSVM_ERR_OK);
    } else {
        ret_error(vm, (c == KSVM_HAL_EOF) ? KSVM_ERR_EOF : KSVM_ERR_AGAIN);
    }
}

/* r1 = ptr, r2 = len. Devolve quantos bytes o host aceitou. */
static void sys_puts(ksvm *vm)
{
    const ksvm_hal *h = vm->hal;
    uint32_t ptr = ksvm_arg(vm, 1u), len = ksvm_arg(vm, 2u), i;

    if (h->putc == NULL) {
        ret_error(vm, KSVM_ERR_NOSYS);
        return;
    }
    if (len != 0u && ksvm_bus_check(&vm->bus, ptr, len, KSVM_MEM_READ) != KSVM_FAULT_NONE) {
        ret_error(vm, KSVM_ERR_FAULT);
        return;
    }
    for (i = 0; i < len; i++) {
        uint8_t b = 0;
        (void)ksvm_bus_read8(&vm->bus, ptr + i, &b);        /* faixa já validada */
        if ((h->putc)(h->ctx, b) < 0) {
            break;
        }
    }
    if (i == 0u && len != 0u) {
        ret_error(vm, KSVM_ERR_IO);
    } else {
        ksvm_ret(vm, i, KSVM_ERR_OK);
    }
}

/* r1 = ptr, r2 = max. Lê até '\n' (incluso), max bytes ou falta de dado;
 * 0 se nada disponível, -1/EOF se a entrada acabou sem ler nada. */
static void sys_gets(ksvm *vm)
{
    const ksvm_hal *h = vm->hal;
    uint32_t ptr = ksvm_arg(vm, 1u), max = ksvm_arg(vm, 2u), n = 0u;
    int c = 0;

    if (h->getc == NULL) {
        ret_error(vm, KSVM_ERR_NOSYS);
        return;
    }
    if (max != 0u && ksvm_bus_check(&vm->bus, ptr, max, KSVM_MEM_WRITE) != KSVM_FAULT_NONE) {
        ret_error(vm, KSVM_ERR_FAULT);
        return;
    }
    while (n < max) {
        c = (h->getc)(h->ctx);
        if (c < 0) {
            break;
        }
        (void)ksvm_bus_write8(&vm->bus, ptr + n, (uint8_t)c);
        n++;
        if (c == '\n') {
            break;
        }
    }
    if (n == 0u && c == KSVM_HAL_EOF) {
        ret_error(vm, KSVM_ERR_EOF);
    } else {
        ksvm_ret(vm, n, KSVM_ERR_OK);
    }
}

/* ---- Grupo 2: tempo --------------------------------------------------- */

static void sys_millis(ksvm *vm)
{
    const ksvm_hal *h = vm->hal;

    if (h->millis == NULL) {
        ret_error(vm, KSVM_ERR_NOSYS);
    } else {
        ksvm_ret(vm, h->millis(h->ctx), KSVM_ERR_OK);
    }
}

/* Sem sleep_ms no host, dormir vira YIELD: o host decide quanto esperar. */
static void sys_sleep_ms(ksvm *vm)
{
    const ksvm_hal *h = vm->hal;
    uint32_t ms = ksvm_arg(vm, 1u);      /* ler antes: ksvm_ret sobrescreve r1 */

    if (h->sleep_ms != NULL) {
        h->sleep_ms(h->ctx, ms);
    } else {
        vm->status = KSVM_YIELD;
    }
    ksvm_ret(vm, 0u, KSVM_ERR_OK);
}

/* ---- Grupo 5: memória ------------------------------------------------- */

/* r1 = base da RAM, r2 = tamanho, r3 = heap_start, r4 = stack_limit. A RAM
 * é a região RAM que contém heap_start (a do programa carregado, mesmo que
 * o host tenha mapeado outra RAM antes); senão a primeira região RAM do
 * barramento, que as mantém à frente (zeros se não houver). */
static void sys_mem_info(ksvm *vm)
{
    const ksvm_region *r = ksvm_bus_find(&vm->bus, vm->heap_start, 1u);
    uint32_t base = 0u, size = 0u;

    if (r == NULL || r->kind != KSVM_REGION_RAM) {
        r = &vm->bus.regions[0];
    }
    if (vm->bus.count != 0u && r->kind == KSVM_REGION_RAM) {
        base = r->base;
        size = r->size;
    }
    vm->cpu.r[1] = base;
    vm->cpu.r[2] = size;
    vm->cpu.r[3] = vm->heap_start;
    vm->cpu.r[4] = vm->stack_limit;
}

/* xorshift32 (Marsaglia): determinístico, sem multiplicação; o estado
 * nunca pode ser 0 (ponto fixo), por isso a ressemeadura. */
static void sys_random(ksvm *vm)
{
    uint32_t x = (vm->rng_state != 0u) ? vm->rng_state : KSVM_RNG_SEED;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    vm->rng_state = x;
    ksvm_ret(vm, x, KSVM_ERR_OK);
}

/* ---- Despacho --------------------------------------------------------- */

static void sys_forward(ksvm *vm, uint32_t num)
{
    int r = KSVM_SYS_UNKNOWN;

    if (vm->hal->syscall != NULL) {
        r = vm->hal->syscall(vm, num);
    }
    if (r == KSVM_SYS_UNKNOWN) {
        ret_error(vm, KSVM_ERR_NOSYS);
    } else if (r == KSVM_SYS_YIELD && vm->status == KSVM_RUNNING) {
        vm->status = KSVM_YIELD;
    }
}

void ksvm_syscall(ksvm *vm, uint32_t num)
{
    switch (num) {
    case KSVM_SYSCALL_EXIT:     ksvm_halt(vm, ksvm_arg(vm, 1u)); break;
    case KSVM_SYSCALL_CAPS:     sys_caps(vm); break;
    case KSVM_SYSCALL_VERSION:  ksvm_ret(vm, version_word(), KSVM_ERR_OK); break;
    case KSVM_SYSCALL_YIELD:    vm->status = KSVM_YIELD; break;
    case KSVM_SYSCALL_ABORT:    ksvm_set_fault(vm, KSVM_FAULT_TRAP, ksvm_arg(vm, 1u)); break;
    case KSVM_SYSCALL_PUTC:     sys_putc(vm); break;
    case KSVM_SYSCALL_GETC:     sys_getc(vm); break;
    case KSVM_SYSCALL_PUTS:     sys_puts(vm); break;
    case KSVM_SYSCALL_GETS:     sys_gets(vm); break;
    case KSVM_SYSCALL_MILLIS:   sys_millis(vm); break;
    case KSVM_SYSCALL_SLEEP_MS: sys_sleep_ms(vm); break;
    case KSVM_SYSCALL_MEM_INFO: sys_mem_info(vm); break;
    case KSVM_SYSCALL_RANDOM:   sys_random(vm); break;
    default:                    sys_forward(vm, num); break;
    }
}
