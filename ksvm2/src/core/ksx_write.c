/*
 * ksx_write.c — escritor mínimo de .ksx em memória (layout em ksx_write.h).
 */
#include <string.h>

#include "ksvm/bytes.h"
#include "ksvm/ksx_write.h"
#include "ksvm/loader.h"

typedef struct ksx_layout {
    uint32_t code_off;
    uint32_t data_off;
    uint32_t sym_off;
    uint32_t total;
} ksx_layout;

/* *acc += v, alinhado a 4 se align; false se passar de 32 bits. */
static bool advance(uint32_t *acc, uint32_t v, bool align)
{
    if (v > UINT32_C(0xFFFFFFFF) - *acc) {
        return false;
    }
    *acc += v;
    if (align) {
        if (*acc > UINT32_C(0xFFFFFFFC)) {
            return false;
        }
        *acc = (*acc + 3u) & ~UINT32_C(3);
    }
    return true;
}

static bool compute_layout(const ksvm_ksx_desc *d, ksx_layout *l)
{
    uint32_t pos = KSVM_KSX_HEADER_SIZE;

    l->code_off = pos;
    if (!advance(&pos, d->code_size, true)) {
        return false;
    }
    l->data_off = pos;
    if (!advance(&pos, d->data_size, true)) {
        return false;
    }
    l->sym_off = (d->sym_size != 0u) ? pos : 0u;
    if (!advance(&pos, d->sym_size, false)) {
        return false;
    }
    l->total = pos;
    return true;
}

uint32_t ksvm_ksx_size(const ksvm_ksx_desc *d)
{
    ksx_layout l;
    return compute_layout(d, &l) ? l.total : 0u;
}

/* data + bss + heap_min + stack; satura em 2^32 - 1 (campo informativo). */
static uint32_t ram_min(const ksvm_ksx_desc *d)
{
    uint32_t acc = d->data_size;

    if (!advance(&acc, d->bss_size, false) || !advance(&acc, d->heap_min, false) ||
        !advance(&acc, d->stack_size, false)) {
        return UINT32_C(0xFFFFFFFF);
    }
    return acc;
}

static void write_header(const ksvm_ksx_desc *d, const ksx_layout *l, uint8_t *out)
{
    uint16_t flags = d->flags;

    if (d->sym_size != 0u) {
        flags = (uint16_t)(flags | KSVM_KSX_FLAG_SYMS);
    }
    out[0] = 0x4Bu;  /* 'K' */
    out[1] = 0x53u;  /* 'S' */
    out[2] = 0x58u;  /* 'X' */
    out[3] = 0x00u;
    out[KSVM_KSX_OFF_VER_MAJOR] = KSVM_KSX_VERSION_MAJOR;
    out[KSVM_KSX_OFF_VER_MINOR] = KSVM_KSX_VERSION_MINOR;
    ksvm_st16le(out + KSVM_KSX_OFF_FLAGS, flags);
    ksvm_st32le(out + KSVM_KSX_OFF_ENTRY, d->entry);
    ksvm_st32le(out + KSVM_KSX_OFF_CODE_OFF, l->code_off);
    ksvm_st32le(out + KSVM_KSX_OFF_CODE_SIZE, d->code_size);
    ksvm_st32le(out + KSVM_KSX_OFF_DATA_OFF, l->data_off);
    ksvm_st32le(out + KSVM_KSX_OFF_DATA_SIZE, d->data_size);
    ksvm_st32le(out + KSVM_KSX_OFF_BSS_SIZE, d->bss_size);
    ksvm_st32le(out + KSVM_KSX_OFF_RAM_BASE, d->ram_base);
    ksvm_st32le(out + KSVM_KSX_OFF_RAM_MIN, ram_min(d));
    ksvm_st32le(out + KSVM_KSX_OFF_STACK_SIZE, d->stack_size);
    ksvm_st32le(out + KSVM_KSX_OFF_SYM_OFF, l->sym_off);
    ksvm_st32le(out + KSVM_KSX_OFF_SYM_SIZE, d->sym_size);
}

static bool desc_valid(const ksvm_ksx_desc *d)
{
    return (d->code_size == 0u || d->code != NULL) &&
           (d->data_size == 0u || d->data != NULL) &&
           (d->sym_size == 0u || d->sym != NULL);
}

static void copy_section(uint8_t *out, uint32_t off, const uint8_t *src, uint32_t len)
{
    /* off + len <= l.total <= cap, verificado em ksvm_ksx_write. cap é um
     * uint32_t, mas descreve o tamanho de "out", um objeto que existe na
     * memória do host e por isso cabe em size_t: o cast não trunca nem onde
     * size_t tem 16 bits (AVR). Contrato de ksvm_ksx_write: cap não pode
     * passar do tamanho real do buffer. */
    if (len != 0u) {
        KSVM_MEMCPY(out + off, src, (size_t)len);
    }
}

int ksvm_ksx_write(const ksvm_ksx_desc *d, uint8_t *out, uint32_t cap,
                   uint32_t *written)
{
    ksx_layout l;
    uint32_t adler = KSVM_ADLER32_INIT;
    uint32_t off;

    if (d == NULL || out == NULL || !desc_valid(d)) {
        return KSVM_E_ARG;
    }
    if (!compute_layout(d, &l) || l.total > cap) {
        return KSVM_E_RANGE;
    }
    KSVM_MEMSET(out, 0, (size_t)l.total); /* cabeçalho reservado e padding = 0 */
    write_header(d, &l, out);
    copy_section(out, l.code_off, d->code, d->code_size);
    copy_section(out, l.data_off, d->data, d->data_size);
    copy_section(out, l.sym_off, d->sym, d->sym_size);
    for (off = KSVM_KSX_CHECKSUM_START; off < l.total; off++) {
        adler = ksvm_adler32_update(adler, out[off]);
    }
    ksvm_st32le(out + KSVM_KSX_OFF_CHECKSUM, adler);
    if (written != NULL) {
        *written = l.total;
    }
    return KSVM_OK;
}
