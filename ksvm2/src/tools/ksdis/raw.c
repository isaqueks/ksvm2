/*
 * raw.c — saída reassemblável do ksdis (--raw): só diretivas, labels e
 * instruções, para o round-trip ksas -> ksdis --raw -> ksas (doc 09).
 *
 * Labels: símbolos da tabela cujo nome é identificador válido e não é nome
 * de registrador; os demais alvos de desvio/ADR viram L_XXXX (endereço em
 * hex). O kind de cada símbolo segue a convenção do ksas (ksdis.h): bit
 * global -> .global; seção ABS -> .equ. Um label que cai no meio de uma
 * instrução (ou de uma palavra) faz esse trecho sair como .byte; um desvio
 * (b<cond>/jmp/call) cujo alvo não está em [0, code_size) — o ksas recusa —
 * ou um ADR/ADRL cujo alvo não pode receber label (fora do código e da RAM)
 * sai como .word: o binário remontado é o mesmo em todos os casos.
 */
#include "ksdis.h"
#include "ksvm/isa.h"

#define RAW_ROW 16u
#define INDENT  "        "

/* ---- Labels ----------------------------------------------------------- */

static uint32_t ram_end(const ksdis *d)
{
    return d->ram_base + d->h.data_size + d->h.bss_size;
}

/* Endereços onde o texto gerado pode pôr um label. */
static bool labelable(const ksdis *d, uint32_t a)
{
    return a <= d->h.code_size || (a >= d->ram_base && a <= ram_end(d));
}

/* Alvo que o ksas aceita por nome: BCOND/JMP/CALL exigem alvo dentro do
 * código (ksas: "alvo de desvio fora do codigo"); ADR/ADRL, qualquer
 * endereço que receba label. */
static bool target_ok(const ksdis *d, uint32_t w, uint32_t target)
{
    uint32_t op = w & 0xFFu;

    if (op == KSVM_OP_ADR || op == KSVM_OP_ADRL) {
        return labelable(d, target);
    }
    return target < d->h.code_size;
}

static const char *resolve_raw(void *ctx, uint32_t addr)
{
    ksdis *d = (ksdis *)ctx;
    const ksdis_sym *s = ksdis_sym_at(d, addr, true);

    if (s != NULL) {
        return s->name;
    }
    if (ksdis_has_label(d, addr)) {
        snprintf(d->namebuf, sizeof d->namebuf, "L_%04lX", (unsigned long)addr);
        return d->namebuf;
    }
    return NULL;
}

#define SEC_ANY 0xFFu

/* Símbolo em addr que vira label e ainda não foi escrito, da seção "sec"
 * (KSDIS_SYMK_CODE/DATA/BSS) ou de qualquer uma (SEC_ANY). */
static bool pending_sym(const ksdis_sym *s, uint8_t sec)
{
    return s->label_ok && !s->emitted &&
           (sec == SEC_ANY || (s->kind & KSDIS_SYMK_SECTION) == sec);
}

/* Escreve os labels pendentes em addr da seção "sec". Num endereço de
 * fronteira (fim do código = início da RAM, fim de .data = início de
 * .bss) cada símbolo sai na seção do seu kind, para o -g preservá-lo. */
static void emit_syms_at(ksdis *d, uint32_t addr, uint8_t sec)
{
    size_t i;

    for (i = ksdis_sym_lower(d, addr); i < d->nsyms && d->syms[i].addr == addr; i++) {
        if (pending_sym(&d->syms[i], sec)) {
            fprintf(d->out, "%s:\n", d->syms[i].name);
            d->syms[i].emitted = true;
        }
    }
}

static bool has_pending_sym(const ksdis *d, uint32_t addr, uint8_t sec)
{
    size_t i;

    for (i = ksdis_sym_lower(d, addr); i < d->nsyms && d->syms[i].addr == addr; i++) {
        if (pending_sym(&d->syms[i], sec)) {
            return true;
        }
    }
    return false;
}

static void emit_labels_at(ksdis *d, uint32_t addr)
{
    emit_syms_at(d, addr, SEC_ANY);
    if (ksdis_has_label(d, addr)) {
        fprintf(d->out, "L_%04lX:\n", (unsigned long)addr);
    }
}

/* ---- Varredura do código ---------------------------------------------- */

/* Bytes do item em pc: 8 só para formato L inteiro dentro do código. */
static uint32_t item_len(const ksdis *d, uint32_t pc)
{
    uint32_t rest = d->h.code_size - pc, len;

    if (rest < 4u) {
        return rest;
    }
    len = ksvm_disasm_len(ksdis_code_word(d, pc));
    return (len == 8u && rest < 8u) ? 4u : len;
}

/* Alvo da instrução completa em pc, se houver. */
static bool item_target(const ksdis *d, uint32_t pc, uint32_t len, uint32_t *target)
{
    uint32_t w, imm32 = 0u;

    if (len < 4u) {
        return false;
    }
    w = ksdis_code_word(d, pc);
    if (ksvm_disasm_len(w) != len) {
        return false;                          /* formato L truncado */
    }
    if (len == 8u) {
        imm32 = ksdis_code_word(d, pc + 4u);
    }
    return ksvm_disasm_target(pc, w, imm32, target);
}

static bool collect_labels(ksdis *d)
{
    uint32_t pc = 0u, target = 0u;

    while (pc < d->h.code_size) {
        uint32_t len = item_len(d, pc);
        if (item_target(d, pc, len, &target) && target_ok(d, ksdis_code_word(d, pc), target) &&
            ksdis_sym_at(d, target, true) == NULL && !ksdis_add_label(d, target)) {
            return false;
        }
        pc += len;
    }
    if (ksdis_sym_at(d, d->h.entry, true) == NULL && !ksdis_add_label(d, d->h.entry)) {
        return false;
    }
    ksdis_sort_labels(d);
    return true;
}

/* ---- Emissão ---------------------------------------------------------- */

/* n bytes em linhas .byte, quebradas a cada 16 e antes de cada label. */
static void emit_bytes(ksdis *d, const uint8_t *p, uint32_t addr, uint32_t n)
{
    uint32_t i = 0u;

    while (i < n) {
        uint32_t stop = (n - i > RAW_ROW) ? i + RAW_ROW : n, j;
        emit_labels_at(d, addr + i);
        stop = ksdis_next_label(d, addr + i + 1u, addr + stop) - addr;
        fputs(INDENT ".byte ", d->out);
        for (j = i; j < stop; j++) {
            fprintf(d->out, "0x%02X%s", (unsigned)p[j], (j + 1u < stop) ? ", " : "\n");
        }
        i = stop;
    }
}

static void emit_insn(ksdis *d, uint32_t pc, uint32_t len)
{
    char text[KSVM_DISASM_BUF];
    uint32_t w = ksdis_code_word(d, pc), imm32 = 0u, target = 0u;

    if (len == 8u) {
        imm32 = ksdis_code_word(d, pc + 4u);
    }
    emit_labels_at(d, pc);
    if (ksvm_disasm_len(w) != len) {
        fprintf(d->out, INDENT ".word 0x%08lX\n", (unsigned long)w);
        return;
    }
    (void)ksvm_disasm_one_ex(pc, w, imm32, resolve_raw, d, text, sizeof text);
    if (item_target(d, pc, len, &target) &&
        (!target_ok(d, w, target) || resolve_raw(d, target) == NULL)) {
        fprintf(d->out, INDENT ".word 0x%08lX", (unsigned long)w);
        if (len == 8u) {
            fprintf(d->out, ", 0x%08lX", (unsigned long)imm32);
        }
        fprintf(d->out, "    ; %s (alvo 0x%08lX)\n", text, (unsigned long)target);
        return;
    }
    fprintf(d->out, INDENT "%s\n", text);
}

static void emit_code(ksdis *d)
{
    uint32_t pc = 0u;

    fputs(INDENT ".text\n", d->out);
    while (pc < d->h.code_size) {
        uint32_t len = item_len(d, pc);
        if (len < 4u) {
            /* Bytes finais fora de palavra: o ksas alinha o fim de .text a 4,
             * o de .rodata não; assim code_size se mantém. */
            fputs(INDENT ".rodata\n", d->out);
        }
        if (len < 4u || ksdis_next_label(d, pc + 1u, pc + len) < pc + len) {
            emit_bytes(d, d->code + pc, pc, len);
        } else {
            emit_insn(d, pc, len);
        }
        pc += len;
    }
    emit_syms_at(d, d->h.code_size, KSDIS_SYMK_CODE);   /* fim de .text/.rodata */
    if (d->h.code_size != d->ram_base) {       /* senão saem no início da RAM */
        emit_labels_at(d, d->h.code_size);
    }
}

/* .space n quebrado antes de cada label. */
static void emit_space(ksdis *d, uint32_t addr, uint32_t n)
{
    uint32_t end = addr + n;

    while (addr < end) {
        uint32_t stop = ksdis_next_label(d, addr + 1u, end);
        emit_labels_at(d, addr);
        fprintf(d->out, INDENT ".space %lu\n", (unsigned long)(stop - addr));
        addr = stop;
    }
}

static void emit_ram(ksdis *d)
{
    uint32_t bss = d->ram_base + d->h.data_size;

    fputs(INDENT ".data\n", d->out);
    emit_bytes(d, d->data, d->ram_base, d->h.data_size);
    emit_syms_at(d, bss, KSDIS_SYMK_DATA);     /* labels no fim de .data */
    if (d->h.bss_size != 0u || has_pending_sym(d, bss, KSDIS_SYMK_BSS)) {
        fputs(INDENT ".bss\n", d->out);
        emit_syms_at(d, bss, KSDIS_SYMK_BSS);  /* .bss vazio: emit_space não roda */
        emit_space(d, bss, d->h.bss_size);
    }
    emit_labels_at(d, ram_end(d));
}

/* Símbolos que não viram label: constantes (kind ABS) e endereços fora do
 * código e da RAM da imagem. */
static void emit_equs(ksdis *d)
{
    size_t i;

    for (i = 0; i < d->nsyms; i++) {
        const ksdis_sym *s = &d->syms[i];
        if (s->name_ok && (!s->label_ok || !labelable(d, s->addr))) {
            fprintf(d->out, INDENT ".equ %s, 0x%08lX\n", s->name, (unsigned long)s->addr);
        }
    }
}

/* .global para os símbolos marcados globais; os locais só existem na
 * tabela se a imagem foi montada com "ksas -g". */
static void emit_globals(ksdis *d)
{
    bool locals = false;
    size_t i;

    for (i = 0; i < d->nsyms; i++) {
        if (!d->syms[i].name_ok) {
            continue;
        }
        if ((d->syms[i].kind & KSDIS_SYMK_GLOBAL) != 0u) {
            fprintf(d->out, INDENT ".global %s\n", d->syms[i].name);
        } else {
            locals = true;
        }
    }
    if (locals) {
        fputs("; simbolos locais na tabela: remonte com ksas -g\n", d->out);
    }
}

static void emit_preamble(ksdis *d)
{
    fprintf(d->out, "; gerado por ksdis --raw a partir de %s\n", d->path);
    if (d->h.ram_base != 0u && d->h.ram_base != ((d->h.code_size + 15u) & ~UINT32_C(15))) {
        fprintf(d->out, "; aviso: ram_base 0x%08lX do cabecalho nao tem diretiva\n",
                (unsigned long)d->h.ram_base);
    }
    fprintf(d->out, INDENT ".entry %s\n", resolve_raw(d, d->h.entry));
    fprintf(d->out, INDENT ".stack %lu\n", (unsigned long)d->h.stack_size);
    if ((d->h.flags & KSVM_KSX_FLAG_PIC) != 0u) {
        fputs(INDENT ".pic\n", d->out);
    }
    emit_globals(d);
    emit_equs(d);
}

bool ksdis_raw(ksdis *d)
{
    if (!collect_labels(d)) {
        return false;
    }
    emit_preamble(d);
    emit_code(d);
    emit_ram(d);
    return true;
}
