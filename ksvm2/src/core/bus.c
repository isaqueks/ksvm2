/*
 * bus.c — regiões de memória do guest e acesso verificado a elas.
 *
 * Todas as verificações de faixa usam o deslocamento (addr - base), nunca
 * "addr + len", para não dar a volta em 2^32:
 *     addr >= base && addr - base < size && size - (addr - base) >= len
 */
#include <string.h>

#include "ksvm/bus.h"
#include "ksvm/bytes.h"

void ksvm_bus_init(ksvm_bus *bus)
{
    KSVM_MEMSET(bus, 0, sizeof *bus);
}

/* ---- Mapeamento ------------------------------------------------------- */

/* Tamanho não nulo e base + size <= 2^32. */
static bool region_fits_space(const ksvm_region *r)
{
    if (r->size == 0u) {
        return false;
    }
    return r->base == 0u || r->size <= (uint32_t)(0u - r->base);
}

/*
 * Toda permissão precisa de um meio de acesso. Escrita direta (sem write8)
 * só em RAM: é o que garante que o backing const de uma ROM nunca é escrito.
 */
static bool region_accessible(const ksvm_region *r)
{
    bool readable = (r->perm & (KSVM_MEM_READ | KSVM_MEM_EXEC)) != 0u;
    bool writable = (r->perm & KSVM_MEM_WRITE) != 0u;

    if (readable && r->read8 == NULL && r->backing == NULL) {
        return false;
    }
    if (writable && r->write8 == NULL &&
        (r->backing == NULL || r->kind != KSVM_REGION_RAM)) {
        return false;
    }
    return true;
}

static bool ranges_overlap(uint32_t b1, uint32_t s1, uint32_t b2, uint32_t s2)
{
    return (b2 >= b1) ? (b2 - b1 < s1) : (b1 - b2 < s2);
}

/* RAM vai à frente das outras regiões (busca linear encontra-a primeiro). */
static uint8_t insert_position(const ksvm_bus *bus, uint8_t kind)
{
    uint8_t pos = 0;

    if (kind != KSVM_REGION_RAM) {
        return bus->count;
    }
    while (pos < bus->count && bus->regions[pos].kind == KSVM_REGION_RAM) {
        pos++;
    }
    return pos;
}

int ksvm_bus_map(ksvm_bus *bus, const ksvm_region *region)
{
    uint8_t i, pos;

    if (!region_fits_space(region) || !region_accessible(region)) {
        return KSVM_E_ARG;
    }
    for (i = 0; i < bus->count; i++) {
        const ksvm_region *r = &bus->regions[i];
        if (ranges_overlap(r->base, r->size, region->base, region->size)) {
            return KSVM_E_OVERLAP;
        }
    }
    if (bus->count >= KSVM_MAX_REGIONS) {
        return KSVM_E_FULL;
    }
    pos = insert_position(bus, region->kind);
    for (i = bus->count; i > pos; i--) {
        bus->regions[i] = bus->regions[i - 1u];
    }
    bus->regions[pos] = *region;
    bus->count++;
    return KSVM_OK;
}

/* ---- Localização ------------------------------------------------------ */

ksvm_region *ksvm_bus_find(ksvm_bus *bus, uint32_t addr, uint32_t len)
{
    uint8_t i;

    for (i = 0; i < bus->count; i++) {
        ksvm_region *r = &bus->regions[i];
        uint32_t off = addr - r->base;
        if (addr >= r->base && off < r->size && r->size - off >= len) {
            return r;
        }
    }
    return NULL;
}

static ksvm_fault locate(ksvm_bus *bus, uint32_t addr, uint32_t len,
                         uint8_t perm, ksvm_region **out)
{
    ksvm_region *r = ksvm_bus_find(bus, addr, len);

    if (r == NULL) {
        return KSVM_FAULT_MEM_RANGE;
    }
    if ((r->perm & perm) != perm) {
        return KSVM_FAULT_MEM_PERM;
    }
    *out = r;
    return KSVM_FAULT_NONE;
}

ksvm_fault ksvm_bus_check(ksvm_bus *bus, uint32_t addr, uint32_t len, uint8_t perm)
{
    ksvm_region *r = NULL;
    return locate(bus, addr, len, perm, &r);
}

/* ---- Leitura e escrita ------------------------------------------------ */

/* Caminho rápido: backing por índice. Com read8 (flash, MMIO) compõe
 * little-endian byte a byte, do mais significativo para o menos. */
static uint32_t region_load(ksvm_region *r, uint32_t off, uint32_t len)
{
    uint32_t v = 0u;
    uint32_t i = len;

    if (r->read8 == NULL) {
        const uint8_t *p = (const uint8_t *)r->backing + off;
        if (len == 4u) {
            return ksvm_ld32le(p);
        }
        return (len == 2u) ? (uint32_t)ksvm_ld16le(p) : (uint32_t)p[0];
    }
    while (i > 0u) {
        i--;
        v = (v << 8) | r->read8(r, off + i);
    }
    return v;
}

/* Escrita em ordem crescente de endereço (relevante para MMIO). */
static void region_store(ksvm_region *r, uint32_t off, uint32_t len, uint32_t v)
{
    uint32_t i;

    if (r->write8 == NULL) {
        uint8_t *p = (uint8_t *)r->backing + off;
        if (len == 4u) {
            ksvm_st32le(p, v);
        } else if (len == 2u) {
            ksvm_st16le(p, (uint16_t)(v & 0xFFFFu));
        } else {
            p[0] = (uint8_t)(v & 0xFFu);
        }
        return;
    }
    for (i = 0; i < len; i++) {
        r->write8(r, off + i, (uint8_t)((v >> (8u * i)) & 0xFFu));
    }
}

/* Exige [addr, addr + span) inteiro numa região com "perm" e lê os ÚLTIMOS
 * len bytes dessa faixa. Dados: span == len. Segunda palavra de instrução
 * L: span 8, len 4. Um só chamador de region_load (no AVR ela fica inline). */
static ksvm_fault bus_load(ksvm_bus *bus, uint32_t addr, uint8_t span, uint8_t len,
                           uint8_t perm, uint32_t *out)
{
    ksvm_region *r = NULL;
    ksvm_fault f = locate(bus, addr, span, perm, &r);

    if (f == KSVM_FAULT_NONE) {
        *out = region_load(r, addr - r->base + (uint32_t)(span - len), len);
    }
    return f;
}

static ksvm_fault bus_store(ksvm_bus *bus, uint32_t addr, uint32_t len, uint32_t v)
{
    ksvm_region *r = NULL;
    ksvm_fault f = locate(bus, addr, len, KSVM_MEM_WRITE, &r);

    if (f == KSVM_FAULT_NONE) {
        region_store(r, addr - r->base, len, v);
    }
    return f;
}

ksvm_fault ksvm_bus_read8(ksvm_bus *bus, uint32_t addr, uint8_t *out)
{
    uint32_t v = 0u;
    ksvm_fault f = bus_load(bus, addr, 1u, 1u, KSVM_MEM_READ, &v);

    if (f == KSVM_FAULT_NONE) {
        *out = (uint8_t)v;
    }
    return f;
}

ksvm_fault ksvm_bus_read16(ksvm_bus *bus, uint32_t addr, uint16_t *out)
{
    uint32_t v = 0u;
    ksvm_fault f = bus_load(bus, addr, 2u, 2u, KSVM_MEM_READ, &v);

    if (f == KSVM_FAULT_NONE) {
        *out = (uint16_t)v;
    }
    return f;
}

ksvm_fault ksvm_bus_read32(ksvm_bus *bus, uint32_t addr, uint32_t *out)
{
    return bus_load(bus, addr, 4u, 4u, KSVM_MEM_READ, out);
}

ksvm_fault ksvm_bus_fetch32(ksvm_bus *bus, uint32_t addr, uint32_t *out)
{
    return bus_load(bus, addr, 4u, 4u, KSVM_MEM_EXEC, out);
}

/* Uma só busca de região para os 8 bytes: custa o mesmo que a antiga
 * fetch32(addr + 4), que aceitava a palavra vinda de outra região. */
ksvm_fault ksvm_bus_fetch_imm32(ksvm_bus *bus, uint32_t addr, uint32_t *out)
{
    return bus_load(bus, addr, 8u, 4u, KSVM_MEM_EXEC, out);
}

ksvm_fault ksvm_bus_write8(ksvm_bus *bus, uint32_t addr, uint8_t v)
{
    return bus_store(bus, addr, 1u, v);
}

ksvm_fault ksvm_bus_write16(ksvm_bus *bus, uint32_t addr, uint16_t v)
{
    return bus_store(bus, addr, 2u, v);
}

ksvm_fault ksvm_bus_write32(ksvm_bus *bus, uint32_t addr, uint32_t v)
{
    return bus_store(bus, addr, 4u, v);
}
