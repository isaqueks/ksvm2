/*
 * genprogs.c — gera as imagens .ksx de teste das portas embarcadas.
 *
 * Roda no host de build. Enquanto o assembler (ksas) não é usado pelas
 * portas, os programas são montados aqui com os codificadores do núcleo
 * (ksvm_enc_*) e gravados com ksvm_ksx_write: a codificação é por
 * construção a mesma que a VM decodifica.
 *
 *   genprogs hello <saida.ksx> <mensagem>   PUTS(mensagem + "\n"); HALT 0
 *   genprogs blink <saida.ksx> <n>          pisca o pino 13 n vezes (100 ms)
 *                                           e confere o tempo; n = 0: sempre
 *   genprogs selftest <saida.ksx>           aritmética, memória, pilha,
 *                                           syscalls; esperado calculado
 *                                           aqui pela ALU do próprio núcleo
 *   genprogs fault <saida.ksx>              LDW fora do mapa (fault esperado)
 *
 * O selftest compara cada resultado com o valor que a ALU do host calcula:
 * se o binário AVR (int de 16 bits) ou ARM divergir do desktop, ele imprime
 * "selftest FAIL <n>".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ksvm/ksvm.h"

#define CODE_MAX  4096u
#define LABEL_MAX 32u
#define FIX_MAX   256u
#define RAM_BASE  UINT32_C(0x10000)

typedef enum fix_kind { FIX_J, FIX_B, FIX_ADR } fix_kind;

typedef struct fixup {
    uint32_t at;        /* offset da instrução */
    unsigned label;
    fix_kind kind;
    uint8_t  op;        /* J: opcode; B: condição; ADR: registrador */
} fixup;

typedef struct prog {
    uint8_t  code[CODE_MAX];
    uint32_t pc;
    long     label[LABEL_MAX];
    fixup    fix[FIX_MAX];
    unsigned nfix;
    uint8_t  data[64];
    uint32_t data_size;
    uint32_t bss_size;
    unsigned next_label;
} prog;

static void die(const char *msg)
{
    fprintf(stderr, "genprogs: %s\n", msg);
    exit(1);
}

static void emit(prog *p, uint32_t w)
{
    if (p->pc + 4u > CODE_MAX) {
        die("codigo grande demais");
    }
    ksvm_st32le(p->code + p->pc, w);
    p->pc += 4u;
}

static unsigned new_label(prog *p)
{
    if (p->next_label >= LABEL_MAX) {
        die("labels demais");
    }
    p->label[p->next_label] = -1;
    return p->next_label++;
}

static void bind(prog *p, unsigned l)
{
    p->label[l] = (long)p->pc;
}

static void add_fix(prog *p, unsigned l, fix_kind k, uint8_t op)
{
    if (p->nfix >= FIX_MAX) {
        die("fixups demais");
    }
    p->fix[p->nfix].at = p->pc;
    p->fix[p->nfix].label = l;
    p->fix[p->nfix].kind = k;
    p->fix[p->nfix].op = op;
    p->nfix++;
    emit(p, 0u);
}

/* ---- Instruções ------------------------------------------------------- */

static void r3(prog *p, uint8_t op, uint8_t rd, uint8_t a, uint8_t b) { emit(p, ksvm_enc_r(op, rd, a, b)); }
static void i3(prog *p, uint8_t op, uint8_t rd, uint8_t a, int32_t imm)
{
    if (!ksvm_imm14_fits(imm)) {
        die("imm14 fora da faixa");
    }
    emit(p, ksvm_enc_i(op, rd, a, imm));
}
static void l3(prog *p, uint8_t op, uint8_t rd, uint8_t a, uint32_t imm32)
{
    emit(p, ksvm_enc_l(op, rd, a));
    emit(p, imm32);
}
static void movi(prog *p, uint8_t rd, int32_t v)     { i3(p, KSVM_OP_MOVI, rd, 0, v); }
static void movl(prog *p, uint8_t rd, uint32_t v)    { l3(p, KSVM_OP_MOVIL, rd, 0, v); }
static void sys(prog *p, uint32_t num)               { emit(p, ksvm_enc_i(KSVM_OP_SYSCALL, 0, 0, (int32_t)num)); }
static void jmp(prog *p, unsigned l)                 { add_fix(p, l, FIX_J, KSVM_OP_JMP); }
static void call(prog *p, unsigned l)                { add_fix(p, l, FIX_J, KSVM_OP_CALL); }
static void bcond(prog *p, uint8_t cond, unsigned l) { add_fix(p, l, FIX_B, cond); }
static void adr(prog *p, uint8_t rd, unsigned l)     { add_fix(p, l, FIX_ADR, rd); }
static void halt(prog *p)                            { r3(p, KSVM_OP_HALT, 0, 0, 0); }

static void bytes(prog *p, const void *src, uint32_t n)
{
    if (p->pc + n + 3u > CODE_MAX) {
        die("codigo grande demais");
    }
    memcpy(p->code + p->pc, src, n);
    p->pc = (p->pc + n + 3u) & ~UINT32_C(3);
}

/* PUTS de uma string guardada no próprio código (ROM: leitura permitida). */
static void puts_label(prog *p, unsigned l, uint32_t len)
{
    adr(p, 1, l);
    movi(p, 2, (int32_t)len);
    sys(p, KSVM_SYSCALL_PUTS);
}

static void resolve(prog *p)
{
    unsigned i;

    for (i = 0; i < p->nfix; i++) {
        const fixup *f = &p->fix[i];
        int32_t off;
        uint32_t w;

        if (p->label[f->label] < 0) {
            die("label sem destino");
        }
        off = (int32_t)(p->label[f->label] - (long)(f->at + 4u));   /* PC_next */
        switch (f->kind) {
        case FIX_J:
            if (!ksvm_off24_fits(off)) die("desvio longe demais");
            w = ksvm_enc_j(f->op, off);
            break;
        case FIX_B:
            if (!ksvm_off20_fits(off)) die("desvio longe demais");
            w = ksvm_enc_b(f->op, off);
            break;
        default:
            if (!ksvm_imm14_fits(off)) die("ADR longe demais");
            w = ksvm_enc_i(KSVM_OP_ADR, f->op, 0, off);
            break;
        }
        ksvm_st32le(p->code + f->at, w);
    }
}

static void write_ksx(prog *p, const char *path)
{
    static uint8_t out[CODE_MAX + 256u];
    ksvm_ksx_desc d;
    uint32_t n = 0;
    FILE *f;

    resolve(p);
    memset(&d, 0, sizeof d);
    d.entry = 0;
    d.code = p->code;
    d.code_size = p->pc;
    d.data = p->data_size ? p->data : NULL;
    d.data_size = p->data_size;
    d.bss_size = p->bss_size;
    d.ram_base = RAM_BASE;
    d.stack_size = 128u;
    if (ksvm_ksx_write(&d, out, (uint32_t)sizeof out, &n) != KSVM_OK) {
        die("ksvm_ksx_write falhou");
    }
    f = fopen(path, "wb");
    if (f == NULL || fwrite(out, 1, n, f) != n || fclose(f) != 0) {
        die("nao consegui gravar a saida");
    }
}

/* ---- Programas -------------------------------------------------------- */

static void gen_hello(prog *p, const char *msg)
{
    unsigned l_msg = new_label(p);
    char text[128];
    size_t n = strlen(msg);

    if (n + 2u > sizeof text) {
        die("mensagem longa demais");
    }
    memcpy(text, msg, n);
    text[n++] = '\n';
    puts_label(p, l_msg, (uint32_t)n);
    movi(p, 1, 0);
    halt(p);
    bind(p, l_msg);
    bytes(p, text, (uint32_t)n);
}

#define LED_PIN   13
#define BLINK_MS  100

/* Pisca o LED; após cada escrita lê o pino de volta e imprime '0'/'1'.
 * No fim confere com MILLIS que passaram ao menos n * 100 ms. */
static void gen_blink(prog *p, int n)
{
    unsigned l_loop = new_label(p), l_bad = new_label(p), l_ok_msg = new_label(p);
    unsigned l_bad_msg = new_label(p), l_end = new_label(p);
    static const char ok_msg[] = " time ok\n";
    static const char bad_msg[] = " time BAD\n";

    movi(p, 1, LED_PIN); movi(p, 2, 1); sys(p, KSVM_SYSCALL_PIN_MODE);
    sys(p, KSVM_SYSCALL_MILLIS);
    r3(p, KSVM_OP_MOV, 20, 1, 0);                 /* r20 = t0 */
    movi(p, 16, 0);                               /* r16 = nível */
    movi(p, 17, n);                               /* r17 = contador */
    bind(p, l_loop);
    i3(p, KSVM_OP_XORI, 16, 16, 1);
    movi(p, 1, LED_PIN); r3(p, KSVM_OP_MOV, 2, 16, 0); sys(p, KSVM_SYSCALL_PIN_WRITE);
    movi(p, 1, LED_PIN); sys(p, KSVM_SYSCALL_PIN_READ);
    i3(p, KSVM_OP_ADDI, 1, 1, '0'); sys(p, KSVM_SYSCALL_PUTC);
    movi(p, 1, BLINK_MS); sys(p, KSVM_SYSCALL_SLEEP_MS);
    if (n == 0) {
        jmp(p, l_loop);                           /* para sempre */
    } else {
        i3(p, KSVM_OP_SUBI, 17, 17, 1);
        bcond(p, KSVM_COND_NE, l_loop);
    }
    sys(p, KSVM_SYSCALL_MILLIS);
    r3(p, KSVM_OP_SUB, 1, 1, 20);
    i3(p, KSVM_OP_CMPI, 1, 0, n * BLINK_MS);
    bcond(p, KSVM_COND_LO, l_bad);
    puts_label(p, l_ok_msg, (uint32_t)strlen(ok_msg));
    movi(p, 1, 0);
    jmp(p, l_end);
    bind(p, l_bad);
    puts_label(p, l_bad_msg, (uint32_t)strlen(bad_msg));
    movi(p, 1, 1);
    bind(p, l_end);
    halt(p);
    bind(p, l_ok_msg);  bytes(p, ok_msg, (uint32_t)strlen(ok_msg));
    bind(p, l_bad_msg); bytes(p, bad_msg, (uint32_t)strlen(bad_msg));
}

/* ---- selftest --------------------------------------------------------- */

typedef struct st_ctx {
    prog    *p;
    unsigned l_fail;
    int      id;
} st_ctx;

/* r10 = id; se r3 != esperado, desvia para a falha. */
static void expect(st_ctx *c, uint32_t expected)
{
    c->id++;
    movi(c->p, 10, c->id);
    l3(c->p, KSVM_OP_CMPL, 3, 0, expected);
    bcond(c->p, KSVM_COND_NE, c->l_fail);
}

/* r3 = op(a, b) no formato R, com a e b carregados por MOVIL. */
static void check_r(st_ctx *c, uint8_t op, uint32_t a, uint32_t b, uint32_t expected)
{
    movl(c->p, 4, a);
    movl(c->p, 5, b);
    r3(c->p, op, 3, 4, 5);
    expect(c, expected);
}

/* Mesmo xorshift32 da syscall RANDOM (syscall.c). */
static uint32_t xorshift(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static void st_alu(st_ctx *c)
{
    static const uint32_t v[][2] = {
        { 0x12345678u, 0x9ABCDEF0u }, { 0xFFFFFFFFu, 0xFFFFFFFFu },
        { 0x80000000u, 0xFFFFFFFFu }, { 0xFFFFFF9Cu, 7u },          /* -100, 7 */
        { 0x7FFFFFFFu, 0x00010001u }, { 1000000007u, 0xFFFFFFF9u }  /* .., -7 */
    };
    uint32_t flags = 0, q = 0;
    size_t i;

    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint32_t a = v[i][0], b = v[i][1];
        check_r(c, KSVM_OP_ADD, a, b, ksvm_alu_add(a, b, &flags));
        check_r(c, KSVM_OP_SUB, a, b, ksvm_alu_sub(a, b, &flags));
        check_r(c, KSVM_OP_MUL, a, b, ksvm_alu_mul(a, b));
        check_r(c, KSVM_OP_MULH, a, b, ksvm_alu_mulh(a, b));
        check_r(c, KSVM_OP_MULHU, a, b, ksvm_alu_mulhu(a, b));
        (void)ksvm_alu_div(a, b, &q);  check_r(c, KSVM_OP_DIV, a, b, q);
        (void)ksvm_alu_divu(a, b, &q); check_r(c, KSVM_OP_DIVU, a, b, q);
        (void)ksvm_alu_rem(a, b, &q);  check_r(c, KSVM_OP_REM, a, b, q);
        (void)ksvm_alu_remu(a, b, &q); check_r(c, KSVM_OP_REMU, a, b, q);
        check_r(c, KSVM_OP_SHL, a, b, ksvm_alu_shl(a, b, &flags));
        check_r(c, KSVM_OP_SAR, a, b, ksvm_alu_sar(a, b, &flags));
        check_r(c, KSVM_OP_ROR, a, b, ksvm_alu_ror(a, b, &flags));
    }
    movl(c->p, 4, 0x00F00000u);
    r3(c->p, KSVM_OP_CLZ, 3, 4, 0);    expect(c, ksvm_alu_clz(0x00F00000u));
    r3(c->p, KSVM_OP_CTZ, 3, 4, 0);    expect(c, ksvm_alu_ctz(0x00F00000u));
    r3(c->p, KSVM_OP_POPCNT, 3, 4, 0); expect(c, 4u);
    /* Carry: 0xFFFFFFFF + 1 liga C; ADC 0 + 0 + C = 1. */
    movl(c->p, 4, 0xFFFFFFFFu);
    i3(c->p, KSVM_OP_ADDI, 4, 4, 1);
    r3(c->p, KSVM_OP_ADC, 3, 0, 0);    expect(c, 1u);
    /* Imediato negativo estendido; constante de 32 bits (MOVIL). */
    i3(c->p, KSVM_OP_MOVI, 3, 0, -8192); expect(c, 0xFFFFE000u);
    movl(c->p, 3, 0x1234E000u);          expect(c, 0x1234E000u);
}

/* .data = 0xDEADBEEF, 0x80; .bss depois. */
static void st_memory(st_ctx *c)
{
    prog *p = c->p;

    movl(p, 6, RAM_BASE);
    i3(p, KSVM_OP_LDW, 3, 6, 0);       expect(c, 0xDEADBEEFu);
    i3(p, KSVM_OP_LDBS, 3, 6, 4);      expect(c, 0xFFFFFF80u);
    i3(p, KSVM_OP_LDH, 3, 6, 2);       expect(c, 0xDEADu);
    i3(p, KSVM_OP_LDHS, 3, 6, 2);      expect(c, 0xFFFFDEADu);
    i3(p, KSVM_OP_LDW, 3, 6, 8);       expect(c, 0u);           /* .bss zerado */
    movl(p, 4, 0x11223344u);
    i3(p, KSVM_OP_STW, 4, 6, 8);
    i3(p, KSVM_OP_LDB, 3, 6, 8);       expect(c, 0x44u);        /* little-endian */
    i3(p, KSVM_OP_LDB, 3, 6, 11);      expect(c, 0x11u);
    movi(p, 4, 0x55);
    movi(p, 7, 9);
    r3(p, KSVM_OP_STBX, 4, 6, 7);
    i3(p, KSVM_OP_LDW, 3, 6, 8);       expect(c, 0x11225544u);
}

/* CALL/RET, PUSH/POP e syscalls determinísticas. */
static void st_flow(st_ctx *c, unsigned l_double)
{
    prog *p = c->p;

    movi(p, 3, 21);
    movl(p, 11, 0xCAFEBABEu);
    r3(p, KSVM_OP_PUSH, 11, 0, 0);
    call(p, l_double);
    r3(p, KSVM_OP_POP, 12, 0, 0);
    expect(c, 42u);
    r3(p, KSVM_OP_MOV, 3, 12, 0);      expect(c, 0xCAFEBABEu);
    sys(p, KSVM_SYSCALL_VERSION);
    r3(p, KSVM_OP_MOV, 3, 1, 0);       expect(c, 0x020000u);
    sys(p, KSVM_SYSCALL_RANDOM);
    r3(p, KSVM_OP_MOV, 3, 1, 0);       expect(c, xorshift(KSVM_RNG_SEED));
    sys(p, 0x3FF0u);                   /* syscall do usuário inexistente */
    r3(p, KSVM_OP_MOV, 3, 2, 0);       expect(c, KSVM_ERR_NOSYS);
}

static void gen_selftest(prog *p)
{
    st_ctx c;
    unsigned l_double, l_ok, l_fail_msg, l_digits;
    static const char ok_msg[] = "selftest ok\n";
    static const char fail_msg[] = "selftest FAIL ";
    static const uint8_t data[8] = { 0xEF, 0xBE, 0xAD, 0xDE, 0x80, 0, 0, 0 };

    memcpy(p->data, data, sizeof data);
    p->data_size = sizeof data;
    p->bss_size = 8u;
    c.p = p;
    c.id = 0;
    c.l_fail = new_label(p);
    l_double = new_label(p);
    l_ok = new_label(p);
    l_fail_msg = new_label(p);
    l_digits = new_label(p);

    st_alu(&c);
    st_memory(&c);
    st_flow(&c, l_double);
    puts_label(p, l_ok, (uint32_t)strlen(ok_msg));
    movi(p, 1, 0);
    halt(p);

    /* Falha: "selftest FAIL <id>\n" e HALT id. Imprime até 3 dígitos. */
    bind(p, c.l_fail);
    puts_label(p, l_fail_msg, (uint32_t)strlen(fail_msg));
    movi(p, 13, 100);
    bind(p, l_digits);
    r3(p, KSVM_OP_DIVU, 1, 10, 13);
    i3(p, KSVM_OP_ADDI, 1, 1, '0');
    sys(p, KSVM_SYSCALL_PUTC);
    r3(p, KSVM_OP_REMU, 10, 10, 13);
    movi(p, 14, 10);
    r3(p, KSVM_OP_DIVU, 13, 13, 14);
    i3(p, KSVM_OP_CMPI, 13, 0, 0);
    bcond(p, KSVM_COND_NE, l_digits);
    movi(p, 1, '\n');
    sys(p, KSVM_SYSCALL_PUTC);
    movi(p, 1, c.id);                 /* não-zero */
    halt(p);

    bind(p, l_double);                 /* r3 *= 2, usando a pilha */
    r3(p, KSVM_OP_PUSH, 4, 0, 0);
    r3(p, KSVM_OP_ADD, 4, 3, 3);
    r3(p, KSVM_OP_MOV, 3, 4, 0);
    r3(p, KSVM_OP_POP, 4, 0, 0);
    r3(p, KSVM_OP_RET, 0, 0, 0);

    bind(p, l_ok);       bytes(p, ok_msg, (uint32_t)strlen(ok_msg));
    bind(p, l_fail_msg); bytes(p, fail_msg, (uint32_t)strlen(fail_msg));
}

static void gen_fault(prog *p)
{
    movl(p, 1, 0x7FFFFFF0u);
    i3(p, KSVM_OP_LDW, 2, 1, 0);      /* pc = 8: MEM_RANGE em 0x7FFFFFF0 */
    halt(p);
}

int main(int argc, char **argv)
{
    static prog p;

    if (argc < 3) {
        die("uso: genprogs hello|blink|selftest|fault saida.ksx [arg]");
    }
    if (strcmp(argv[1], "hello") == 0 && argc == 4) {
        gen_hello(&p, argv[3]);
    } else if (strcmp(argv[1], "blink") == 0 && argc == 4) {
        gen_blink(&p, atoi(argv[3]));
    } else if (strcmp(argv[1], "selftest") == 0) {
        gen_selftest(&p);
    } else if (strcmp(argv[1], "fault") == 0) {
        gen_fault(&p);
    } else {
        die("programa desconhecido ou argumentos errados");
    }
    write_ksx(&p, argv[2]);
    return 0;
}
