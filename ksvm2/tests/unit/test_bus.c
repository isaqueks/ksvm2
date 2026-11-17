/*
 * test_bus.c — regiões do barramento: bordas, permissões, callbacks (ROM em
 * "flash" e MMIO), sobreposição e limites do espaço de 32 bits.
 */
#include "ksvm/ksvm.h"
#include "test.h"

static void ram_region(ksvm_region *r, uint32_t base, uint8_t *mem, uint32_t size)
{
    memset(r, 0, sizeof *r);
    r->base = base;
    r->size = size;
    r->perm = KSVM_MEM_READ | KSVM_MEM_WRITE;
    r->kind = KSVM_REGION_RAM;
    r->backing = mem;
}

/* "Flash" simulada: conta leituras e devolve backing[off] ^ 0 por callback. */
typedef struct flash_ctx {
    const uint8_t *data;
    unsigned       reads;
} flash_ctx;

static uint8_t flash_read8(ksvm_region *r, uint32_t off)
{
    flash_ctx *fc = (flash_ctx *)r->backing;
    fc->reads++;
    return fc->data[off];
}

/* MMIO que registra a ordem das escritas e devolve off + 0x10 nas leituras. */
typedef struct mmio_ctx {
    uint32_t log_off[8];
    uint8_t  log_val[8];
    unsigned writes;
} mmio_ctx;

static uint8_t mmio_read8(ksvm_region *r, uint32_t off)
{
    (void)r;
    return (uint8_t)(off + 0x10u);
}

static void mmio_write8(ksvm_region *r, uint32_t off, uint8_t v)
{
    mmio_ctx *m = (mmio_ctx *)r->backing;
    if (m->writes < 8) {
        m->log_off[m->writes] = off;
        m->log_val[m->writes] = v;
    }
    m->writes++;
}

TEST(ram_read_write_little_endian)
{
    ksvm_bus bus;
    ksvm_region r;
    uint8_t mem[16] = { 0 };
    uint32_t w = 0;
    uint16_t h = 0;
    uint8_t b = 0;

    ksvm_bus_init(&bus);
    ram_region(&r, 0x1000u, mem, sizeof mem);
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_OK);
    ASSERT_EQ(ksvm_bus_write32(&bus, 0x1001u, 0x11223344u), KSVM_FAULT_NONE);
    ASSERT_EQ_U32(mem[1], 0x44u);
    ASSERT_EQ_U32(mem[4], 0x11u);
    ASSERT_EQ(ksvm_bus_read32(&bus, 0x1001u, &w), KSVM_FAULT_NONE);
    ASSERT_EQ_U32(w, 0x11223344u);
    ASSERT_EQ(ksvm_bus_read16(&bus, 0x1002u, &h), KSVM_FAULT_NONE);
    ASSERT_EQ_U32(h, 0x2233u);
    ASSERT_EQ(ksvm_bus_write16(&bus, 0x1008u, 0xBEEFu), KSVM_FAULT_NONE);
    ASSERT_EQ(ksvm_bus_write8(&bus, 0x100Au, 0x7Au), KSVM_FAULT_NONE);
    ASSERT_EQ(ksvm_bus_read8(&bus, 0x1009u, &b), KSVM_FAULT_NONE);
    ASSERT_EQ_U32(b, 0xBEu);
    ASSERT_EQ_U32(mem[10], 0x7Au);
}

TEST(region_edges)
{
    ksvm_bus bus;
    ksvm_region r;
    uint8_t mem[16] = { 0 };
    uint32_t w = 0x5A5A5A5Au;
    uint8_t b = 0;

    ksvm_bus_init(&bus);
    ram_region(&r, 0x1000u, mem, sizeof mem);
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_OK);
    ASSERT_EQ(ksvm_bus_read8(&bus, 0x100Fu, &b), KSVM_FAULT_NONE);      /* último byte */
    ASSERT_EQ(ksvm_bus_read32(&bus, 0x100Cu, &w), KSVM_FAULT_NONE);     /* última palavra */
    w = 0x5A5A5A5Au;
    ASSERT_EQ(ksvm_bus_read32(&bus, 0x100Du, &w), KSVM_FAULT_MEM_RANGE);  /* cruza o fim */
    ASSERT_EQ_U32(w, 0x5A5A5A5Au);                                       /* saída intocada */
    ASSERT_EQ(ksvm_bus_read8(&bus, 0x0FFFu, &b), KSVM_FAULT_MEM_RANGE);
    ASSERT_EQ(ksvm_bus_read8(&bus, 0x1010u, &b), KSVM_FAULT_MEM_RANGE);
    ASSERT_EQ(ksvm_bus_write16(&bus, 0x100Fu, 0xFFFFu), KSVM_FAULT_MEM_RANGE);
    ASSERT_EQ_U32(mem[15], 0u);                                          /* sem escrita parcial */
}

TEST(access_across_adjacent_regions_faults)
{
    ksvm_bus bus;
    ksvm_region r;
    uint8_t m1[8] = { 0 }, m2[8] = { 0 };
    uint32_t w = 0;

    ksvm_bus_init(&bus);
    ram_region(&r, 0x100u, m1, 8);
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_OK);
    ram_region(&r, 0x108u, m2, 8);
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_OK);                /* adjacente: ok */
    ASSERT_EQ(ksvm_bus_read32(&bus, 0x106u, &w), KSVM_FAULT_MEM_RANGE);
    ASSERT_EQ(ksvm_bus_write32(&bus, 0x108u, 0xCAFEBABEu), KSVM_FAULT_NONE);
    ASSERT_EQ_U32(m2[0], 0xBEu);
}

TEST(rom_is_read_exec_only)
{
    ksvm vm;
    static const uint8_t rom[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t ram[8] = { 0 };
    uint32_t w = 0;

    ksvm_init(&vm, NULL);
    ASSERT_EQ(ksvm_map_rom(&vm, 0u, rom, sizeof rom, NULL), KSVM_OK);
    ASSERT_EQ(ksvm_map_ram(&vm, 0x100u, ram, sizeof ram), KSVM_OK);
    ASSERT_EQ(ksvm_bus_read32(&vm.bus, 4u, &w), KSVM_FAULT_NONE);
    ASSERT_EQ_U32(w, 0x08070605u);
    ASSERT_EQ(ksvm_bus_fetch32(&vm.bus, 0u, &w), KSVM_FAULT_NONE);
    ASSERT_EQ(ksvm_bus_write8(&vm.bus, 0u, 9u), KSVM_FAULT_MEM_PERM);
    ASSERT_EQ_U32(rom[0], 1u);
    ASSERT_EQ(ksvm_bus_fetch32(&vm.bus, 0x100u, &w), KSVM_FAULT_MEM_PERM);  /* RAM sem EXEC */
}

TEST(rom_with_read8_callback)
{
    ksvm vm;
    static const uint8_t flash[8] = { 0x78, 0x56, 0x34, 0x12, 0xAA, 0xBB, 0xCC, 0xDD };
    flash_ctx fc;
    uint32_t w = 0;
    uint16_t h = 0;

    fc.data = flash;
    fc.reads = 0;
    ksvm_init(&vm, NULL);
    /* backing é o contexto do callback (como um ponteiro PROGMEM no AVR) */
    ASSERT_EQ(ksvm_map_rom(&vm, 0x2000u, (const uint8_t *)(void *)&fc, 8u, flash_read8), KSVM_OK);
    ASSERT_EQ(ksvm_bus_read32(&vm.bus, 0x2000u, &w), KSVM_FAULT_NONE);
    ASSERT_EQ_U32(w, 0x12345678u);
    ASSERT_EQ(fc.reads, 4);
    ASSERT_EQ(ksvm_bus_fetch32(&vm.bus, 0x2004u, &w), KSVM_FAULT_NONE);
    ASSERT_EQ_U32(w, 0xDDCCBBAAu);
    ASSERT_EQ(ksvm_bus_read16(&vm.bus, 0x2003u, &h), KSVM_FAULT_NONE);
    ASSERT_EQ_U32(h, 0xAA12u);
}

TEST(mmio_callbacks_and_write_order)
{
    ksvm vm;
    mmio_ctx m;
    uint32_t w = 0;

    memset(&m, 0, sizeof m);
    ksvm_init(&vm, NULL);
    ASSERT_EQ(ksvm_map_mmio(&vm, 0xFFFF0000u, 0x100u, &m, mmio_read8, mmio_write8), KSVM_OK);
    ASSERT_EQ(ksvm_bus_write32(&vm.bus, 0xFFFF0004u, 0xA1B2C3D4u), KSVM_FAULT_NONE);
    ASSERT_EQ(m.writes, 4);
    ASSERT_EQ_U32(m.log_off[0], 4u);
    ASSERT_EQ_U32(m.log_val[0], 0xD4u);   /* byte menos significativo primeiro */
    ASSERT_EQ_U32(m.log_off[3], 7u);
    ASSERT_EQ_U32(m.log_val[3], 0xA1u);
    ASSERT_EQ(ksvm_bus_read32(&vm.bus, 0xFFFF0000u, &w), KSVM_FAULT_NONE);
    ASSERT_EQ_U32(w, 0x13121110u);
    ASSERT_EQ(ksvm_bus_fetch32(&vm.bus, 0xFFFF0000u, &w), KSVM_FAULT_MEM_PERM);
}

TEST(mmio_without_writer_is_read_only)
{
    ksvm vm;
    mmio_ctx m;
    memset(&m, 0, sizeof m);
    ksvm_init(&vm, NULL);
    ASSERT_EQ(ksvm_map_mmio(&vm, 0xF000u, 4u, &m, mmio_read8, NULL), KSVM_OK);
    ASSERT_EQ(ksvm_bus_write8(&vm.bus, 0xF000u, 1u), KSVM_FAULT_MEM_PERM);
    ASSERT_EQ(ksvm_map_mmio(&vm, 0xF100u, 4u, &m, NULL, NULL), KSVM_E_ARG);
}

TEST(overlap_rejected)
{
    ksvm_bus bus;
    ksvm_region r;
    uint8_t mem[16];

    ksvm_bus_init(&bus);
    ram_region(&r, 0x100u, mem, 16);
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_OK);
    ram_region(&r, 0x10Fu, mem, 16);
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_E_OVERLAP);
    ram_region(&r, 0x0F1u, mem, 16);
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_E_OVERLAP);
    ram_region(&r, 0x0u, mem, 0x1000u);                    /* contém a primeira */
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_E_OVERLAP);
    ASSERT_EQ(bus.count, 1);
}

TEST(invalid_regions_rejected)
{
    ksvm_bus bus;
    ksvm_region r;
    uint8_t mem[16];

    ksvm_bus_init(&bus);
    ram_region(&r, 0x100u, mem, 0u);                       /* tamanho 0 */
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_E_ARG);
    ram_region(&r, 0xFFFFFFF8u, mem, 16u);                 /* passa de 2^32 */
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_E_ARG);
    ram_region(&r, 0x100u, NULL, 16u);                     /* RAM sem backing */
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_E_ARG);
    ram_region(&r, 0x100u, mem, 16u);
    r.kind = KSVM_REGION_ROM;                              /* escrita direta fora de RAM */
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_E_ARG);
    ASSERT_EQ(bus.count, 0);
}

TEST(region_at_top_of_address_space)
{
    ksvm_bus bus;
    ksvm_region r;
    uint8_t mem[16] = { 0 };
    uint32_t w = 0;

    ksvm_bus_init(&bus);
    ram_region(&r, 0xFFFFFFF0u, mem, 16u);                 /* termina exatamente em 2^32 */
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_OK);
    ASSERT_EQ(ksvm_bus_write32(&bus, 0xFFFFFFFCu, 0x01020304u), KSVM_FAULT_NONE);
    ASSERT_EQ(ksvm_bus_read32(&bus, 0xFFFFFFFCu, &w), KSVM_FAULT_NONE);
    ASSERT_EQ_U32(w, 0x01020304u);
    ASSERT_EQ(ksvm_bus_read32(&bus, 0xFFFFFFFEu, &w), KSVM_FAULT_MEM_RANGE);  /* sem wrap */
}

TEST(bus_full)
{
    ksvm_bus bus;
    ksvm_region r;
    static uint8_t mem[KSVM_MAX_REGIONS + 1][4];
    unsigned i;

    ksvm_bus_init(&bus);
    for (i = 0; i < KSVM_MAX_REGIONS; i++) {
        ram_region(&r, 0x1000u * (i + 1u), mem[i], 4u);
        ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_OK);
    }
    ram_region(&r, 0x100000u, mem[KSVM_MAX_REGIONS], 4u);
    ASSERT_EQ(ksvm_bus_map(&bus, &r), KSVM_E_FULL);
}

TEST(ram_regions_come_first)
{
    ksvm vm;
    static const uint8_t rom[4] = { 0 };
    uint8_t ram[4];

    ksvm_init(&vm, NULL);
    ASSERT_EQ(ksvm_map_rom(&vm, 0u, rom, 4u, NULL), KSVM_OK);
    ASSERT_EQ(ksvm_map_ram(&vm, 0x100u, ram, 4u), KSVM_OK);
    ASSERT_EQ(vm.bus.regions[0].kind, KSVM_REGION_RAM);
    ASSERT_EQ(vm.bus.regions[1].kind, KSVM_REGION_ROM);
}

TEST(check_and_find)
{
    ksvm vm;
    static const uint8_t rom[8] = { 0 };
    uint8_t ram[8];

    ksvm_init(&vm, NULL);
    ASSERT_EQ(ksvm_map_rom(&vm, 0u, rom, 8u, NULL), KSVM_OK);
    ASSERT_EQ(ksvm_map_ram(&vm, 0x100u, ram, 8u), KSVM_OK);
    ASSERT_EQ(ksvm_bus_check(&vm.bus, 0x100u, 8u, KSVM_MEM_WRITE), KSVM_FAULT_NONE);
    ASSERT_EQ(ksvm_bus_check(&vm.bus, 0x100u, 9u, KSVM_MEM_READ), KSVM_FAULT_MEM_RANGE);
    ASSERT_EQ(ksvm_bus_check(&vm.bus, 0u, 8u, KSVM_MEM_WRITE), KSVM_FAULT_MEM_PERM);
    ASSERT_TRUE(ksvm_bus_find(&vm.bus, 0x104u, 4u) != NULL);
    ASSERT_TRUE(ksvm_bus_find(&vm.bus, 0x50u, 1u) == NULL);
}

SUITE(bus)
{
    RUN_TEST(ram_read_write_little_endian);
    RUN_TEST(region_edges);
    RUN_TEST(access_across_adjacent_regions_faults);
    RUN_TEST(rom_is_read_exec_only);
    RUN_TEST(rom_with_read8_callback);
    RUN_TEST(mmio_callbacks_and_write_order);
    RUN_TEST(mmio_without_writer_is_read_only);
    RUN_TEST(overlap_rejected);
    RUN_TEST(invalid_regions_rejected);
    RUN_TEST(region_at_top_of_address_space);
    RUN_TEST(bus_full);
    RUN_TEST(ram_regions_come_first);
    RUN_TEST(check_and_find);
}
