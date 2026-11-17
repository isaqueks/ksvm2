/*
 * mkimg.c — gera as imagens .ksx usadas por test_runner.sh, montadas com
 * ksvm_enc_* + ksvm_ksx_write (independe do ksas).
 *
 *   mkimg NOME saida.ksx       NOME: hello halt42 fault trap loop echo files
 *                                    stack rawedge
 *
 * Cada programa é montado duas vezes: a primeira só mede o código, para
 * que a segunda conheça ram_base (= código alinhado a 16, escolha do
 * loader) e possa usar endereços absolutos de .data em MOVIL.
 */
#include <stdio.h>
#include <string.h>

#include "ksvm/ksvm.h"

#define MAX_WORDS 256u
#define MAX_DATA  256u
#define MAX_SYM   512u

typedef struct prog {
    uint32_t code[MAX_WORDS];
    uint32_t n;
    uint8_t  data[MAX_DATA];
    uint32_t data_size;
    uint32_t bss_size;
    uint32_t stack_size;
    uint8_t  sym[MAX_SYM];
    uint32_t sym_size;
    uint32_t ram;           /* ram_base calculado na primeira passada */
} prog;

/* ---- Montagem --------------------------------------------------------- */

static void w(prog *p, uint32_t word)
{
    if (p->n < MAX_WORDS) {
        p->code[p->n] = word;
    }
    p->n++;
}

static uint32_t here(const prog *p)
{
    return p->n * 4u;
}

static void op_r(prog *p, uint8_t op, uint8_t a, uint8_t b, uint8_t c)
{
    w(p, ksvm_enc_r(op, a, b, c));
}

static void op_i(prog *p, uint8_t op, uint8_t a, uint8_t b, int32_t imm)
{
    w(p, ksvm_enc_i(op, a, b, imm));
}

static void op_l(prog *p, uint8_t op, uint8_t a, uint32_t imm32)
{
    w(p, ksvm_enc_l(op, a, 0u));
    w(p, imm32);
}

static void sys(prog *p, uint32_t num)
{
    op_i(p, KSVM_OP_SYSCALL, 0u, 0u, (int32_t)num);
}

/* Desvio para o endereço absoluto "target" (a partir da posição atual). */
static void branch(prog *p, uint8_t cond, uint32_t target)
{
    w(p, ksvm_enc_b(cond, (int32_t)(target - (here(p) + 4u))));
}

static void call(prog *p, uint32_t target)
{
    w(p, ksvm_enc_j(KSVM_OP_CALL, (int32_t)(target - (here(p) + 4u))));
}

/* Acrescenta bytes a .data e devolve o endereço no guest. */
static uint32_t data(prog *p, const char *bytes, uint32_t len)
{
    uint32_t addr = p->ram + p->data_size;

    if (p->data_size + len <= MAX_DATA) {
        memcpy(p->data + p->data_size, bytes, len);
    }
    p->data_size += len;
    return addr;
}

static void sym(prog *p, uint32_t addr, uint8_t kind, const char *name)
{
    uint32_t len = (uint32_t)strlen(name);

    if (p->sym_size + 6u + len > MAX_SYM) {
        return;
    }
    ksvm_st32le(p->sym + p->sym_size, addr);
    p->sym[p->sym_size + 4u] = kind;
    p->sym[p->sym_size + 5u] = (uint8_t)len;
    memcpy(p->sym + p->sym_size + 6u, name, len);
    p->sym_size += 6u + len;
}

/* ---- Programas -------------------------------------------------------- */

/* PUTS de .data, CALL de sub-rotina com PUTC, EXIT 3. Tabela de símbolos. */
static void build_hello(prog *p)
{
    uint32_t msg = data(p, "Hello, KSVM!\n", 13u), sub;

    sym(p, 0u, 0x80u, "_start");                     /* kind: convenção do ksas */
    op_l(p, KSVM_OP_MOVIL, 1u, msg);
    op_i(p, KSVM_OP_MOVI, 2u, 0u, 13);
    sys(p, KSVM_SYSCALL_PUTS);
    sub = here(p) + 12u;                              /* após CALL, MOVI, SYSCALL */
    call(p, sub);
    op_i(p, KSVM_OP_MOVI, 1u, 0u, 3);
    sys(p, KSVM_SYSCALL_EXIT);
    sym(p, here(p), 0x00u, "sub");                   /* local, código */
    op_i(p, KSVM_OP_MOVI, 1u, 0u, '!');
    sys(p, KSVM_SYSCALL_PUTC);
    op_i(p, KSVM_OP_MOVI, 1u, 0u, '\n');
    sys(p, KSVM_SYSCALL_PUTC);
    op_r(p, KSVM_OP_RET, 0u, 0u, 0u);
    p->bss_size = 16u;
    sym(p, msg, 0x81u, "msg");                       /* global, .data */
    sym(p, p->ram + p->data_size, 0x02u, "buf");     /* local, .bss */
}

/* Casos de borda do ksdis --raw (só o round-trip importa; executa CALL,
 * RET e HALT com r1 = 0): símbolo com nome de registrador em maiúsculas
 * ("R1": o ksas recusa como label), JMP para dentro da RAM e BAL para
 * code_size (o ksas exige alvo de desvio em [0, code_size)). */
static void build_rawedge(prog *p)
{
    (void)data(p, "\1\2\3\4\5\6\7\10", 8u);
    call(p, 12u);                                     /* 0 */
    op_i(p, KSVM_OP_MOVI, 1u, 0u, 0);                 /* 4 */
    op_r(p, KSVM_OP_HALT, 0u, 0u, 0u);                /* 8 */
    sym(p, here(p), 0x00u, "R1");
    op_r(p, KSVM_OP_RET, 0u, 0u, 0u);                 /* 12 */
    op_r(p, KSVM_OP_NOP, 0u, 0u, 0u);                 /* 16 */
    w(p, ksvm_enc_j(KSVM_OP_JMP, 12));               /* 20: alvo 36 = ram_base + 4 */
    op_r(p, KSVM_OP_NOP, 0u, 0u, 0u);                 /* 24 */
    w(p, ksvm_enc_b(KSVM_COND_AL, 0));               /* 28: alvo 32 = code_size */
}

/* r1 = 42, flags Z-C- por CMPI, BRK, HALT. */
static void build_halt42(prog *p)
{
    op_i(p, KSVM_OP_MOVI, 1u, 0u, 42);
    op_i(p, KSVM_OP_CMPI, 1u, 0u, 42);
    op_r(p, KSVM_OP_BRK, 0u, 0u, 0u);
    op_r(p, KSVM_OP_HALT, 0u, 0u, 0u);
}

/* LDW de 0x80000000: MEM_RANGE em pc = 8. */
static void build_fault(prog *p)
{
    op_l(p, KSVM_OP_MOVIL, 2u, UINT32_C(0x80000000));
    op_i(p, KSVM_OP_LDW, 1u, 2u, 0);
    op_r(p, KSVM_OP_HALT, 0u, 0u, 0u);
}

static void build_trap(prog *p)
{
    op_i(p, KSVM_OP_TRAP, 0u, 0u, 5);
}

static void build_loop(prog *p)
{
    branch(p, KSVM_COND_AL, 0u);
}

/* Copia stdin para stdout com GETC/PUTC; YIELD sem dado, EXIT 0 no EOF. */
static void build_echo(prog *p)
{
    uint32_t loop = here(p), nodata = loop + 20u, done = nodata + 16u;

    sys(p, KSVM_SYSCALL_GETC);                        /* loop */
    op_i(p, KSVM_OP_CMPI, 1u, 0u, -1);
    branch(p, KSVM_COND_EQ, nodata);
    sys(p, KSVM_SYSCALL_PUTC);
    branch(p, KSVM_COND_AL, loop);
    op_i(p, KSVM_OP_CMPI, 2u, 0u, KSVM_ERR_EOF);      /* nodata */
    branch(p, KSVM_COND_EQ, done);
    sys(p, KSVM_SYSCALL_YIELD);
    branch(p, KSVM_COND_AL, loop);
    op_i(p, KSVM_OP_MOVI, 1u, 0u, 0);                 /* done */
    sys(p, KSVM_SYSCALL_EXIT);
}

/* r1 == -1 desvia para fail (EXIT 10 + r2). */
static void check_ok(prog *p, uint32_t fail)
{
    op_i(p, KSVM_OP_CMPI, 1u, 0u, -1);
    branch(p, KSVM_COND_EQ, fail);
}

static void open_file(prog *p, uint32_t path, uint32_t len, uint32_t flags, uint32_t fail)
{
    op_l(p, KSVM_OP_MOVIL, 1u, path);
    op_i(p, KSVM_OP_MOVI, 2u, 0u, (int32_t)len);
    op_i(p, KSVM_OP_MOVI, 3u, 0u, (int32_t)flags);
    sys(p, KSVM_SYSCALL_OPEN);
    check_ok(p, fail);
    op_r(p, KSVM_OP_MOV, 10u, 1u, 0u);                /* r10 = fd */
}

/* fd r10, buffer em ptr, len -> syscall num (READ/WRITE). */
static void rw_file(prog *p, uint32_t num, uint32_t ptr, uint32_t len, uint32_t fail)
{
    op_r(p, KSVM_OP_MOV, 1u, 10u, 0u);
    op_l(p, KSVM_OP_MOVIL, 2u, ptr);
    op_i(p, KSVM_OP_MOVI, 3u, 0u, (int32_t)len);
    sys(p, num);
    check_ok(p, fail);
}

/* Escreve out.txt, relê a partir do byte 6 (SEEK), mostra com PUTS, e
 * confere que "../x" é recusado com INVAL. Erros: EXIT 10 + r2. */
static void build_files(prog *p, uint32_t fail)
{
    uint32_t path = data(p, "out.txt", 7u), msg = data(p, "hello file!\n", 12u);
    uint32_t bad = data(p, "../x", 4u), buf = p->ram + p->data_size;

    open_file(p, path, 7u, 6u, fail);                 /* WR | CREAT */
    rw_file(p, KSVM_SYSCALL_WRITE, msg, 12u, fail);
    op_r(p, KSVM_OP_MOV, 1u, 10u, 0u);
    sys(p, KSVM_SYSCALL_CLOSE);
    open_file(p, path, 7u, 1u, fail);                 /* RD */
    op_r(p, KSVM_OP_MOV, 1u, 10u, 0u);
    op_i(p, KSVM_OP_MOVI, 2u, 0u, 6);
    op_i(p, KSVM_OP_MOVI, 3u, 0u, 0);
    sys(p, KSVM_SYSCALL_SEEK);
    check_ok(p, fail);
    rw_file(p, KSVM_SYSCALL_READ, buf, 64u, fail);
    op_r(p, KSVM_OP_MOV, 2u, 1u, 0u);                 /* r2 = bytes lidos */
    op_l(p, KSVM_OP_MOVIL, 1u, buf);
    sys(p, KSVM_SYSCALL_PUTS);
    op_r(p, KSVM_OP_MOV, 1u, 10u, 0u);
    sys(p, KSVM_SYSCALL_CLOSE);
    op_l(p, KSVM_OP_MOVIL, 1u, bad);                  /* "../x": deve falhar */
    op_i(p, KSVM_OP_MOVI, 2u, 0u, 4);
    op_i(p, KSVM_OP_MOVI, 3u, 0u, 1);
    sys(p, KSVM_SYSCALL_OPEN);
    op_r(p, KSVM_OP_MOV, 1u, 2u, 0u);                 /* EXIT r2 (INVAL = 3) */
    sys(p, KSVM_SYSCALL_EXIT);
    p->bss_size = 64u;
}

/* Fim do programa "files": fail = EXIT 10 + r2. */
static void files_with_fail(prog *p, uint32_t fail_at)
{
    build_files(p, fail_at);
    op_i(p, KSVM_OP_ADDI, 1u, 2u, 10);                /* fail */
    sys(p, KSVM_SYSCALL_EXIT);
}

/* Três PUSH: com --stack 8 o terceiro dá STACK. */
static void build_stack(prog *p)
{
    op_r(p, KSVM_OP_PUSH, 1u, 0u, 0u);
    op_r(p, KSVM_OP_PUSH, 1u, 0u, 0u);
    op_r(p, KSVM_OP_PUSH, 1u, 0u, 0u);
    op_i(p, KSVM_OP_MOVI, 1u, 0u, 0);
    op_r(p, KSVM_OP_HALT, 0u, 0u, 0u);
}

/* ---- Seleção e escrita ------------------------------------------------ */

static bool build(prog *p, const char *name, uint32_t fail_at)
{
    if (strcmp(name, "hello") == 0)       { build_hello(p); }
    else if (strcmp(name, "halt42") == 0) { build_halt42(p); }
    else if (strcmp(name, "fault") == 0)  { build_fault(p); }
    else if (strcmp(name, "trap") == 0)   { build_trap(p); }
    else if (strcmp(name, "loop") == 0)   { build_loop(p); }
    else if (strcmp(name, "echo") == 0)   { build_echo(p); }
    else if (strcmp(name, "files") == 0)  { files_with_fail(p, fail_at); }
    else if (strcmp(name, "stack") == 0)  { build_stack(p); }
    else if (strcmp(name, "rawedge") == 0) { build_rawedge(p); }
    else { return false; }
    return true;
}

/* Duas passadas: a primeira mede código e acha "fail" (último par de
 * instruções de "files"); a segunda usa ram_base e fail definitivos. */
static bool assemble(prog *p, const char *name)
{
    uint32_t fail_at;

    memset(p, 0, sizeof *p);
    if (!build(p, name, 0u)) {
        return false;
    }
    fail_at = here(p) - 8u;
    p->ram = (here(p) + 15u) & ~UINT32_C(15);
    p->n = p->data_size = p->sym_size = 0u;
    (void)build(p, name, fail_at);
    p->stack_size = 256u;
    return p->n <= MAX_WORDS && p->data_size <= MAX_DATA;
}

static int write_image(const prog *p, const char *path)
{
    static uint8_t code[MAX_WORDS * 4u], out[8192];
    ksvm_ksx_desc d;
    uint32_t i, size = 0u;
    FILE *f;

    for (i = 0; i < p->n; i++) {
        ksvm_st32le(code + 4u * i, p->code[i]);
    }
    memset(&d, 0, sizeof d);
    d.code = code;
    d.code_size = here(p);
    d.data = p->data;
    d.data_size = p->data_size;
    d.bss_size = p->bss_size;
    d.stack_size = p->stack_size;
    d.sym = p->sym;
    d.sym_size = p->sym_size;
    if (ksvm_ksx_write(&d, out, (uint32_t)sizeof out, &size) != KSVM_OK) {
        fprintf(stderr, "mkimg: ksvm_ksx_write falhou\n");
        return 1;
    }
    f = fopen(path, "wb");
    if (f == NULL || fwrite(out, 1u, size, f) != size || fclose(f) != 0) {
        perror(path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static prog p;

    if (argc != 3 || !assemble(&p, argv[1])) {
        fprintf(stderr, "uso: mkimg {hello|halt42|fault|trap|loop|echo|files|stack|rawedge} saida.ksx\n");
        return 2;
    }
    return write_image(&p, argv[2]);
}
