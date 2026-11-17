/*
 * ksvm — runner de imagens .ksx (doc 07 §5).
 *
 *   ksvm prog.ksx [--ram N] [--stack N] [--max-insns N] [--trace] [--step]
 *                 [--regs] [--no-fs] [--stdin-block | --stdin-poll]
 *
 * Saída do guest em stdout (HAL do host: hal_posix ou hal_win32); trace,
 * registradores e mensagens do runner em stderr. Código de saída:
 *   exit_code do guest (& 0xFF)       término normal (HALT / EXIT)
 *   70 + código do fault              "fault: NOME at pc=0x... addr=0x..."
 *   124                               --max-insns esgotado
 *   130                               'q' no modo --step
 *   64 uso, 65 imagem inválida, 66 arquivo ilegível, 69 sem memória
 *
 * Exige KSVM_ENABLE_TRACE=1 para --trace/--step e para --regs em BRK.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L                /* isatty, fileno */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ksvm/disasm.h"
#include "ksvm/ksvm.h"
#include "toolio.h"

#ifdef _WIN32
#include <io.h>
#include "ksvm/hal_win32.h"
typedef ksvm_hal_win32 host_hal;
#define host_hal_init      ksvm_hal_win32_init
#define host_hal_flush     ksvm_hal_win32_flush
#define host_hal_close_all ksvm_hal_win32_close_all
#define host_stdin_is_tty() (_isatty(_fileno(stdin)) != 0)
#define TTY_PATH "CONIN$"
#else
#include <unistd.h>
#include "ksvm/hal_posix.h"
typedef ksvm_hal_posix host_hal;
#define host_hal_init      ksvm_hal_posix_init
#define host_hal_flush     ksvm_hal_posix_flush
#define host_hal_close_all ksvm_hal_posix_close_all
#define host_stdin_is_tty() (isatty(fileno(stdin)) != 0)
#define TTY_PATH "/dev/tty"
#endif

#define PROG "ksvm"

enum {
    EXIT_USAGE = 64, EXIT_IMAGE = 65, EXIT_NOINPUT = 66, EXIT_NOMEM = 69,
    EXIT_FAULT_BASE = 70, EXIT_LIMIT = 124, EXIT_QUIT = 130
};

#define DEFAULT_RAM   (UINT32_C(1024) * 1024u)
#define MAX_RAM       (UINT32_C(1024) * 1024u * 1024u)
#define RUN_SLICE     UINT32_C(65536)

typedef struct options {
    const char *path;
    uint32_t    ram_size;
    uint32_t    max_insns;      /* 0 = sem limite */
    uint32_t    stack_size;
    bool        stack_set;
    bool        trace, step, regs, no_fs;
    int         stdin_mode;     /* -1 automático, 0 poll, 1 bloqueante */
} options;

typedef struct runner {
    options   opt;
    ksvm      vm;
    ksvm_hal  hal;
    host_hal  hh;
    uint8_t  *image;
    uint32_t  image_size;
    uint8_t  *ram;
    FILE     *tty;              /* --step: de onde vem o Enter */
    bool      quit;
} runner;

/* ---- Linha de comando ------------------------------------------------- */

static void usage(void)
{
    fputs("uso: " PROG " prog.ksx [--ram N] [--stack N] [--max-insns N] [--trace]\n"
          "            [--step] [--regs] [--no-fs] [--stdin-block | --stdin-poll]\n"
          "  N aceita decimal, 0x hex e sufixos k/m (ex.: --ram 64k)\n", stderr);
}

static bool number_arg(int argc, char **argv, int *i, uint32_t *out)
{
    if (*i + 1 >= argc || !tool_parse_u32(argv[*i + 1], out)) {
        fprintf(stderr, PROG ": %s espera um numero\n", argv[*i]);
        return false;
    }
    (*i)++;
    return true;
}

/* Opções sem argumento; false se a opção não é uma delas. */
static bool flag_arg(const char *a, options *o)
{
    if (strcmp(a, "--trace") == 0)            { o->trace = true; }
    else if (strcmp(a, "--step") == 0)        { o->step = true; }
    else if (strcmp(a, "--regs") == 0)        { o->regs = true; }
    else if (strcmp(a, "--no-fs") == 0)       { o->no_fs = true; }
    else if (strcmp(a, "--stdin-block") == 0) { o->stdin_mode = 1; }
    else if (strcmp(a, "--stdin-poll") == 0)  { o->stdin_mode = 0; }
    else { return false; }
    return true;
}

static bool parse_args(int argc, char **argv, options *o)
{
    int i;
    bool ok = true;

    memset(o, 0, sizeof *o);
    o->ram_size = DEFAULT_RAM;
    o->stdin_mode = -1;
    for (i = 1; i < argc && ok; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--ram") == 0) {
            ok = number_arg(argc, argv, &i, &o->ram_size);
        } else if (strcmp(a, "--max-insns") == 0) {
            ok = number_arg(argc, argv, &i, &o->max_insns);
        } else if (strcmp(a, "--stack") == 0) {
            ok = number_arg(argc, argv, &i, &o->stack_size);
            o->stack_set = true;
        } else if (!flag_arg(a, o)) {
            ok = (a[0] != '-' && o->path == NULL);
            o->path = a;
        }
    }
    if (ok && (o->path == NULL || o->ram_size == 0u || o->ram_size > MAX_RAM)) {
        ok = false;
    }
    if (!ok) {
        usage();
    }
    return ok;
}

/* ---- Saída de diagnóstico --------------------------------------------- */

static void flags_str(uint32_t f, char out[5])
{
    out[0] = (f & KSVM_FLAG_Z) ? 'Z' : '-';
    out[1] = (f & KSVM_FLAG_N) ? 'N' : '-';
    out[2] = (f & KSVM_FLAG_C) ? 'C' : '-';
    out[3] = (f & KSVM_FLAG_V) ? 'V' : '-';
    out[4] = '\0';
}

static const char *reg_name(unsigned i, char buf[12])
{
    if (i == KSVM_REG_ZR) {
        return "zr";
    }
    if (i == KSVM_REG_FP) {
        return "fp";
    }
    if (i == KSVM_REG_SP) {
        return "sp";
    }
    snprintf(buf, 12, "r%u", i);
    return buf;
}

static void dump_regs(const ksvm *vm)
{
    char fl[5], nb[12];
    unsigned i;

    flags_str(vm->cpu.flags, fl);
    fprintf(stderr, "pc=0x%08lX flags=%s insns=%lu\n", (unsigned long)vm->cpu.pc, fl,
            (unsigned long)vm->insn_count);
    for (i = 0; i < KSVM_NUM_REGS; i++) {
        fprintf(stderr, "%3s=0x%08lX%s", reg_name(i, nb), (unsigned long)vm->cpu.r[i],
                (i % 4u == 3u) ? "\n" : "  ");
    }
}

/* ---- Trace ------------------------------------------------------------ */

#if KSVM_ENABLE_TRACE

/* "pc: bytes  instrução  flags" para a instrução recém-executada. */
static void trace_line(runner *r, uint32_t pc, uint32_t w)
{
    char text[KSVM_DISASM_BUF], bytes[32], fl[5];
    uint32_t imm32 = 0u, len = ksvm_disasm_len(w), i;
    size_t pos = 0u;

    if (len == 8u) {
        (void)ksvm_bus_fetch32(&r->vm.bus, pc + 4u, &imm32);   /* já buscado pela VM */
    }
    (void)ksvm_disasm_one(pc, w, imm32, text, sizeof text);
    for (i = 0; i < len; i++) {
        uint32_t word = (i < 4u) ? w : imm32;
        unsigned b = (unsigned)((word >> (8u * (i % 4u))) & 0xFFu);
        pos += (size_t)snprintf(bytes + pos, sizeof bytes - pos, (i == 0u) ? "%02X" : " %02X", b);
    }
    flags_str(r->vm.cpu.flags, fl);
    fprintf(stderr, "%08lX: %-23s  %-28s %s\n", (unsigned long)pc, bytes, text, fl);
}

/* Espera Enter no terminal: 'c' continua sem parar, 'q' encerra. */
static void step_wait(runner *r)
{
    char line[64];
    FILE *in = (r->tty != NULL) ? r->tty : stdin;

    fputs("[step] Enter = proxima, c = continuar, q = sair: ", stderr);
    fflush(stderr);
    if (fgets(line, sizeof line, in) == NULL || line[0] == 'q') {
        r->quit = true;
    } else if (line[0] == 'c') {
        r->opt.step = false;
    }
}

static void trace_hook(void *ctx, const struct ksvm *vm, ksvm_trace_event ev,
                       uint32_t pc, uint32_t word)
{
    runner *r = (runner *)ctx;

    host_hal_flush(&r->hh);                    /* saída do guest antes do trace */
    if (ev == KSVM_TRACE_BRK) {
        fprintf(stderr, "brk at pc=0x%08lX\n", (unsigned long)pc);
        dump_regs(vm);
    } else if (r->opt.trace || r->opt.step) {
        trace_line(r, pc, word);
    }
    if (r->opt.step && !r->quit) {
        if (r->opt.regs && ev != KSVM_TRACE_BRK) {
            dump_regs(vm);
        }
        step_wait(r);
    }
}

static bool install_trace(runner *r)
{
    if (r->opt.trace || r->opt.step || r->opt.regs) {
        ksvm_set_trace(&r->vm, trace_hook, r);
    }
    if (r->opt.step) {
        r->tty = fopen(TTY_PATH, "r");
    }
    return true;
}

#else

static bool install_trace(runner *r)
{
    if (r->opt.trace || r->opt.step) {
        fputs(PROG ": compilado sem KSVM_ENABLE_TRACE; --trace/--step indisponiveis\n", stderr);
        return false;
    }
    return true;
}

#endif /* KSVM_ENABLE_TRACE */

/* ---- Carga e execução ------------------------------------------------- */

static int load(runner *r)
{
    int err;

    if (!tool_read_file(PROG, r->opt.path, &r->image, &r->image_size)) {
        return EXIT_NOINPUT;
    }
    if (r->opt.stack_set && r->image_size >= KSVM_KSX_HEADER_SIZE) {
        /* stack_size fica fora da faixa do checksum ([0x38, fim)). */
        ksvm_st32le(r->image + KSVM_KSX_OFF_STACK_SIZE, r->opt.stack_size);
    }
    r->ram = (uint8_t *)calloc(1u, (size_t)r->opt.ram_size);
    if (r->ram == NULL) {
        fprintf(stderr, PROG ": sem memoria para %lu bytes de RAM\n",
                (unsigned long)r->opt.ram_size);
        return EXIT_NOMEM;
    }
    err = ksvm_load_image(&r->vm, r->image, r->image_size, r->ram, r->opt.ram_size);
    if (err != KSVM_OK) {
        fprintf(stderr, PROG ": %s: imagem invalida: %s\n", r->opt.path, tool_err_name(err));
        return EXIT_IMAGE;
    }
    return 0;
}

static void setup_hal(runner *r)
{
    host_hal_init(&r->hh, &r->hal);
    r->hh.fs_enabled = !r->opt.no_fs;
    r->hh.stdin_block = (r->opt.stdin_mode < 0) ? !host_stdin_is_tty()
                                                : (r->opt.stdin_mode == 1);
    ksvm_init(&r->vm, &r->hal);
}

/* Roda até parar; devolve 0 ou EXIT_LIMIT/EXIT_QUIT. */
static int run(runner *r)
{
    uint32_t left = r->opt.max_insns;

    for (;;) {
        uint32_t slice = r->opt.step ? 1u : RUN_SLICE, before = r->vm.insn_count;
        ksvm_status st;
        if (r->opt.max_insns != 0u) {
            if (left == 0u) {
                return EXIT_LIMIT;
            }
            slice = (left < slice) ? left : slice;
        }
        st = ksvm_run(&r->vm, slice);
        if (r->quit) {
            return EXIT_QUIT;
        }
        if (st == KSVM_HALTED || st == KSVM_FAULT) {
            return 0;
        }
        if (r->opt.max_insns != 0u) {
            uint32_t done = r->vm.insn_count - before;
            left -= (done < left) ? done : left;
        }
        if (st == KSVM_YIELD && r->hal.sleep_ms != NULL) {
            r->hal.sleep_ms(r->hal.ctx, 1u);   /* WFI/YIELD: não girar a CPU */
        }
    }
}

static int finish(runner *r, int run_rc)
{
    host_hal_close_all(&r->hh);
    fflush(stdout);
    if (r->opt.regs) {
        dump_regs(&r->vm);
    }
    if (run_rc == EXIT_LIMIT) {
        fprintf(stderr, PROG ": limite de %lu instrucoes atingido (pc=0x%08lX)\n",
                (unsigned long)r->opt.max_insns, (unsigned long)r->vm.cpu.pc);
        return run_rc;
    }
    if (run_rc != 0) {
        return run_rc;
    }
    if (r->vm.status == KSVM_FAULT) {
        fprintf(stderr, "fault: %s at pc=0x%08lX addr=0x%08lX\n", ksvm_fault_name(r->vm.fault),
                (unsigned long)r->vm.cpu.pc, (unsigned long)r->vm.fault_addr);
        return EXIT_FAULT_BASE + (int)r->vm.fault;
    }
    return (int)(r->vm.exit_code & 0xFFu);
}

int main(int argc, char **argv)
{
    static runner r;                           /* struct grande: fora da pilha */
    int rc;

    if (!parse_args(argc, argv, &r.opt)) {
        return EXIT_USAGE;
    }
    setup_hal(&r);
    rc = load(&r);
    if (rc == 0) {
        rc = install_trace(&r) ? finish(&r, run(&r)) : EXIT_USAGE;
    }
    if (r.tty != NULL) {
        fclose(r.tty);
    }
    free(r.ram);
    free(r.image);
    return rc;
}
