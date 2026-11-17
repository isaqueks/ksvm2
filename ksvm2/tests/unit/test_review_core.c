/*
 * test_review_core.c — regressões da revisão adversarial do núcleo:
 * cada teste reproduz um defeito confirmado (falhava antes da correção) ou
 * fixa uma propriedade de segurança verificada na revisão (cabeçalho .ksx
 * hostil e syscalls com faixas extremas nunca tocam memória do host fora
 * dos buffers dados).
 */
#include "fixture.h"
#include "test.h"

#define IMG_MAX    512u
#define CODE_WORDS 2u
#define GUARD      64u

typedef struct image {
    uint8_t  buf[IMG_MAX];
    uint32_t len;
} image;

/* MOVI r1, 7; HALT. .data de 4 bytes, .bss de 8, pilha de 16. */
static int build_image(image *img, uint32_t ram_base)
{
    static const uint8_t data[4] = { 1, 2, 3, 4 };
    uint8_t code[CODE_WORDS * 4u];
    ksvm_ksx_desc d;

    ksvm_st32le(code, ksvm_enc_i(KSVM_OP_MOVI, 1, 0, 7));
    ksvm_st32le(code + 4, ksvm_enc_r(KSVM_OP_HALT, 0, 0, 0));
    memset(&d, 0, sizeof d);
    d.code = code;
    d.code_size = sizeof code;
    d.data = data;
    d.data_size = sizeof data;
    d.bss_size = 8u;
    d.stack_size = 16u;
    d.ram_base = ram_base;
    return ksvm_ksx_write(&d, img->buf, IMG_MAX, &img->len);
}

/* Campos do cabeçalho antes de 0x38 não entram no checksum: um atacante
 * os troca sem recalcular nada. */
static void poke(image *img, uint32_t off, uint32_t v)
{
    ksvm_st32le(img->buf + off, v);
}

static int guard_intact(const uint8_t *g, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (g[i] != 0xA5u) {
            return 0;
        }
    }
    return 1;
}

/* MEM_INFO deve descrever a RAM do programa carregado, não a primeira RAM
 * que o host por acaso mapeou antes (ex.: framebuffer compartilhado). */
TEST(mem_info_reports_loaded_ram_with_premapped_ram)
{
    image img;
    uint8_t ram[128];
    uint8_t shared[32];
    ksvm vm;

    ASSERT_EQ(build_image(&img, 0x1000u), KSVM_OK);
    ksvm_init(&vm, NULL);
    ASSERT_EQ(ksvm_map_ram(&vm, 0x40000000u, shared, sizeof shared), KSVM_OK);
    ASSERT_EQ(ksvm_load_image(&vm, img.buf, img.len, ram, sizeof ram), KSVM_OK);
    ksvm_syscall(&vm, KSVM_SYSCALL_MEM_INFO);
    ASSERT_EQ_U32(vm.cpu.r[1], 0x1000u);
    ASSERT_EQ_U32(vm.cpu.r[2], sizeof ram);
    ASSERT_EQ_U32(vm.cpu.r[3], 0x1000u + 4u + 8u);
    ASSERT_EQ_U32(vm.cpu.r[4], 0x1000u + sizeof ram - 16u);
}

/* ram_base do cabeçalho tal que ram_base + ram_size passa de 2^32: erro
 * documentado do loader (não KSVM_E_ARG, que acusaria o host). */
TEST(loader_ram_past_end_of_address_space)
{
    image img;
    uint8_t ram[128];
    ksvm vm;

    ASSERT_EQ(build_image(&img, 0u), KSVM_OK);
    poke(&img, KSVM_KSX_OFF_RAM_BASE, 0xFFFFFFC0u);       /* 64 bytes até 2^32 */
    ksvm_init(&vm, NULL);
    ASSERT_EQ(ksvm_load_image(&vm, img.buf, img.len, ram, sizeof ram), KSVM_E_OVERLAP);
    ASSERT_EQ(vm.bus.count, 0);
    /* Cabendo exatamente até 2^32 carrega (sp = 0 por wrap, documentado). */
    ASSERT_EQ(ksvm_load_image(&vm, img.buf, img.len, ram, 64u), KSVM_OK);
    ASSERT_EQ_U32(vm.cpu.r[KSVM_REG_SP], 0u);
}

/* Cabeçalho hostil: nenhuma combinação de campos extremos escreve fora de
 * ram[0..ram_size) nem lê fora da imagem (ASan pega a leitura). */
TEST(loader_hostile_header_fields)
{
    static const uint32_t offs[] = {
        KSVM_KSX_OFF_ENTRY, KSVM_KSX_OFF_CODE_OFF, KSVM_KSX_OFF_CODE_SIZE,
        KSVM_KSX_OFF_DATA_OFF, KSVM_KSX_OFF_DATA_SIZE, KSVM_KSX_OFF_BSS_SIZE,
        KSVM_KSX_OFF_RAM_BASE, KSVM_KSX_OFF_STACK_SIZE, KSVM_KSX_OFF_SYM_OFF,
        KSVM_KSX_OFF_SYM_SIZE
    };
    static const uint32_t vals[] = {
        0u, 1u, 3u, 4u, 0x3Fu, 0x40u, 0x7FFFFFFFu, 0x80000000u,
        0xFFFFFFF0u, 0xFFFFFFFCu, 0xFFFFFFFFu
    };
    const uint32_t ram_size = 96u;
    uint8_t ram[96u + GUARD];
    size_t i, j;

    for (i = 0; i < sizeof offs / sizeof offs[0]; i++) {
        for (j = 0; j < sizeof vals / sizeof vals[0]; j++) {
            image img;
            ksvm vm;
            int err;
            ASSERT_EQ(build_image(&img, 0u), KSVM_OK);
            poke(&img, offs[i], vals[j]);
            memset(ram, 0xA5, sizeof ram);
            ksvm_init(&vm, NULL);
            err = ksvm_load_image(&vm, img.buf, img.len, ram, ram_size);
            if (!guard_intact(ram + ram_size, GUARD)) {
                test_note(t_, "campo 0x%02X = 0x%08X (err %d)",
                          (unsigned)offs[i], (unsigned)vals[j], err);
            }
            ASSERT_TRUE(guard_intact(ram + ram_size, GUARD));
            if (err == KSVM_OK) {
                (void)ksvm_run(&vm, 1000u);
                ASSERT_TRUE(guard_intact(ram + ram_size, GUARD));
            } else {
                ASSERT_EQ(vm.bus.count, 0);
            }
        }
    }
}

/* Syscalls de console e cópias com faixas que dão a volta em 2^32, numa
 * região que termina exatamente em 2^32. */
TEST(syscall_ranges_at_top_of_address_space)
{
    static const uint32_t ptrs[] = { 0xFFFFFF00u, 0xFFFFFFF0u, 0xFFFFFFFFu, 0u };
    static const uint32_t lens[] = { 0xFFFFFFFFu, 0x80000000u, 0x101u, 0x11u, 2u };
    uint8_t top[256 + GUARD];
    uint8_t buf[16];
    size_t i, j;

    for (i = 0; i < sizeof ptrs / sizeof ptrs[0]; i++) {
        for (j = 0; j < sizeof lens / sizeof lens[0]; j++) {
            ksvm vm;
            ksvm_hal hal;
            ksvm_hal_null hn;
            static const uint8_t in[4] = { 'a', 'b', 'c', '\n' };
            memset(top, 0xA5, sizeof top);
            ksvm_init(&vm, NULL);
            ksvm_hal_null_init(&hn, &hal, &vm);
            ksvm_hal_null_set_input(&hn, in, sizeof in, true);
            vm.hal = &hal;
            ASSERT_EQ(ksvm_map_ram(&vm, 0xFFFFFF00u, top, 256u), KSVM_OK);
            vm.cpu.r[1] = ptrs[i];
            vm.cpu.r[2] = lens[j];
            ksvm_syscall(&vm, KSVM_SYSCALL_GETS);
            ASSERT_TRUE(guard_intact(top + 256, GUARD));
            vm.cpu.r[1] = ptrs[i];
            vm.cpu.r[2] = lens[j];
            ksvm_syscall(&vm, KSVM_SYSCALL_PUTS);
            ASSERT_TRUE(lens[j] <= 256u || vm.cpu.r[1] == KSVM_SYS_RET_ERROR);
            memset(buf, 0x5A, sizeof buf);
            (void)ksvm_copy_to_guest(&vm, ptrs[i], buf,
                                     lens[j] > sizeof buf ? (uint32_t)sizeof buf : lens[j]);
            ASSERT_TRUE(guard_intact(top + 256, GUARD));
            {
                const uint8_t *p = ksvm_guest_ptr(&vm, ptrs[i], lens[j], KSVM_MEM_READ);
                ASSERT_TRUE(p == NULL || (p >= top && lens[j] <= (uint32_t)(top + 256 - p)));
            }
        }
    }
}

SUITE(review_core)
{
    RUN_TEST(mem_info_reports_loaded_ram_with_premapped_ram);
    RUN_TEST(loader_ram_past_end_of_address_space);
    RUN_TEST(loader_hostile_header_fields);
    RUN_TEST(syscall_ranges_at_top_of_address_space);
}
