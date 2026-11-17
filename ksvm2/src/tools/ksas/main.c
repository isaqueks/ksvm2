/*
 * main.c — CLI do assembler (doc 07 §2):
 *
 *   ksas entrada.ksasm [-o saida.ksx] [-l listing.txt] [-g] [-DNOME[=valor]]
 *
 * Toda E/S de arquivo fica aqui; o montador em si (ksas_assemble) recebe
 * strings e callbacks. Código de saída: 0 ok, 1 erros de montagem, 2 uso
 * incorreto ou falha de E/S.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ksas.h"

#define EXIT_ASM   1
#define EXIT_USAGE 2

typedef struct cli {
    const char  *input;
    const char  *output;
    const char  *listing;
    bool         debug;
    const char **defines;
    size_t       num_defines;
    char        *default_output;   /* entrada com extensão .ksx */
} cli;

static void usage(FILE *f)
{
    fputs("uso: ksas entrada.ksasm [-o saida.ksx] [-l listing.txt] [-g] [-DNOME[=valor]]\n"
          "  -o ARQ        imagem .ksx de saida (padrao: entrada com extensao .ksx)\n"
          "  -l ARQ        listing: endereco, bytes e fonte lado a lado\n"
          "  -g            inclui todos os labels na tabela de simbolos\n"
          "  -DNOME=valor  define uma constante (como NOME = valor no fonte)\n", f);
}

/* Lê o arquivo inteiro como texto terminado em NUL; NULL + *err em falha. */
static char *read_file(const char *path, const char **err)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL, *nb;
    size_t len = 0, cap = 0, n;

    if (f == NULL) {
        *err = "arquivo nao encontrado ou sem permissao";
        return NULL;
    }
    do {
        if (cap - len < 4096u) {
            cap = (cap == 0u) ? 8192u : cap * 2u;
            nb = realloc(buf, cap + 1u);
            if (nb == NULL) {
                free(buf);
                fclose(f);
                *err = "memoria insuficiente";
                return NULL;
            }
            buf = nb;
        }
        n = fread(buf + len, 1u, cap - len, f);
        len += n;
    } while (n != 0u);
    if (ferror(f) || memchr(buf, '\0', len) != NULL) {
        *err = ferror(f) ? "erro de leitura" : "arquivo contem byte NUL (nao e texto)";
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

static bool is_absolute(const char *p)
{
    return p[0] == '/' || p[0] == '\\' || (p[0] != '\0' && p[1] == ':');
}

/* .include "x": relativo ao diretório do arquivo que inclui. */
static char *include_file(void *ctx, const char *from, const char *path, const char **err)
{
    const char *slash = NULL, *p;
    size_t dir_len;
    char *full, *text;

    (void)ctx;
    for (p = from; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            slash = p;
        }
    }
    if (is_absolute(path) || slash == NULL) {
        return read_file(path, err);
    }
    dir_len = (size_t)(slash - from) + 1u;
    full = malloc(dir_len + strlen(path) + 1u);
    if (full == NULL) {
        *err = "memoria insuficiente";
        return NULL;
    }
    memcpy(full, from, dir_len);
    strcpy(full + dir_len, path);
    text = read_file(full, err);
    free(full);
    return text;
}

static void print_diag(void *ctx, const ksas_diag *d)
{
    (void)ctx;
    fprintf(stderr, "%s\n", d->text);
}

static void write_listing_line(void *ctx, const char *line)
{
    fputs(line, (FILE *)ctx);
}

static char *replace_extension(const char *in, const char *ext)
{
    const char *dot = strrchr(in, '.');
    const char *sep1 = strrchr(in, '/'), *sep2 = strrchr(in, '\\');
    const char *sep = (sep1 > sep2) ? sep1 : sep2;
    size_t base = (dot != NULL && (sep == NULL || dot > sep)) ? (size_t)(dot - in) : strlen(in);
    char *out = malloc(base + strlen(ext) + 1u);

    if (out != NULL) {
        memcpy(out, in, base);
        strcpy(out + base, ext);
    }
    return out;
}

/* Opção com argumento: "-o ARQ" ou "-oARQ". */
static const char *option_arg(int argc, char **argv, int *i)
{
    if (argv[*i][2] != '\0') {
        return argv[*i] + 2;
    }
    if (*i + 1 < argc) {
        (*i)++;
        return argv[*i];
    }
    return NULL;
}

static bool parse_option(cli *o, int argc, char **argv, int *i)
{
    const char *a = argv[*i];

    switch (a[1]) {
    case 'o': o->output = option_arg(argc, argv, i);  return o->output != NULL;
    case 'l': o->listing = option_arg(argc, argv, i); return o->listing != NULL;
    case 'D': {
        const char *d = option_arg(argc, argv, i);
        if (d == NULL) {
            return false;
        }
        o->defines[o->num_defines++] = d;
        return true;
    }
    case 'g': o->debug = (a[2] == '\0'); return o->debug;
    default:  return false;
    }
}

static int parse_args(cli *o, int argc, char **argv)
{
    int i;

    memset(o, 0, sizeof *o);
    o->defines = calloc((size_t)argc + 1u, sizeof *o->defines);
    if (o->defines == NULL) {
        return EXIT_USAGE;
    }
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return EXIT_SUCCESS;
        }
        if (a[0] == '-' && a[1] != '\0') {
            if (!parse_option(o, argc, argv, &i)) {
                fprintf(stderr, "ksas: opcao invalida ou sem argumento: %s\n", a);
                usage(stderr);
                return EXIT_USAGE;
            }
        } else if (o->input == NULL) {
            o->input = a;
        } else {
            fprintf(stderr, "ksas: mais de um arquivo de entrada (%s)\n", a);
            return EXIT_USAGE;
        }
    }
    if (o->input == NULL) {
        usage(stderr);
        return EXIT_USAGE;
    }
    return -1;   /* continuar */
}

static bool write_file(const char *path, const uint8_t *data, uint32_t size)
{
    FILE *f = fopen(path, "wb");
    bool ok;

    if (f == NULL) {
        return false;
    }
    ok = fwrite(data, 1u, size, f) == size;
    return (fclose(f) == 0) && ok;
}

static int assemble(cli *o, const char *src)
{
    ksas_options opt;
    ksas_result res;
    FILE *lst = NULL;
    int rc;

    memset(&opt, 0, sizeof opt);
    opt.filename = o->input;
    opt.defines = o->defines;
    opt.num_defines = o->num_defines;
    opt.debug_symbols = o->debug;
    opt.diag = print_diag;
    opt.include = include_file;
    if (o->listing != NULL) {
        lst = fopen(o->listing, "w");
        if (lst == NULL) {
            fprintf(stderr, "ksas: nao foi possivel criar %s\n", o->listing);
            return EXIT_USAGE;
        }
        opt.listing = write_listing_line;
        opt.listing_ctx = lst;
    }
    rc = (ksas_assemble(src, &opt, &res) == 0) ? EXIT_SUCCESS : EXIT_ASM;
    if (lst != NULL && fclose(lst) != 0) {
        fprintf(stderr, "ksas: erro ao gravar %s\n", o->listing);
        rc = EXIT_USAGE;
    }
    if (rc == EXIT_SUCCESS && !write_file(o->output, res.image, res.image_size)) {
        fprintf(stderr, "ksas: nao foi possivel gravar %s\n", o->output);
        rc = EXIT_USAGE;
    }
    if (rc != EXIT_SUCCESS && lst != NULL) {
        (void)remove(o->listing);   /* o listing só existe junto com a imagem */
    }
    if (rc == EXIT_ASM) {
        fprintf(stderr, "ksas: %u erro(s); nenhuma saida gerada\n", res.errors);
    }
    ksas_result_free(&res);
    return rc;
}

int main(int argc, char **argv)
{
    cli o;
    const char *err = NULL;
    char *src;
    int rc = parse_args(&o, argc, argv);

    if (rc < 0) {
        if (o.output == NULL) {
            o.default_output = replace_extension(o.input, ".ksx");
            o.output = o.default_output;
        }
        src = (o.output != NULL) ? read_file(o.input, &err) : NULL;
        if (src == NULL) {
            fprintf(stderr, "ksas: %s: %s\n", o.input, err != NULL ? err : "memoria insuficiente");
            rc = EXIT_USAGE;
        } else {
            rc = assemble(&o, src);
            free(src);
        }
    }
    free(o.default_output);
    free(o.defines);
    return rc;
}
