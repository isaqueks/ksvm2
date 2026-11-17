/*
 * ksimg — converte um .ksx num header C (doc 07 §6, doc 05 §3.2):
 *
 *   ksimg prog.ksx -o image.h [--name sym] [--progmem]
 *
 * Gera "static const uint8_t [PROGMEM] sym[] = {...};" e
 * "#define sym_SIZE n". Com --progmem o header inclui <avr/pgmspace.h> sob
 * __AVR__ e define PROGMEM vazio nos demais compiladores, então o mesmo
 * header compila no host. A imagem é validada (ksvm_image_parse) antes.
 * Sem -o (ou com "-o -") escreve em stdout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ksvm/loader.h"
#include "../toolio.h"

#define PROG "ksimg"
#define BYTES_PER_LINE 12u

typedef struct options {
    const char *in;
    const char *out;
    const char *name;
    bool        progmem;
} options;

static void usage(void)
{
    fprintf(stderr, "uso: " PROG " prog.ksx [-o image.h] [--name simbolo] [--progmem]\n");
}

/* 0 ok, 1 erro (já reportado). */
static int parse_args(int argc, char **argv, options *o)
{
    int i;

    o->in = NULL;
    o->out = NULL;
    o->name = "ksvm_image";
    o->progmem = false;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            o->out = argv[++i];
        } else if (strcmp(a, "--name") == 0 && i + 1 < argc) {
            o->name = argv[++i];
        } else if (strcmp(a, "--progmem") == 0) {
            o->progmem = true;
        } else if (a[0] != '-' && o->in == NULL) {
            o->in = a;
        } else {
            usage();
            return 1;
        }
    }
    if (o->in == NULL) {
        usage();
        return 1;
    }
    if (!tool_is_ident(o->name)) {
        fprintf(stderr, PROG ": --name '%s' nao e um identificador C\n", o->name);
        return 1;
    }
    return 0;
}

/* Nome do arquivo sem diretórios, para o comentário do header. */
static const char *base_name(const char *path)
{
    const char *b = path, *p;

    for (p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            b = p + 1;
        }
    }
    return b;
}

static void put_guard(FILE *f, const char *name)
{
    const char *p;

    fputs("KSIMG_", f);
    for (p = name; *p != '\0'; p++) {
        char c = *p;
        fputc((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c, f);
    }
    fputs("_H", f);
}

static void write_prologue(FILE *f, const options *o, const ksvm_image_header *h)
{
    fprintf(f, "/*\n * Gerado por " PROG " a partir de %s. Nao editar.\n", base_name(o->in));
    fprintf(f, " * entry 0x%08lX, codigo %lu, dados %lu, bss %lu, pilha %lu bytes.\n */\n",
            (unsigned long)h->entry, (unsigned long)h->code_size,
            (unsigned long)h->data_size, (unsigned long)h->bss_size,
            (unsigned long)h->stack_size);
    fputs("#ifndef ", f);
    put_guard(f, o->name);
    fputs("\n#define ", f);
    put_guard(f, o->name);
    fputs("\n\n#include <stdint.h>\n", f);
    if (o->progmem) {
        fputs("\n#if defined(__AVR__)\n#include <avr/pgmspace.h>\n#endif\n"
              "#ifndef PROGMEM\n#define PROGMEM\n#endif\n", f);
    }
}

static void write_array(FILE *f, const options *o, const uint8_t *img, uint32_t size)
{
    uint32_t i;

    fprintf(f, "\nstatic const uint8_t %s%s[] = {", o->progmem ? "PROGMEM " : "", o->name);
    for (i = 0; i < size; i++) {
        fputs((i % BYTES_PER_LINE == 0u) ? "\n    " : " ", f);
        fprintf(f, "0x%02X%s", (unsigned)img[i], (i + 1u < size) ? "," : "");
    }
    fprintf(f, "\n};\n\n#define %s_SIZE %luu\n\n#endif\n", o->name, (unsigned long)size);
}

static int emit(const options *o, const uint8_t *img, uint32_t size,
                const ksvm_image_header *h)
{
    bool to_stdout = (o->out == NULL || strcmp(o->out, "-") == 0);
    FILE *f = to_stdout ? stdout : fopen(o->out, "w");
    bool ok;

    if (f == NULL) {
        perror(o->out);
        return 1;
    }
    write_prologue(f, o, h);
    write_array(f, o, img, size);
    ok = (fflush(f) == 0 && !ferror(f));
    if (!to_stdout && fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, PROG ": erro ao escrever %s\n", to_stdout ? "stdout" : o->out);
    }
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    options o;
    ksvm_image_header h;
    uint8_t *img = NULL;
    uint32_t size = 0u;
    int err, rc;

    if (parse_args(argc, argv, &o) != 0) {
        return 2;
    }
    if (!tool_read_file(PROG, o.in, &img, &size)) {
        return 1;
    }
    err = ksvm_image_parse(img, size, NULL, &h);
    if (err != KSVM_OK) {
        fprintf(stderr, PROG ": %s: imagem invalida: %s\n", o.in, tool_err_name(err));
        free(img);
        return 1;
    }
    rc = emit(&o, img, size, &h);
    free(img);
    return rc;
}
