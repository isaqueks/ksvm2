/*
 * symtab.c — tabela de símbolos do .ksx (doc 05 §2: registros
 * "u32 addr, u8 kind, u8 len, char name[len]") e labels gerados.
 */
#include <stdlib.h>
#include <string.h>

#include "ksdis.h"
#include "ksvm/bytes.h"
#include "../toolio.h"

#define SYM_REC_HEAD 6u   /* addr(4) + kind(1) + len(1) */

uint32_t ksdis_code_word(const ksdis *d, uint32_t off)
{
    return ksvm_ld32le(d->code + off);
}

/* Nomes que o ksas leria como registrador não servem de label. O ksas não
 * diferencia maiúsculas em registradores ("R1", "SP"): compara em
 * minúsculas. Nenhum nome de registrador passa de 3 caracteres. */
static bool is_reg_name(const char *name)
{
    static const char *const fixed[] = { "zr", "fp", "sp" };
    static const char prefixes[] = "rats";
    char s[4];
    size_t i;

    for (i = 0; name[i] != '\0'; i++) {
        if (i == 3u) {
            return false;
        }
        s[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] - 'A' + 'a') : name[i];
    }
    s[i] = '\0';
    if (i < 2u) {
        return false;
    }
    for (i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
        if (strcmp(s, fixed[i]) == 0) {
            return true;
        }
    }
    if (strchr(prefixes, s[0]) == NULL || s[1] < '0' || s[1] > '9') {
        return false;
    }
    for (i = 2; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return i <= 3u;                            /* r0..r99 e afins */
}

static int sym_cmp(const void *a, const void *b)
{
    const ksdis_sym *x = (const ksdis_sym *)a, *y = (const ksdis_sym *)b;

    if (x->addr != y->addr) {
        return (x->addr < y->addr) ? -1 : 1;
    }
    return (x->index < y->index) ? -1 : (x->index > y->index);
}

/* Conta os registros; false se algum ultrapassa a seção ou tem nome vazio. */
static bool count_syms(const uint8_t *p, uint32_t size, size_t *n)
{
    uint32_t off = 0u;

    *n = 0u;
    while (off < size) {
        uint32_t len;
        if (size - off < SYM_REC_HEAD) {
            return false;
        }
        len = p[off + 5u];
        if (len == 0u || size - off - SYM_REC_HEAD < len) {
            return false;
        }
        off += SYM_REC_HEAD + len;
        (*n)++;
    }
    return true;
}

static bool fill_syms(ksdis *d, const uint8_t *p, size_t n)
{
    uint32_t off = 0u;
    size_t i;

    for (i = 0; i < n; i++) {
        ksdis_sym *s = &d->syms[i];
        uint32_t len = p[off + 5u];
        s->addr = ksvm_ld32le(p + off);
        s->kind = p[off + 4u];
        s->index = (uint32_t)i;
        s->name = (char *)malloc((size_t)len + 1u);
        if (s->name == NULL) {
            d->nsyms = i;
            return false;
        }
        memcpy(s->name, p + off + SYM_REC_HEAD, (size_t)len);
        s->name[len] = '\0';
        s->name_ok = strlen(s->name) == (size_t)len && tool_is_ident(s->name) &&
                     !is_reg_name(s->name);
        s->label_ok = s->name_ok && (s->kind & KSDIS_SYMK_SECTION) != KSDIS_SYMK_ABS;
        off += SYM_REC_HEAD + len;
    }
    d->nsyms = n;
    return true;
}

bool ksdis_load_syms(ksdis *d)
{
    const uint8_t *p = d->img + d->h.sym_off;
    size_t n = 0u;

    d->syms = NULL;
    d->nsyms = 0u;
    if (d->h.sym_size == 0u) {
        return true;
    }
    if (!count_syms(p, d->h.sym_size, &n)) {
        return false;
    }
    d->syms = (ksdis_sym *)calloc(n, sizeof *d->syms);
    if (d->syms == NULL || !fill_syms(d, p, n)) {
        return false;
    }
    qsort(d->syms, d->nsyms, sizeof *d->syms, sym_cmp);
    return true;
}

void ksdis_free(ksdis *d)
{
    size_t i;

    for (i = 0; i < d->nsyms; i++) {
        free(d->syms[i].name);
    }
    free(d->syms);
    free(d->labels);
    d->syms = NULL;
    d->labels = NULL;
    d->nsyms = d->nlabels = d->cap_labels = 0u;
}

size_t ksdis_sym_lower(const ksdis *d, uint32_t addr)
{
    size_t lo = 0u, hi = d->nsyms;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (d->syms[mid].addr < addr) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

const ksdis_sym *ksdis_sym_at(const ksdis *d, uint32_t addr, bool labels_only)
{
    size_t i;

    for (i = ksdis_sym_lower(d, addr); i < d->nsyms && d->syms[i].addr == addr; i++) {
        if (!labels_only || d->syms[i].label_ok) {
            return &d->syms[i];
        }
    }
    return NULL;
}

/* ---- Labels gerados --------------------------------------------------- */

bool ksdis_add_label(ksdis *d, uint32_t addr)
{
    if (d->nlabels == d->cap_labels) {
        size_t cap = (d->cap_labels == 0u) ? 64u : d->cap_labels * 2u;
        uint32_t *nl = (uint32_t *)realloc(d->labels, cap * sizeof *nl);
        if (nl == NULL) {
            return false;
        }
        d->labels = nl;
        d->cap_labels = cap;
    }
    d->labels[d->nlabels] = addr;
    d->nlabels++;
    return true;
}

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

    return (x < y) ? -1 : (x > y);
}

/* Ordena e remove repetidos. */
void ksdis_sort_labels(ksdis *d)
{
    size_t i, n = 0u;

    if (d->nlabels == 0u) {
        return;
    }
    qsort(d->labels, d->nlabels, sizeof *d->labels, u32_cmp);
    for (i = 1; i < d->nlabels; i++) {
        if (d->labels[i] != d->labels[n]) {
            n++;
            d->labels[n] = d->labels[i];
        }
    }
    d->nlabels = n + 1u;
}

static size_t label_lower(const ksdis *d, uint32_t addr)
{
    size_t lo = 0u, hi = d->nlabels;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (d->labels[mid] < addr) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

bool ksdis_has_label(const ksdis *d, uint32_t addr)
{
    size_t i = label_lower(d, addr);

    return i < d->nlabels && d->labels[i] == addr;
}

uint32_t ksdis_next_label(const ksdis *d, uint32_t lo, uint32_t hi)
{
    size_t i = label_lower(d, lo);
    uint32_t best = hi;

    if (i < d->nlabels && d->labels[i] < best) {
        best = d->labels[i];
    }
    for (i = ksdis_sym_lower(d, lo); i < d->nsyms && d->syms[i].addr < best; i++) {
        if (d->syms[i].label_ok) {
            best = d->syms[i].addr;
            break;
        }
    }
    return best;
}
