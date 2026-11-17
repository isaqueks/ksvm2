/*
 * loader.c — validação do cabeçalho .ksx e carregamento na VM (doc 05).
 *
 * A imagem é lida por uma ksvm_region "fonte": com read8 == NULL o acesso é
 * direto ao buffer; com read8 (flash do AVR) cada byte passa pelo callback,
 * inclusive os do próprio cabeçalho. Assim o mesmo código serve aos dois
 * casos e nunca derreferencia memória que não está no espaço de dados.
 */
#include <string.h>

#include "ksvm/bytes.h"
#include "ksvm/loader.h"

#define ADLER_MOD UINT32_C(65521)

uint32_t ksvm_adler32_update(uint32_t adler, uint8_t byte)
{
    uint32_t a = adler & UINT32_C(0xFFFF);
    uint32_t b = adler >> 16;

    a += byte;                /* a < MOD + 256: uma subtração basta */
    if (a >= ADLER_MOD) {
        a -= ADLER_MOD;
    }
    b += a;                   /* b < 2 * MOD */
    if (b >= ADLER_MOD) {
        b -= ADLER_MOD;
    }
    return (b << 16) | a;
}

/* ---- Leitura da imagem ------------------------------------------------ */

static void source_init(ksvm_region *src, const uint8_t *image, uint32_t size,
                        ksvm_read8_fn read8)
{
    KSVM_MEMSET(src, 0, sizeof *src);
    src->size = size;
    src->perm = KSVM_MEM_READ;
    src->kind = KSVM_REGION_ROM;
    src->backing = (void *)image;   /* só lido; ver ksvm_map_rom */
    src->read8 = read8;
}

static uint8_t source_byte(ksvm_region *src, uint32_t off)
{
    if (src->read8 != NULL) {
        return src->read8(src, off);
    }
    return ((const uint8_t *)src->backing)[off];
}

static uint32_t source_u32(ksvm_region *src, uint32_t off)
{
    uint8_t b[4];
    uint32_t i;

    for (i = 0; i < 4u; i++) {
        b[i] = source_byte(src, off + i);
    }
    return ksvm_ld32le(b);
}

static bool magic_ok(ksvm_region *src)
{
    return source_byte(src, 0) == 0x4Bu && source_byte(src, 1) == 0x53u &&
           source_byte(src, 2) == 0x58u && source_byte(src, 3) == 0x00u;
}

static void read_header(ksvm_region *src, ksvm_image_header *h)
{
    h->ver_major  = source_byte(src, KSVM_KSX_OFF_VER_MAJOR);
    h->ver_minor  = source_byte(src, KSVM_KSX_OFF_VER_MINOR);
    h->flags      = (uint16_t)(source_byte(src, KSVM_KSX_OFF_FLAGS) |
                    ((unsigned)source_byte(src, KSVM_KSX_OFF_FLAGS + 1u) << 8));
    h->entry      = source_u32(src, KSVM_KSX_OFF_ENTRY);
    h->code_off   = source_u32(src, KSVM_KSX_OFF_CODE_OFF);
    h->code_size  = source_u32(src, KSVM_KSX_OFF_CODE_SIZE);
    h->data_off   = source_u32(src, KSVM_KSX_OFF_DATA_OFF);
    h->data_size  = source_u32(src, KSVM_KSX_OFF_DATA_SIZE);
    h->bss_size   = source_u32(src, KSVM_KSX_OFF_BSS_SIZE);
    h->ram_base   = source_u32(src, KSVM_KSX_OFF_RAM_BASE);
    h->ram_min    = source_u32(src, KSVM_KSX_OFF_RAM_MIN);
    h->stack_size = source_u32(src, KSVM_KSX_OFF_STACK_SIZE);
    h->sym_off    = source_u32(src, KSVM_KSX_OFF_SYM_OFF);
    h->sym_size   = source_u32(src, KSVM_KSX_OFF_SYM_SIZE);
    h->checksum   = source_u32(src, KSVM_KSX_OFF_CHECKSUM);
}

static uint32_t image_checksum(ksvm_region *src, uint32_t size)
{
    uint32_t adler = KSVM_ADLER32_INIT;
    uint32_t off;

    for (off = KSVM_KSX_CHECKSUM_START; off < size; off++) {
        adler = ksvm_adler32_update(adler, source_byte(src, off));
    }
    return adler;
}

/* [off, off + len) dentro de [0, size), sem somar (sem overflow). */
static bool section_ok(uint32_t off, uint32_t len, uint32_t size)
{
    return off <= size && len <= size - off;
}

static int validate_layout(const ksvm_image_header *h, uint32_t size)
{
    if (!section_ok(h->code_off, h->code_size, size) ||
        !section_ok(h->data_off, h->data_size, size) ||
        (h->sym_size != 0u && !section_ok(h->sym_off, h->sym_size, size))) {
        return KSVM_E_IMAGE_RANGE;
    }
    if (h->entry >= h->code_size || (h->entry & 3u) != 0u) {
        return KSVM_E_IMAGE_ENTRY;
    }
    return KSVM_OK;
}

int ksvm_image_parse(const uint8_t *image, uint32_t image_size,
                     ksvm_read8_fn read8, ksvm_image_header *hdr)
{
    ksvm_region src;

    if (image == NULL || hdr == NULL) {
        return KSVM_E_ARG;
    }
    if (image_size < KSVM_KSX_HEADER_SIZE) {
        return KSVM_E_IMAGE_RANGE;
    }
    source_init(&src, image, image_size, read8);
    if (!magic_ok(&src)) {
        return KSVM_E_IMAGE_MAGIC;
    }
    read_header(&src, hdr);
    if (hdr->ver_major != KSVM_KSX_VERSION_MAJOR) {
        return KSVM_E_IMAGE_VERSION;
    }
    if (image_checksum(&src, image_size) != hdr->checksum) {
        return KSVM_E_IMAGE_CHECKSUM;
    }
    return validate_layout(hdr, image_size);
}

/* ---- Carregamento ----------------------------------------------------- */

typedef struct ram_plan {
    uint32_t base;
    uint32_t sp;
} ram_plan;

static bool add_overflows(uint32_t *acc, uint32_t v)
{
    if (v > UINT32_C(0xFFFFFFFF) - *acc) {
        return true;
    }
    *acc += v;
    return false;
}

/*
 * Base da RAM (cabeçalho, ou código alinhado a 16) e sp inicial = fim da RAM
 * alinhado para baixo a 4. Exige que a RAM caiba no espaço de 32 bits
 * (senão KSVM_E_OVERLAP, como ksvm_bus_map recusaria) e que .data + .bss +
 * pilha caibam entre a base e esse sp. Se a RAM terminar exatamente em
 * 2^32, sp vale 0 (wrap).
 */
static int plan_ram(const ksvm_image_header *h, uint32_t ram_size, ram_plan *p)
{
    uint32_t need = h->data_size;
    uint32_t usable;

    p->base = h->ram_base;
    if (p->base == 0u) {
        p->base = (h->code_size + 15u) & ~UINT32_C(15);
    }
    /* base + ram_size passaria de 2^32. Cobre também code_size > 2^32 - 16,
     * cujo arredondamento dá base 0 (ram_size >= 1). */
    if (ram_size > (uint32_t)(0u - p->base)) {
        return KSVM_E_OVERLAP;
    }
    p->sp = (p->base + ram_size) & ~UINT32_C(3);
    usable = p->sp - p->base;
    if (usable > ram_size || add_overflows(&need, h->bss_size) ||
        add_overflows(&need, h->stack_size) || need > usable) {
        return KSVM_E_NO_RAM;
    }
    return KSVM_OK;
}

/* Mapeia CODE (em 0, sem cópia) e RAM; em erro o barramento volta ao que
 * era, para não deixar a VM meio carregada. */
static int map_image(ksvm *vm, const uint8_t *image, const ksvm_image_header *h,
                     uint8_t *ram, uint32_t ram_size, uint32_t ram_base,
                     ksvm_read8_fn read8)
{
    ksvm_bus saved = vm->bus;
    int err = ksvm_map_rom(vm, 0u, image + h->code_off, h->code_size, read8);

    if (err == KSVM_OK) {
        err = ksvm_map_ram(vm, ram_base, ram, ram_size);
    }
    if (err != KSVM_OK) {
        vm->bus = saved;
    }
    return err;
}

/* Copia .data (via read8 se houver) e zera .bss. */
static void init_ram(const uint8_t *image, uint32_t image_size,
                     const ksvm_image_header *h, uint8_t *ram, ksvm_read8_fn read8)
{
    ksvm_region src;
    uint32_t i;

    source_init(&src, image, image_size, read8);
    for (i = 0; i < h->data_size; i++) {
        ram[i] = source_byte(&src, h->data_off + i);
    }
    /* bss_size já foi validado contra ram_size (size_t) pelo chamador. */
    KSVM_MEMSET(ram + h->data_size, 0, (size_t)h->bss_size);
}

int ksvm_load_image_ex(ksvm *vm, const uint8_t *image, uint32_t image_size,
                       uint8_t *ram, uint32_t ram_size, ksvm_read8_fn read8)
{
    ksvm_image_header h;
    ram_plan plan;
    int err;

    if (vm == NULL || ram == NULL || ram_size == 0u) {
        return KSVM_E_ARG;
    }
    err = ksvm_image_parse(image, image_size, read8, &h);
    if (err == KSVM_OK) {
        err = plan_ram(&h, ram_size, &plan);
    }
    if (err == KSVM_OK) {
        err = map_image(vm, image, &h, ram, ram_size, plan.base, read8);
    }
    if (err != KSVM_OK) {
        return err;
    }
    init_ram(image, image_size, &h, ram, read8);
    vm->stack_limit = plan.sp - h.stack_size;
    vm->heap_start = plan.base + h.data_size + h.bss_size;
    ksvm_reset(vm, h.entry, plan.sp);
    return KSVM_OK;
}

int ksvm_load_image(ksvm *vm, const uint8_t *image, uint32_t image_size,
                    uint8_t *ram, uint32_t ram_size)
{
    return ksvm_load_image_ex(vm, image, image_size, ram, ram_size, NULL);
}
