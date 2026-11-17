/*
 * listing.c — saída de inspeção do ksdis: cabeçalho, símbolos, código
 * (endereço, bytes, instrução, alvo) e dados em hex + ASCII.
 */
#include "ksdis.h"

#define DUMP_ROW 16u

/* ---- Cabeçalho -------------------------------------------------------- */

static void print_flags(FILE *o, uint16_t flags)
{
    fprintf(o, "; flags        0x%04X", (unsigned)flags);
    if ((flags & KSVM_KSX_FLAG_PIC) != 0u) {
        fputs(" PIC", o);
    }
    if ((flags & KSVM_KSX_FLAG_SYMS) != 0u) {
        fputs(" SYMS", o);
    }
    fputc('\n', o);
}

static void print_header(const ksdis *d)
{
    FILE *o = d->out;
    const ksvm_image_header *h = &d->h;

    fprintf(o, "; ksdis: %s (%lu bytes)\n", d->path, (unsigned long)d->img_size);
    fprintf(o, "; versao       %u.%u\n", (unsigned)h->ver_major, (unsigned)h->ver_minor);
    print_flags(o, h->flags);
    fprintf(o, "; entry        0x%08lX\n", (unsigned long)h->entry);
    fprintf(o, "; codigo       off 0x%08lX  %lu bytes\n",
            (unsigned long)h->code_off, (unsigned long)h->code_size);
    fprintf(o, "; dados        off 0x%08lX  %lu bytes  (ram 0x%08lX)\n",
            (unsigned long)h->data_off, (unsigned long)h->data_size,
            (unsigned long)d->ram_base);
    fprintf(o, "; bss          %lu bytes  (ram 0x%08lX)\n", (unsigned long)h->bss_size,
            (unsigned long)(d->ram_base + h->data_size));
    fprintf(o, "; ram_base     0x%08lX%s\n", (unsigned long)h->ram_base,
            (h->ram_base == 0u) ? " (loader decide)" : "");
    fprintf(o, "; ram_min      %lu\n", (unsigned long)h->ram_min);
    fprintf(o, "; pilha        %lu\n", (unsigned long)h->stack_size);
    fprintf(o, "; simbolos     off 0x%08lX  %lu bytes  (%lu registros)\n",
            (unsigned long)h->sym_off, (unsigned long)h->sym_size, (unsigned long)d->nsyms);
    fprintf(o, "; checksum     0x%08lX (ok)\n", (unsigned long)h->checksum);
}

static void print_symbols(const ksdis *d)
{
    size_t i;

    if (d->nsyms == 0u) {
        return;
    }
    fputs(";\n; ---- simbolos\n", d->out);
    for (i = 0; i < d->nsyms; i++) {
        fprintf(d->out, ";   0x%08lX  kind %u  %s\n", (unsigned long)d->syms[i].addr,
                (unsigned)d->syms[i].kind, d->syms[i].name);
    }
}

/* ---- Labels ----------------------------------------------------------- */

/* "nome:" para os símbolos em addr; "; nome = 0x..." para os que caem
 * dentro de [addr + 1, end). */
static void print_labels(const ksdis *d, uint32_t addr, uint32_t end)
{
    size_t i;

    for (i = ksdis_sym_lower(d, addr); i < d->nsyms && d->syms[i].addr < end; i++) {
        if (d->syms[i].addr == addr) {
            fprintf(d->out, "%s:\n", d->syms[i].name);
        } else {
            fprintf(d->out, "    ; %s = 0x%08lX\n", d->syms[i].name,
                    (unsigned long)d->syms[i].addr);
        }
    }
}

/* ---- Código ----------------------------------------------------------- */

static const char *resolve_any(void *ctx, uint32_t addr)
{
    const ksdis_sym *s = ksdis_sym_at((const ksdis *)ctx, addr, false);

    return (s != NULL) ? s->name : NULL;
}

static void print_bytes(FILE *o, const uint8_t *p, uint32_t n, uint32_t width)
{
    uint32_t i;

    for (i = 0; i < width; i++) {
        if (i < n) {
            fprintf(o, "%02X ", (unsigned)p[i]);
        } else {
            fputs("   ", o);
        }
    }
}

/* Uma instrução; devolve quantos bytes consumiu. */
static uint32_t list_insn(ksdis *d, uint32_t pc)
{
    char text[KSVM_DISASM_BUF];
    uint32_t w = ksdis_code_word(d, pc), imm32 = 0u, target = 0u;
    uint32_t len = ksvm_disasm_len(w);

    bool has_target = false;

    if (len == 8u && d->h.code_size - pc < 8u) {
        len = 4u;                              /* formato L truncado no fim */
        snprintf(text, sizeof text, ".word 0x%08lX ; formato L truncado", (unsigned long)w);
    } else {
        if (len == 8u) {
            imm32 = ksdis_code_word(d, pc + 4u);
        }
        (void)ksvm_disasm_one_ex(pc, w, imm32, resolve_any, d, text, sizeof text);
        has_target = ksvm_disasm_target(pc, w, imm32, &target);
    }
    print_labels(d, pc, pc + len);
    fprintf(d->out, "    %08lX:  ", (unsigned long)pc);
    print_bytes(d->out, d->code + pc, len, 8u);
    fprintf(d->out, " %s", text);
    if (has_target) {
        fprintf(d->out, "    ; -> 0x%08lX", (unsigned long)target);
    }
    fputc('\n', d->out);
    return len;
}

static void list_code(ksdis *d)
{
    uint32_t pc = 0u, size = d->h.code_size;

    fprintf(d->out, ";\n; ---- .text (CODE 0x00000000, %lu bytes)\n", (unsigned long)size);
    while (size - pc >= 4u) {
        pc += list_insn(d, pc);
    }
    if (pc < size) {
        print_labels(d, pc, size);
        fprintf(d->out, "    %08lX:  ", (unsigned long)pc);
        print_bytes(d->out, d->code + pc, size - pc, 8u);
        fputs(" ; bytes finais\n", d->out);
    }
    if (size != d->ram_base) {                 /* senão saem com os dados */
        print_labels(d, size, size + 1u);
    }
}

/* ---- Dados ------------------------------------------------------------ */

/* Fim da linha de dump: 16 bytes, o fim, ou o próximo símbolo. */
static uint32_t row_end(const ksdis *d, uint32_t addr, uint32_t end)
{
    uint32_t stop = (end - addr > DUMP_ROW) ? addr + DUMP_ROW : end;
    size_t i = ksdis_sym_lower(d, addr + 1u);

    if (i < d->nsyms && d->syms[i].addr < stop) {
        stop = d->syms[i].addr;
    }
    return stop;
}

static void dump_row(const ksdis *d, uint32_t addr, uint32_t stop)
{
    const uint8_t *p = d->data + (addr - d->ram_base);
    uint32_t n = stop - addr, i;

    fprintf(d->out, "    %08lX:  ", (unsigned long)addr);
    print_bytes(d->out, p, n, DUMP_ROW);
    fputs(" |", d->out);
    for (i = 0; i < n; i++) {
        fputc((p[i] >= 0x20u && p[i] < 0x7Fu) ? (int)p[i] : '.', d->out);
    }
    fputs("|\n", d->out);
}

static void list_data(ksdis *d)
{
    uint32_t addr = d->ram_base, end = d->ram_base + d->h.data_size;

    fprintf(d->out, ";\n; ---- .data (ram 0x%08lX, %lu bytes)\n", (unsigned long)addr,
            (unsigned long)d->h.data_size);
    while (addr < end) {
        uint32_t stop = row_end(d, addr, end);
        print_labels(d, addr, addr + 1u);
        dump_row(d, addr, stop);
        addr = stop;
    }
}

static void list_bss(ksdis *d)
{
    uint32_t start = d->ram_base + d->h.data_size, end = start + d->h.bss_size;

    fprintf(d->out, ";\n; ---- .bss (ram 0x%08lX, %lu bytes, zerado no load)\n",
            (unsigned long)start, (unsigned long)d->h.bss_size);
    print_labels(d, start, end + 1u);
}

void ksdis_listing(ksdis *d)
{
    print_header(d);
    print_symbols(d);
    list_code(d);
    list_data(d);
    list_bss(d);
}
