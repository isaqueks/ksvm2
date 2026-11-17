/*
 * test_loader.c — escritor .ksx, validação do cabeçalho (doc 05 §4) e
 * carregamento: .data copiado, .bss zerado, sp/stack_limit/heap_start,
 * caminho Harvard com read8.
 */
#include "ksvm/hal_null.h"
#include "ksvm/ksvm.h"
#include "test.h"

#define IMG_MAX  512u
#define RAM_SIZE 256u
#define CODE_WORDS 6u
#define RAM_AUTO 0x20u       /* align16(code_size = 24) */

typedef struct image {
    uint8_t  buf[IMG_MAX];
    uint32_t len;
} image;

static const uint8_t data_init[8] = { 0xEF, 0xBE, 0xAD, 0xDE, 1, 2, 3, 4 };

/* r2 = .data[0], r3 = .bss[0], r1 = r2; HALT. */
static void program(uint8_t *code)
{
    const uint32_t words[CODE_WORDS] = {
        ksvm_enc_l(KSVM_OP_MOVIL, 4, 0), RAM_AUTO,
        ksvm_enc_i(KSVM_OP_LDW, 2, 4, 0),
        ksvm_enc_i(KSVM_OP_LDW, 3, 4, 8),
        ksvm_enc_r(KSVM_OP_MOV, 1, 2, 0),
        ksvm_enc_r(KSVM_OP_HALT, 0, 0, 0)
    };
    unsigned i;
    for (i = 0; i < CODE_WORDS; i++) {
        ksvm_st32le(code + 4u * i, words[i]);
    }
}

static void default_desc(ksvm_ksx_desc *d, const uint8_t *code)
{
    memset(d, 0, sizeof *d);
    d->code = code;
    d->code_size = CODE_WORDS * 4u;
    d->data = data_init;
    d->data_size = sizeof data_init;
    d->bss_size = 16u;
    d->stack_size = 64u;
}

static int build(image *img, const ksvm_ksx_desc *d)
{
    return ksvm_ksx_write(d, img->buf, IMG_MAX, &img->len);
}

static int build_default(image *img, uint8_t *code)
{
    ksvm_ksx_desc d;
    program(code);
    default_desc(&d, code);
    return build(img, &d);
}

static void set_u32(image *img, uint32_t off, uint32_t v)
{
    ksvm_st32le(img->buf + off, v);
}

TEST(adler32_reference_vector)
{
    static const char text[] = "Wikipedia";
    uint32_t a = KSVM_ADLER32_INIT;
    size_t i;
    for (i = 0; i < sizeof text - 1; i++) {
        a = ksvm_adler32_update(a, (uint8_t)text[i]);
    }
    ASSERT_EQ_U32(a, 0x11E60398u);
}

/* Entrada longa o bastante para A e B passarem do módulo: confere a
 * redução por subtração contra o "%" direto da definição. */
TEST(adler32_matches_modulo_definition)
{
    uint32_t a = KSVM_ADLER32_INIT, s1 = 1, s2 = 0, i;
    for (i = 0; i < 70000u; i++) {
        uint8_t byte = (uint8_t)(0xFFu - (i % 7u));
        a = ksvm_adler32_update(a, byte);
        s1 = (s1 + byte) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    ASSERT_EQ_U32(a, (s2 << 16) | s1);
}

TEST(writer_layout_and_checksum)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    uint32_t a = KSVM_ADLER32_INIT, off;
    ksvm_ksx_desc d;

    program(code);
    default_desc(&d, code);
    ASSERT_EQ(build(&img, &d), KSVM_OK);
    ASSERT_EQ_U32(img.len, ksvm_ksx_size(&d));
    ASSERT_EQ_U32(img.len, 64u + 24u + 8u);
    ASSERT_MEM_EQ(img.buf, "KSX", 4);
    ASSERT_EQ(img.buf[4], 2);
    ASSERT_EQ_U32(ksvm_ld32le(img.buf + KSVM_KSX_OFF_CODE_OFF), 64u);
    ASSERT_EQ_U32(ksvm_ld32le(img.buf + KSVM_KSX_OFF_DATA_OFF), 88u);
    ASSERT_EQ_U32(ksvm_ld32le(img.buf + KSVM_KSX_OFF_RAM_MIN), 8u + 16u + 64u);
    ASSERT_EQ_U32(ksvm_ld32le(img.buf + KSVM_KSX_OFF_SYM_OFF), 0u);
    for (off = KSVM_KSX_CHECKSUM_START; off < img.len; off++) {
        a = ksvm_adler32_update(a, img.buf[off]);
    }
    ASSERT_EQ_U32(ksvm_ld32le(img.buf + KSVM_KSX_OFF_CHECKSUM), a);
}

TEST(writer_symbols_and_errors)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    static const uint8_t sym[7] = { 0, 0, 0, 0, 1, 1, 'x' };
    ksvm_ksx_desc d;
    ksvm_image_header h;

    program(code);
    default_desc(&d, code);
    d.sym = sym;
    d.sym_size = sizeof sym;
    ASSERT_EQ(build(&img, &d), KSVM_OK);
    ASSERT_EQ(ksvm_image_parse(img.buf, img.len, NULL, &h), KSVM_OK);
    ASSERT_TRUE((h.flags & KSVM_KSX_FLAG_SYMS) != 0);
    ASSERT_EQ_U32(h.sym_off, 96u);
    ASSERT_MEM_EQ(img.buf + h.sym_off, sym, sizeof sym);
    ASSERT_EQ(ksvm_ksx_write(&d, img.buf, 64u, &img.len), KSVM_E_RANGE);
    d.code = NULL;
    ASSERT_EQ(build(&img, &d), KSVM_E_ARG);
}

TEST(load_valid_image_and_run)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    uint8_t ram[RAM_SIZE];
    ksvm vm;

    ASSERT_EQ(build_default(&img, code), KSVM_OK);
    memset(ram, 0xAA, sizeof ram);
    ksvm_init(&vm, NULL);
    ASSERT_EQ(ksvm_load_image(&vm, img.buf, img.len, ram, RAM_SIZE), KSVM_OK);
    ASSERT_EQ(vm.status, KSVM_RUNNING);
    ASSERT_EQ(vm.bus.count, 2);
    ASSERT_EQ_U32(vm.cpu.pc, 0u);
    ASSERT_EQ_U32(vm.cpu.r[KSVM_REG_SP], RAM_AUTO + RAM_SIZE);
    ASSERT_EQ_U32(vm.stack_limit, RAM_AUTO + RAM_SIZE - 64u);
    ASSERT_EQ_U32(vm.heap_start, RAM_AUTO + 8u + 16u);
    ASSERT_MEM_EQ(ram, data_init, sizeof data_init);
    ASSERT_EQ(ram[8], 0);                     /* .bss zerado */
    ASSERT_EQ(ram[23], 0);
    ASSERT_EQ(ram[24], 0xAA);                 /* além do .bss: intocado */
    ASSERT_EQ(ksvm_run(&vm, 100), KSVM_HALTED);
    ASSERT_EQ_U32(vm.exit_code, 0xDEADBEEFu);
    ASSERT_EQ_U32(vm.cpu.r[3], 0u);
    ASSERT_TRUE(img.buf + 64 == (const uint8_t *)vm.bus.regions[1].backing);  /* sem cópia */
}

TEST(explicit_ram_base)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    uint8_t ram[RAM_SIZE];
    ksvm_ksx_desc d;
    ksvm vm;

    program(code);
    default_desc(&d, code);
    d.ram_base = 0x10000u;
    ASSERT_EQ(build(&img, &d), KSVM_OK);
    ksvm_init(&vm, NULL);
    ASSERT_EQ(ksvm_load_image(&vm, img.buf, img.len, ram, RAM_SIZE), KSVM_OK);
    ASSERT_EQ_U32(vm.cpu.r[KSVM_REG_SP], 0x10000u + RAM_SIZE);
    ASSERT_EQ_U32(vm.bus.regions[0].base, 0x10000u);
}

static int load_tampered(image *img)
{
    uint8_t ram[RAM_SIZE];
    ksvm vm;
    ksvm_init(&vm, NULL);
    return ksvm_load_image(&vm, img->buf, img->len, ram, RAM_SIZE);
}

TEST(rejects_short_image)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    ASSERT_EQ(build_default(&img, code), KSVM_OK);
    img.len = 63u;
    ASSERT_EQ(load_tampered(&img), KSVM_E_IMAGE_RANGE);
}

TEST(rejects_bad_magic)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    ASSERT_EQ(build_default(&img, code), KSVM_OK);
    img.buf[3] = 'X';
    ASSERT_EQ(load_tampered(&img), KSVM_E_IMAGE_MAGIC);
}

TEST(rejects_bad_version_accepts_any_minor)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    ASSERT_EQ(build_default(&img, code), KSVM_OK);
    img.buf[KSVM_KSX_OFF_VER_MINOR] = 7;      /* cabeçalho < 0x38: fora do checksum */
    ASSERT_EQ(load_tampered(&img), KSVM_OK);
    img.buf[KSVM_KSX_OFF_VER_MAJOR] = 3;
    ASSERT_EQ(load_tampered(&img), KSVM_E_IMAGE_VERSION);
}

TEST(rejects_bad_checksum)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    ASSERT_EQ(build_default(&img, code), KSVM_OK);
    img.buf[img.len - 1u] ^= 0x01u;
    ASSERT_EQ(load_tampered(&img), KSVM_E_IMAGE_CHECKSUM);
    img.buf[img.len - 1u] ^= 0x01u;
    img.buf[0x38] = 1;                        /* reservado também é coberto */
    ASSERT_EQ(load_tampered(&img), KSVM_E_IMAGE_CHECKSUM);
}

TEST(rejects_sections_outside_file)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    ASSERT_EQ(build_default(&img, code), KSVM_OK);
    set_u32(&img, KSVM_KSX_OFF_CODE_SIZE, 0xFFFFFFF0u);    /* off + size estouraria */
    ASSERT_EQ(load_tampered(&img), KSVM_E_IMAGE_RANGE);
    ASSERT_EQ(build_default(&img, code), KSVM_OK);
    set_u32(&img, KSVM_KSX_OFF_DATA_OFF, img.len - 4u);
    ASSERT_EQ(load_tampered(&img), KSVM_E_IMAGE_RANGE);
    ASSERT_EQ(build_default(&img, code), KSVM_OK);
    set_u32(&img, KSVM_KSX_OFF_SYM_OFF, 64u);
    set_u32(&img, KSVM_KSX_OFF_SYM_SIZE, img.len);
    ASSERT_EQ(load_tampered(&img), KSVM_E_IMAGE_RANGE);
}

TEST(rejects_bad_entry)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    ASSERT_EQ(build_default(&img, code), KSVM_OK);
    set_u32(&img, KSVM_KSX_OFF_ENTRY, 2u);
    ASSERT_EQ(load_tampered(&img), KSVM_E_IMAGE_ENTRY);
    set_u32(&img, KSVM_KSX_OFF_ENTRY, CODE_WORDS * 4u);
    ASSERT_EQ(load_tampered(&img), KSVM_E_IMAGE_ENTRY);
}

TEST(rejects_insufficient_ram_without_side_effects)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    uint8_t ram[RAM_SIZE];
    ksvm vm;

    ASSERT_EQ(build_default(&img, code), KSVM_OK);
    memset(ram, 0xAA, sizeof ram);
    ksvm_init(&vm, NULL);
    ASSERT_EQ(ksvm_load_image(&vm, img.buf, img.len, ram, 8u + 16u + 64u - 4u), KSVM_E_NO_RAM);
    ASSERT_EQ(vm.bus.count, 0);
    ASSERT_EQ(ram[0], 0xAA);
    ASSERT_EQ(vm.status, KSVM_HALTED);
    ASSERT_EQ(ksvm_load_image(&vm, img.buf, img.len, ram, 8u + 16u + 64u), KSVM_OK);
}

TEST(rejects_overlap_and_restores_bus)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    uint8_t ram[RAM_SIZE];
    uint8_t mmio_mem[16];
    ksvm_ksx_desc d;
    ksvm vm;

    program(code);
    default_desc(&d, code);
    d.ram_base = 8u;                          /* dentro do código */
    ASSERT_EQ(build(&img, &d), KSVM_OK);
    ksvm_init(&vm, NULL);
    ASSERT_EQ(ksvm_load_image(&vm, img.buf, img.len, ram, RAM_SIZE), KSVM_E_OVERLAP);
    ASSERT_EQ(vm.bus.count, 0);

    ASSERT_EQ(build_default(&img, code), KSVM_OK);
    ASSERT_EQ(ksvm_map_ram(&vm, RAM_AUTO + 0x40u, mmio_mem, sizeof mmio_mem), KSVM_OK);
    ASSERT_EQ(ksvm_load_image(&vm, img.buf, img.len, ram, RAM_SIZE), KSVM_E_OVERLAP);
    ASSERT_EQ(vm.bus.count, 1);
}

/* "Flash" que guarda cada byte invertido: só um loader que lê TUDO por read8
 * (cabeçalho, checksum, .data e fetch) consegue carregar e rodar. */
static uint8_t inverted_read8(ksvm_region *r, uint32_t off)
{
    return (uint8_t)~((const uint8_t *)r->backing)[off];
}

TEST(harvard_load_through_read8)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    uint8_t ram[RAM_SIZE];
    uint32_t i;
    ksvm vm;

    ASSERT_EQ(build_default(&img, code), KSVM_OK);
    for (i = 0; i < img.len; i++) {
        img.buf[i] = (uint8_t)~img.buf[i];
    }
    ksvm_init(&vm, NULL);
    ASSERT_EQ(ksvm_load_image(&vm, img.buf, img.len, ram, RAM_SIZE), KSVM_E_IMAGE_MAGIC);
    ASSERT_EQ(ksvm_load_image_ex(&vm, img.buf, img.len, ram, RAM_SIZE, inverted_read8), KSVM_OK);
    ASSERT_MEM_EQ(ram, data_init, sizeof data_init);
    ASSERT_EQ(ksvm_run(&vm, 100), KSVM_HALTED);
    ASSERT_EQ_U32(vm.exit_code, 0xDEADBEEFu);
}

TEST(mem_info_after_load)
{
    image img;
    uint8_t code[CODE_WORDS * 4];
    uint8_t ram[RAM_SIZE];
    ksvm vm;

    ASSERT_EQ(build_default(&img, code), KSVM_OK);
    ksvm_init(&vm, NULL);
    ASSERT_EQ(ksvm_load_image(&vm, img.buf, img.len, ram, RAM_SIZE), KSVM_OK);
    ksvm_syscall(&vm, KSVM_SYSCALL_MEM_INFO);
    ASSERT_EQ_U32(vm.cpu.r[1], RAM_AUTO);
    ASSERT_EQ_U32(vm.cpu.r[2], RAM_SIZE);
    ASSERT_EQ_U32(vm.cpu.r[3], RAM_AUTO + 24u);
    ASSERT_EQ_U32(vm.cpu.r[4], RAM_AUTO + RAM_SIZE - 64u);
}

SUITE(loader)
{
    RUN_TEST(adler32_reference_vector);
    RUN_TEST(adler32_matches_modulo_definition);
    RUN_TEST(writer_layout_and_checksum);
    RUN_TEST(writer_symbols_and_errors);
    RUN_TEST(load_valid_image_and_run);
    RUN_TEST(explicit_ram_base);
    RUN_TEST(rejects_short_image);
    RUN_TEST(rejects_bad_magic);
    RUN_TEST(rejects_bad_version_accepts_any_minor);
    RUN_TEST(rejects_bad_checksum);
    RUN_TEST(rejects_sections_outside_file);
    RUN_TEST(rejects_bad_entry);
    RUN_TEST(rejects_insufficient_ram_without_side_effects);
    RUN_TEST(rejects_overlap_and_restores_bus);
    RUN_TEST(harvard_load_through_read8);
    RUN_TEST(mem_info_after_load);
}
