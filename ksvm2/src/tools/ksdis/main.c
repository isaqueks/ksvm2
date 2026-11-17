/*
 * ksdis — disassembler de imagens .ksx (doc 07 §4).
 *
 *   ksdis prog.ksx [--raw] [-o saida]
 *
 * Sem --raw: cabeçalho, símbolos, código (endereço, bytes, instrução, alvo)
 * e dados em hex + ASCII. Com --raw: texto reassemblável pelo ksas.
 * Códigos de saída: 0 ok, 1 erro de arquivo/imagem, 2 uso.
 */
#include <stdlib.h>
#include <string.h>

#include "ksdis.h"
#include "../toolio.h"

#define PROG "ksdis"

typedef struct options {
    const char *in;
    const char *out;
    bool        raw;
} options;

static int parse_args(int argc, char **argv, options *o)
{
    int i;

    o->in = NULL;
    o->out = NULL;
    o->raw = false;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--raw") == 0) {
            o->raw = true;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            o->out = argv[++i];
        } else if (argv[i][0] != '-' && o->in == NULL) {
            o->in = argv[i];
        } else {
            o->in = NULL;
            break;
        }
    }
    if (o->in == NULL) {
        fprintf(stderr, "uso: " PROG " prog.ksx [--raw] [-o saida]\n");
        return 1;
    }
    return 0;
}

/* Valida a imagem e calcula o layout efetivo; false = erro já reportado. */
static bool open_image(ksdis *d)
{
    int err = ksvm_image_parse(d->img, d->img_size, NULL, &d->h);
    uint64_t end;

    if (err != KSVM_OK) {
        fprintf(stderr, PROG ": %s: imagem invalida: %s\n", d->path, tool_err_name(err));
        return false;
    }
    d->code = d->img + d->h.code_off;
    d->data = d->img + d->h.data_off;
    d->ram_base = d->h.ram_base;
    if (d->ram_base == 0u) {
        d->ram_base = (d->h.code_size + 15u) & ~UINT32_C(15);
    }
    end = (uint64_t)d->ram_base + d->h.data_size + d->h.bss_size;
    if (end > UINT32_C(0xFFFFFFFF) || d->ram_base < d->h.code_size) {
        fprintf(stderr, PROG ": %s: RAM fora do espaco de enderecos\n", d->path);
        return false;
    }
    if (!ksdis_load_syms(d)) {
        fprintf(stderr, PROG ": %s: aviso: tabela de simbolos invalida, ignorada\n", d->path);
        ksdis_free(d);
    }
    return true;
}

static int disassemble(ksdis *d, const options *o)
{
    bool to_stdout = (o->out == NULL || strcmp(o->out, "-") == 0);
    bool ok = true;

    d->out = to_stdout ? stdout : fopen(o->out, "w");
    if (d->out == NULL) {
        perror(o->out);
        return 1;
    }
    if (o->raw) {
        ok = ksdis_raw(d);
    } else {
        ksdis_listing(d);
    }
    if (fflush(d->out) != 0 || ferror(d->out)) {
        ok = false;
    }
    if (!to_stdout && fclose(d->out) != 0) {
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, PROG ": erro ao escrever a saida\n");
    }
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    options o;
    ksdis d;
    uint8_t *img = NULL;
    int rc = 1;

    if (parse_args(argc, argv, &o) != 0) {
        return 2;
    }
    memset(&d, 0, sizeof d);
    d.path = o.in;
    if (!tool_read_file(PROG, o.in, &img, &d.img_size)) {
        return 1;
    }
    d.img = img;
    if (open_image(&d)) {
        rc = disassemble(&d, &o);
    }
    ksdis_free(&d);
    free(img);
    return rc;
}
