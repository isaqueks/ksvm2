/*
 * bus.h — barramento de memória do guest: regiões RAM/ROM/MMIO.
 *
 * Todo acesso do guest passa por aqui. Acessos de 16/32 bits são compostos
 * little-endian pelo barramento a partir de bytes, então nenhum chamador lida
 * com alinhamento ou endianness. Um acesso multi-byte precisa caber inteiro
 * numa única região; cruzar a borda gera KSVM_FAULT_MEM_RANGE.
 */
#ifndef KSVM_BUS_H
#define KSVM_BUS_H

#include "fault.h"
#include "types.h"

typedef enum ksvm_mem_perm {
    KSVM_MEM_READ  = 1,
    KSVM_MEM_WRITE = 2,
    KSVM_MEM_EXEC  = 4
} ksvm_mem_perm;

typedef enum ksvm_region_kind {
    KSVM_REGION_RAM = 0,     /* backing = uint8_t[] gravável */
    KSVM_REGION_ROM,         /* backing = const uint8_t[] (ou flash via read8) */
    KSVM_REGION_MMIO         /* backing = contexto dos callbacks */
} ksvm_region_kind;

struct ksvm_region;
typedef uint8_t (*ksvm_read8_fn)(struct ksvm_region *r, uint32_t off);
typedef void (*ksvm_write8_fn)(struct ksvm_region *r, uint32_t off, uint8_t v);

typedef struct ksvm_region {
    uint32_t base;           /* endereço inicial no espaço do guest */
    uint32_t size;           /* > 0; base + size não passa de 2^32 */
    uint8_t  perm;           /* máscara ksvm_mem_perm */
    uint8_t  kind;           /* ksvm_region_kind */
    /* uint8_t* (RAM), const uint8_t* (ROM; o núcleo nunca escreve nele) ou
     * ctx (MMIO). Com read8/write8 == NULL o acesso é direto por índice. */
    void          *backing;
    ksvm_read8_fn  read8;    /* ROM em flash (Harvard) e MMIO */
    ksvm_write8_fn write8;   /* MMIO */
} ksvm_region;

typedef struct ksvm_bus {
    ksvm_region regions[KSVM_MAX_REGIONS];
    uint8_t     count;
} ksvm_bus;

void ksvm_bus_init(ksvm_bus *bus);

/*
 * Adiciona uma região (copiada). Erros: KSVM_E_ARG (tamanho 0, estoura 2^32,
 * permissão sem meio de acesso), KSVM_E_OVERLAP, KSVM_E_FULL.
 * Regiões RAM são mantidas à frente das demais: são as mais acessadas e a
 * busca linear as encontra primeiro (caminho rápido).
 */
int ksvm_bus_map(ksvm_bus *bus, const ksvm_region *region);

/* Região que contém [addr, addr + len) inteira, ou NULL. len >= 1. */
ksvm_region *ksvm_bus_find(ksvm_bus *bus, uint32_t addr, uint32_t len);

/* Verifica se [addr, addr + len) cabe numa região com "perm" (sem acessar). */
ksvm_fault ksvm_bus_check(ksvm_bus *bus, uint32_t addr, uint32_t len, uint8_t perm);

/* Acessos de dados: KSVM_FAULT_NONE, _MEM_RANGE ou _MEM_PERM. Em fault,
 * *out não é escrito e a memória não muda. */
ksvm_fault ksvm_bus_read8 (ksvm_bus *bus, uint32_t addr, uint8_t  *out);
ksvm_fault ksvm_bus_read16(ksvm_bus *bus, uint32_t addr, uint16_t *out);
ksvm_fault ksvm_bus_read32(ksvm_bus *bus, uint32_t addr, uint32_t *out);
ksvm_fault ksvm_bus_write8 (ksvm_bus *bus, uint32_t addr, uint8_t  v);
ksvm_fault ksvm_bus_write16(ksvm_bus *bus, uint32_t addr, uint16_t v);
ksvm_fault ksvm_bus_write32(ksvm_bus *bus, uint32_t addr, uint32_t v);

/* Busca de instrução: exige KSVM_MEM_EXEC. */
ksvm_fault ksvm_bus_fetch32(ksvm_bus *bus, uint32_t addr, uint32_t *out);

/* Segunda palavra (imm32) da instrução de 8 bytes em addr: exige que
 * [addr, addr + 8) inteiro esteja numa ÚNICA região EXEC (doc 04 §5.6,
 * "Limites do fetch") e lê addr + 4. Uma região vizinha não serve, e o
 * topo do espaço não dá a volta para o endereço 0. */
ksvm_fault ksvm_bus_fetch_imm32(ksvm_bus *bus, uint32_t addr, uint32_t *out);

#endif /* KSVM_BUS_H */
