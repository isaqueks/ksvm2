/*
 * fuzz_exec.c — fuzzer de execução da KSVM (doc 09 §2, "Fuzz").
 *
 *   fuzz_exec [sementes] [primeira_semente] [max_passos]
 *
 * Para cada semente, um xorshift32 preenche o CÓDIGO (ROM de acesso direto
 * em 0), a RAM, uma ROM "flash" lida por callback (Harvard, executável),
 * uma janela MMIO e os registradores; a VM roda com hal_null até
 * max_passos (padrão 100000) instruções. Resultados aceitáveis: RUNNING
 * (limite), HALTED, FAULT, YIELD (retomado). Qualquer outra coisa — ou um
 * crash/ASan/UBSan — é bug.
 *
 * Além do resultado, cada semente confere invariantes:
 *   - r0 == 0, bits reservados de FLAGS == 0, fault coerente com status;
 *   - a ROM (código e flash) nunca é escrita pelo núcleo;
 *   - REPLAY: a semente é executada de novo do zero, agora com ksvm_step
 *     um a um (a primeira usa ksvm_run em fatias de tamanho aleatório);
 *     o estado final (cpu, RAM, MMIO, saída da HAL) tem de ser idêntico;
 *   - ATOMICIDADE: no replay, a instrução que entra em fault é executada a
 *     partir de um snapshot completo e não pode ter mudado nada além de
 *     status/fault/fault_addr; depois, ksvm_step não avança.
 *   - LIMITES DO FETCH (doc 04 §5.6): no replay, toda instrução concluída
 *     estava inteira numa única região EXEC e PC_next não deu a volta. A
 *     flash executável fica colada ao fim do código, então uma instrução L
 *     na última palavra do código tem o imm32 na região vizinha e precisa
 *     falhar com PC_RANGE em vez de ler de lá.
 *   - Com KSVM_ENABLE_TRACE, um hook confere r0/FLAGS a cada instrução.
 *   - EXTENSÃO F: no replay, depois de cada instrução F, todo NaN produzido
 *     é o canônico 0x7FC00000 e FCMP deixou uma das quatro combinações de
 *     flags (N, ZC, C, CV). Registradores e imm32 recebem às vezes padrões
 *     float especiais (zeros, inf, NaN, subnormais, extremos).
 *
 * -DKSVM_FUZZ_STRUCTURED=1: em vez de bytes aleatórios, gera instruções
 * VÁLIDAS (opcode da tabela da ISA + campos aleatórios com viés: bases de
 * memória em registradores que apontam para regiões mapeadas, alvos de
 * salto alinhados dentro do código, desvios curtos, syscalls conhecidas)
 * para exercitar o caminho feliz com profundidade.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ksvm/hal_null.h"
#include "ksvm/ksvm.h"

#ifndef KSVM_FUZZ_STRUCTURED
#define KSVM_FUZZ_STRUCTURED 0
#endif

/* Execuções por semente: bytes aleatórios quase sempre morrem nas primeiras
 * instruções, então cada semente é executada a partir de 64 entradas. */
#if KSVM_FUZZ_STRUCTURED
#define SUBRUNS 1u
#else
#define SUBRUNS 64u
#endif

#define CODE_SIZE   4096u
#define RAM_BASE    UINT32_C(0x00010000)
#define RAM_SIZE    4096u
#define RAM_END     (RAM_BASE + RAM_SIZE)
#define MMIO_BASE   UINT32_C(0x00020000)
#define MMIO_SIZE   256u
#define FLASH_BASE  CODE_SIZE              /* colada ao código: ver LIMITES DO FETCH */
#define FLASH_SIZE  1024u
#define INPUT_SIZE  64u

/* Com 3 regiões (config "alt") não há flash; com 2, nem MMIO. */
#define HAVE_FLASH (KSVM_MAX_REGIONS >= 4)
#define HAVE_MMIO  (KSVM_MAX_REGIONS >= 3)

typedef struct world {
    ksvm          vm;
    ksvm_hal      hal;
    ksvm_hal_null hn;
    uint8_t       code[CODE_SIZE];
    uint8_t       code_copy[CODE_SIZE];
    uint8_t       ram[RAM_SIZE];
    uint8_t       mmio[MMIO_SIZE];
    uint8_t       flash[FLASH_SIZE];
    uint8_t       flash_copy[FLASH_SIZE];
    uint8_t       input[INPUT_SIZE];
} world;

static unsigned long g_errors;
static uint32_t g_seed;

static void fail(const char *msg)
{
    g_errors++;
    printf("ERRO semente %lu: %s\n", (unsigned long)g_seed, msg);
}

/* ---- Gerador ---------------------------------------------------------- */

static uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static uint32_t below(uint32_t *s, uint32_t n)
{
    return rnd(s) % n;
}

/* Padrões binary32 que exercitam os casos especiais da extensão F (zeros,
 * infinitos, NaNs com payload, subnormais, extremos, empates de
 * conversão), usados nos registradores iniciais e nos imm32 do MOVIL. */
static const uint32_t g_float_specials[] = {
    0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u,
    0xFFC12345u, 0x00000001u, 0x807FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0xFF7FFFFFu,
    0x3F800000u, 0xBF800000u, 0x3F000000u, 0x40200000u, 0x4B800001u, 0x4F000000u,
    0xCF000000u, 0x4F800000u, 0x3EAAAAABu, 0x34000000u,
};

static uint32_t float_special(uint32_t *s)
{
    return g_float_specials[below(s, (uint32_t)(sizeof g_float_specials / sizeof g_float_specials[0]))];
}

/* Oráculo da extensão F, independente de softfloat.c, conferido no replay
 * depois de cada instrução F concluída: todo NaN que uma operação produz é
 * o canônico (FNEG/FABS só mexem no sinal e FTOI/FTOU dão inteiros, então
 * ficam de fora), e FCMP deixa uma das quatro combinações de flags. */
static void check_float_result(const ksvm *vm, uint32_t w)
{
#if KSVM_ENABLE_FLOAT
    uint8_t op = ksvm_field_op(w);
    uint32_t v = vm->cpu.r[ksvm_field_ra(w)], f = vm->cpu.flags;

    if (op == KSVM_OP_FCMP) {
        if (f != KSVM_FLAG_N && f != (KSVM_FLAG_Z | KSVM_FLAG_C) && f != KSVM_FLAG_C &&
            f != (KSVM_FLAG_C | KSVM_FLAG_V)) {
            fail("FCMP deixou uma combinacao de flags invalida");
        }
        return;
    }
    if (op < KSVM_OP_FADD || op > KSVM_OP_UTOF || op == KSVM_OP_FNEG || op == KSVM_OP_FABS ||
        op == KSVM_OP_FTOI || op == KSVM_OP_FTOU) {
        return;
    }
    if ((v & 0x7FFFFFFFu) > 0x7F800000u && v != 0x7FC00000u) {
        fail("extensao F produziu NaN nao canonico");
    }
#else
    (void)vm;
    (void)w;
#endif
}

/* ---- Regiões ---------------------------------------------------------- */

#if HAVE_MMIO
static uint8_t mmio_read(ksvm_region *r, uint32_t off)
{
    return ((const uint8_t *)r->backing)[off];
}

static void mmio_write(ksvm_region *r, uint32_t off, uint8_t v)
{
    ((uint8_t *)r->backing)[off] = (uint8_t)(v ^ 0x5Au);   /* registrador "vivo" */
}
#endif

#if HAVE_FLASH
static uint8_t flash_read(ksvm_region *r, uint32_t off)
{
    return ((const uint8_t *)r->backing)[off];
}
#endif

#if KSVM_ENABLE_TRACE
static unsigned long g_trace_calls;
static void trace_hook(void *ctx, const struct ksvm *vm, ksvm_trace_event ev,
                       uint32_t pc, uint32_t word)
{
    (void)ctx;
    (void)pc;
    (void)word;
    g_trace_calls++;
    if (ev == KSVM_TRACE_STEP && vm->cpu.r[0] != 0u) {
        fail("trace: r0 != 0 apos uma instrucao");
    }
    if ((vm->cpu.flags & ~KSVM_FLAGS_MASK) != 0u) {
        fail("trace: bits reservados de FLAGS");
    }
}
#endif

/* ---- Geração estruturada ---------------------------------------------- */

#if KSVM_FUZZ_STRUCTURED
static unsigned long g_float_insns;      /* instruções F geradas */
static uint8_t g_ops[256];
static unsigned g_nops;

static void collect_ops(void)
{
    unsigned op;
    for (op = 0; op < 256u; op++) {
        if (ksvm_isa[op].format == KSVM_FMT_INVALID) continue;
        if (op == KSVM_OP_PUSHM || op == KSVM_OP_POPM) continue;   /* não implementados */
        g_ops[g_nops++] = (uint8_t)op;
    }
}

/* Registradores com papel no gerador:
 *   r1..r15  destino/fonte geral (aritmética reescreve à vontade)
 *   r16..r23 bases de memória (RAM, MMIO, flash, código) — nunca destino
 *   r24..r27 alvos de salto alinhados dentro do código — nunca destino
 *   sp       pilha */
static uint8_t gen_rd(uint32_t *s)  { return (uint8_t)below(s, 16u); }
static uint8_t gen_src(uint32_t *s) { return (uint8_t)(below(s, 8u) == 0u ? below(s, 32u) : below(s, 16u)); }
static uint8_t gen_base(uint32_t *s){ return (uint8_t)(16u + below(s, 8u)); }

static int32_t gen_small(uint32_t *s, int32_t span)
{
    return (int32_t)below(s, (uint32_t)(2 * span + 1)) - span;
}

static uint32_t gen_imm32(uint32_t *s)
{
    switch (below(s, 5u)) {
    case 0:  return RAM_BASE + below(s, RAM_SIZE);
    case 1:  return below(s, 64u);
    case 2:  return 0u - below(s, 64u);
    case 3:  return float_special(s);       /* fli */
    default: return rnd(s);
    }
}

static const uint32_t g_sys[] = {
    KSVM_SYSCALL_CAPS, KSVM_SYSCALL_VERSION, KSVM_SYSCALL_YIELD,
    KSVM_SYSCALL_PUTC, KSVM_SYSCALL_GETC, KSVM_SYSCALL_PUTS, KSVM_SYSCALL_GETS,
    KSVM_SYSCALL_MILLIS, KSVM_SYSCALL_SLEEP_MS, KSVM_SYSCALL_MICROS,
    KSVM_SYSCALL_OPEN, KSVM_SYSCALL_PIN_READ, KSVM_SYSCALL_MEM_INFO, KSVM_SYSCALL_RANDOM,
    KSVM_SYSCALL_USER_BASE, 0x3FFFu, KSVM_SYSCALL_EXIT, KSVM_SYSCALL_ABORT,
};

/* Uma instrução válida em w[i] (e w[i+1] no formato L). Devolve palavras usadas. */
static unsigned gen_insn(uint32_t *s, uint32_t *w, unsigned i, unsigned nwords)
{
    uint8_t op;
    const ksvm_isa_info *info;
    int32_t imm = 0;

    for (;;) {
        op = g_ops[below(s, g_nops)];
        /* terminais e RET raros, para os programas irem longe */
        if ((op == KSVM_OP_HALT || op == KSVM_OP_TRAP || op == KSVM_OP_RET) && below(s, 16u) != 0u) {
            continue;
        }
        if (ksvm_isa[op].len == 8u && i + 1u >= nwords) continue;
        break;
    }
    info = &ksvm_isa[op];
    if (op >= KSVM_OP_FADD && op <= KSVM_OP_FCMP) {
        g_float_insns++;
    }

    switch (op) {
    case KSVM_OP_MFSR: case KSVM_OP_MTSR:
        imm = below(s, 8u) == 0u ? gen_small(s, 8) : (int32_t)below(s, 4u);
        w[i] = ksvm_enc_i(op, gen_rd(s), gen_src(s), imm);
        return 1u;
    case KSVM_OP_SYSCALL: {
        uint32_t n = g_sys[below(s, (uint32_t)(sizeof g_sys / sizeof g_sys[0]))];
        if ((n == KSVM_SYSCALL_EXIT || n == KSVM_SYSCALL_ABORT) && below(s, 8u) != 0u) {
            n = KSVM_SYSCALL_RANDOM;
        }
        w[i] = (uint32_t)op | (n << 18);
        return 1u;
    }
    case KSVM_OP_JMPR: case KSVM_OP_CALLR:
        w[i] = ksvm_enc_r(op, 0, (uint8_t)(below(s, 8u) == 0u ? gen_src(s) : 24u + below(s, 4u)), 0);
        return 1u;
    case KSVM_OP_PUSH: case KSVM_OP_POP:
        w[i] = ksvm_enc_r(op, op == KSVM_OP_POP ? gen_rd(s) : gen_src(s), 0, 0);
        return 1u;
    default:
        break;
    }
    if (op >= KSVM_OP_LDB && op <= KSVM_OP_STWX) {
        int is_store = (op >= KSVM_OP_STB && op <= KSVM_OP_STW) || op >= KSVM_OP_STBX;
        uint8_t rd = is_store ? gen_src(s) : gen_rd(s);   /* store: fonte qualquer */
        /* stores quase sempre em RAM/MMIO (r16, r18, r20, r22); às vezes em ROM */
        uint8_t base = (is_store && below(s, 16u) != 0u) ? (uint8_t)(16u + 2u * below(s, 4u)) : gen_base(s);
        if (info->format == KSVM_FMT_I) {
            w[i] = ksvm_enc_i(op, rd, base, gen_small(s, 40));
        } else {
            /* índice: r0 ou um registrador geral (pode estar "sujo") */
            w[i] = ksvm_enc_r(op, rd, base, (uint8_t)(below(s, 8u) != 0u ? 0u : gen_src(s)));
        }
        return 1u;
    }
    switch (info->format) {
    case KSVM_FMT_R:
        w[i] = ksvm_enc_r(op, gen_rd(s), gen_src(s), gen_src(s));
        return 1u;
    case KSVM_FMT_I:
        imm = below(s, 4u) == 0u ? (int32_t)ksvm_sext14(rnd(s)) : gen_small(s, 40);
        w[i] = ksvm_enc_i(op, gen_rd(s), gen_src(s), imm);
        return 1u;
    case KSVM_FMT_L:
        w[i] = ksvm_enc_l(op, gen_rd(s), gen_src(s));
        w[i + 1u] = gen_imm32(s);
        return 2u;
    case KSVM_FMT_B:   /* em geral curto; às vezes qualquer lugar do alcance */
        w[i] = ksvm_enc_b((uint8_t)below(s, 16u),
                          below(s, 128u) == 0u ? ksvm_u32_to_i32(rnd(s) & ~3u) % 0x200000
                                              : gen_small(s, 24) * 4);
        return 1u;
    default:           /* J: JMP, CALL */
        w[i] = ksvm_enc_j(op, below(s, 128u) == 0u ? ksvm_u32_to_i32(rnd(s) & ~3u) % 0x2000000
                                                  : gen_small(s, 24) * 4);
        return 1u;
    }
}
#endif

/* ---- Montagem do mundo ------------------------------------------------ */

/* "sub" (0..SUBRUNS-1) mantém a imagem da semente e varia entrada e registradores. */
static void build_world(world *wd, uint32_t seed, uint32_t sub)
{
    uint32_t s = seed * UINT32_C(0x9E3779B9) + UINT32_C(0x7F4A7C15);
    unsigned i;

    if (s == 0u) s = 1u;
    memset(wd, 0, sizeof *wd);

#if KSVM_FUZZ_STRUCTURED
    {
        uint32_t words[CODE_SIZE / 4u];
        unsigned n = CODE_SIZE / 4u;
        i = 0;
        while (i < n) i += gen_insn(&s, words, i, n);
        for (i = 0; i < n; i++) ksvm_st32le(&wd->code[4u * i], words[i]);
    }
#else
    for (i = 0; i < CODE_SIZE; i++) wd->code[i] = (uint8_t)rnd(&s);
    /* às vezes um opcode válido em toda palavra, para ir mais longe */
    if (below(&s, 2u) == 0u) {
        for (i = 0; i < CODE_SIZE; i += 4u) {
            uint8_t op;
            do { op = (uint8_t)rnd(&s); } while (ksvm_isa[op].format == KSVM_FMT_INVALID);
            wd->code[i] = op;
        }
    }
#endif
    for (i = 0; i < RAM_SIZE; i++) wd->ram[i] = (uint8_t)rnd(&s);
    for (i = 0; i < MMIO_SIZE; i++) wd->mmio[i] = (uint8_t)rnd(&s);
    for (i = 0; i < FLASH_SIZE; i++) wd->flash[i] = (uint8_t)rnd(&s);
    for (i = 0; i < INPUT_SIZE; i++) wd->input[i] = (uint8_t)rnd(&s);
    memcpy(wd->code_copy, wd->code, CODE_SIZE);
    memcpy(wd->flash_copy, wd->flash, FLASH_SIZE);
    s ^= sub * UINT32_C(0x85EBCA6B);
    if (s == 0u) s = 1u;

    ksvm_init(&wd->vm, &wd->hal);
    ksvm_hal_null_init(&wd->hn, &wd->hal, &wd->vm);
    ksvm_hal_null_set_input(&wd->hn, wd->input, below(&s, INPUT_SIZE), below(&s, 2u) != 0u);
    if (ksvm_map_rom(&wd->vm, 0u, wd->code, CODE_SIZE, NULL) != KSVM_OK ||
        ksvm_map_ram(&wd->vm, RAM_BASE, wd->ram, RAM_SIZE) != KSVM_OK) {
        fail("mapeamento");
    }
#if HAVE_MMIO
    if (ksvm_map_mmio(&wd->vm, MMIO_BASE, MMIO_SIZE, wd->mmio, mmio_read, mmio_write) != KSVM_OK) {
        fail("mapeamento MMIO");
    }
#endif
#if HAVE_FLASH
    if (ksvm_map_rom(&wd->vm, FLASH_BASE, wd->flash, FLASH_SIZE, flash_read) != KSVM_OK) {
        fail("mapeamento flash");
    }
#endif
    wd->vm.stack_limit = RAM_BASE + below(&s, RAM_SIZE / 2u);
    wd->vm.heap_start = RAM_BASE;
    wd->vm.rng_state = rnd(&s) | 1u;
    ksvm_reset(&wd->vm, (sub != 0u || below(&s, 4u) == 0u) ? 4u * below(&s, CODE_SIZE / 4u) : 0u,
               RAM_END);
#if KSVM_ENABLE_TRACE
    ksvm_set_trace(&wd->vm, trace_hook, NULL);
#endif

    for (i = 1; i < 31u; i++) {
        uint32_t v;
        switch (below(&s, 7u)) {
        case 0:  v = RAM_BASE + below(&s, RAM_SIZE); break;
        case 1:  v = below(&s, 256u); break;
        case 2:  v = 4u * below(&s, CODE_SIZE / 4u); break;
        case 3:  v = MMIO_BASE + below(&s, MMIO_SIZE); break;
        case 4:  v = float_special(&s); break;     /* operandos da extensão F */
        default: v = rnd(&s); break;
        }
        wd->vm.cpu.r[i] = v;
    }
#if KSVM_FUZZ_STRUCTURED
    /* bases de memória e alvos de salto (ver gen_insn) */
    for (i = 16; i < 24u; i++) {
        switch (i % 4u) {
        case 0:  wd->vm.cpu.r[i] = RAM_BASE + 64u + below(&s, RAM_SIZE - 128u); break;
        case 1:  wd->vm.cpu.r[i] = HAVE_FLASH ? FLASH_BASE + 64u + below(&s, FLASH_SIZE - 128u)
                                              : RAM_BASE + 64u + below(&s, RAM_SIZE - 128u); break;
        case 2:  wd->vm.cpu.r[i] = MMIO_BASE + 48u + below(&s, MMIO_SIZE - 96u); break;
        default: wd->vm.cpu.r[i] = 64u + below(&s, CODE_SIZE - 128u); break;
        }
    }
    for (i = 24; i < 28u; i++) wd->vm.cpu.r[i] = 4u * below(&s, CODE_SIZE / 4u);
#endif
    wd->vm.cpu.flags = below(&s, 16u);
}

/* ---- Execução --------------------------------------------------------- */

typedef struct outcome {
    ksvm_status status;
    ksvm_fault  fault;
    uint32_t    fault_addr;
    uint32_t    exit_code;
    uint32_t    insns;      /* instruções concluídas */
    unsigned    yields;
} outcome;

static int stopped(ksvm_status st)
{
    return st == KSVM_HALTED || st == KSVM_FAULT;
}

/* Execução 1: ksvm_run em fatias aleatórias, retomando YIELD. */
static outcome run_slices(world *wd, uint32_t max_insns, uint32_t seed)
{
    outcome o;
    uint32_t s = seed ^ UINT32_C(0xA5A5A5A5);

    memset(&o, 0, sizeof o);
    if (s == 0u) s = 1u;
    while (!stopped(wd->vm.status) && wd->vm.insn_count < max_insns) {
        uint32_t slice = 1u + below(&s, 5000u);
        ksvm_status st;
        if (slice > max_insns - wd->vm.insn_count) slice = max_insns - wd->vm.insn_count;
        st = ksvm_run(&wd->vm, slice);
        if (st == KSVM_YIELD) o.yields++;
        if (st != KSVM_RUNNING && st != KSVM_YIELD && st != KSVM_HALTED && st != KSVM_FAULT) {
            fail("status fora do enum");
            break;
        }
    }
    o.status = wd->vm.status;
    o.fault = wd->vm.fault;
    o.fault_addr = wd->vm.fault_addr;
    o.exit_code = wd->vm.exit_code;
    o.insns = wd->vm.insn_count;
    return o;
}

static void check_invariants(const world *wd, const outcome *o, uint32_t max_insns)
{
    if (wd->vm.cpu.r[0] != 0u) fail("r0 != 0");
    if ((wd->vm.cpu.flags & ~KSVM_FLAGS_MASK) != 0u) fail("bits reservados de FLAGS");
    if (memcmp(wd->code, wd->code_copy, CODE_SIZE) != 0) fail("ROM de codigo foi escrita");
    if (memcmp(wd->flash, wd->flash_copy, FLASH_SIZE) != 0) fail("flash foi escrita");
    if (o->status == KSVM_FAULT) {
        if (o->fault == KSVM_FAULT_NONE || o->fault >= KSVM_FAULT_COUNT) fail("fault invalido");
    } else if (o->fault != KSVM_FAULT_NONE) {
        fail("fault != NONE sem status FAULT");
    }
    if ((o->status == KSVM_RUNNING || o->status == KSVM_YIELD) && o->insns != max_insns) {
        fail("parou antes do limite sem HALT/FAULT");
    }
    if (o->insns > max_insns) fail("passou do limite de instrucoes");
    if (o->status == KSVM_HALTED && !wd->hn.halted) fail("HALTED sem on_halt");
}

/* Oráculo dos limites do fetch, independente de exec.c: a instrução em pc
 * (que acabou de ser concluída) cabia inteira numa região EXEC e
 * pc + len não passou de 0xFFFFFFFF. */
static void check_fetch_bounds(world *wd, uint32_t pc)
{
    uint32_t w = 0u;
    uint32_t len;
    const ksvm_region *r;

    if (ksvm_bus_fetch32(&wd->vm.bus, pc, &w) != KSVM_FAULT_NONE) {
        fail("instrucao concluida sem palavra executavel em pc");
        return;
    }
    len = ksvm_isa_len(ksvm_field_op(w));
    r = ksvm_bus_find(&wd->vm.bus, pc, len);
    if (r == NULL || (r->perm & KSVM_MEM_EXEC) == 0u || pc + len < pc) {
        fail("instrucao concluida fora dos limites do fetch");
    }
}

/* Execução 2 (replay): ksvm_step um a um; antes do passo que falha,
 * snapshot completo para conferir a atomicidade. */
static void replay(world *wd, const outcome *o)
{
    static world before;
    uint32_t i;

    for (i = 0; i < o->insns; i++) {
        uint32_t pc = wd->vm.cpu.pc, w = 0u;
        ksvm_status st;
        (void)ksvm_bus_fetch32(&wd->vm.bus, pc, &w);
        st = ksvm_step(&wd->vm);
        check_fetch_bounds(wd, pc);
        if (st != KSVM_FAULT) {
            check_float_result(&wd->vm, w);
        }
        if (st == KSVM_FAULT || (st == KSVM_HALTED && i + 1u != o->insns)) {
            fail("replay divergiu (parou antes)");
            return;
        }
    }
    if (o->status != KSVM_FAULT) return;

    memcpy(&before, wd, sizeof before);
    (void)ksvm_step(&wd->vm);
    if (wd->vm.status != KSVM_FAULT || wd->vm.fault != o->fault ||
        wd->vm.fault_addr != o->fault_addr) {
        fail("replay: fault diferente");
        return;
    }
    if (memcmp(&before.vm.cpu, &wd->vm.cpu, sizeof wd->vm.cpu) != 0) fail("fault alterou a cpu");
    if (memcmp(before.ram, wd->ram, RAM_SIZE) != 0) fail("fault alterou a RAM");
    if (memcmp(before.mmio, wd->mmio, MMIO_SIZE) != 0) fail("fault alterou o MMIO");
    if (memcmp(&before.hn.out, &wd->hn.out, sizeof wd->hn.out) != 0 ||
        before.hn.out_total != wd->hn.out_total || before.hn.in_pos != wd->hn.in_pos) {
        fail("fault teve efeito na HAL");
    }
    if (before.vm.insn_count != wd->vm.insn_count) fail("fault contou instrucao");
    if (before.vm.rng_state != wd->vm.rng_state) fail("fault alterou rng_state");
    /* congelada */
    memcpy(&before, wd, sizeof before);
    (void)ksvm_step(&wd->vm);
    (void)ksvm_run(&wd->vm, 10u);
    if (memcmp(&before.vm, &wd->vm, sizeof wd->vm) != 0) fail("VM em fault avancou");
}

int main(int argc, char **argv)
{
    static world w1, w2;
    unsigned long nseeds = (argc > 1) ? strtoul(argv[1], NULL, 0) : 100ul;
    unsigned long first = (argc > 2) ? strtoul(argv[2], NULL, 0) : 1ul;
    uint32_t max_insns = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 100000u;
    unsigned long n, runs = 0, limit = 0, halted = 0, yields = 0;
    unsigned long faults[KSVM_FAULT_COUNT];
    unsigned long long total = 0;
    unsigned f;

    memset(faults, 0, sizeof faults);
#if KSVM_FUZZ_STRUCTURED
    collect_ops();
#endif
    for (n = 0; n < nseeds && g_errors <= 20u; n++) {
        uint32_t sub;
        g_seed = (uint32_t)(first + n);
        for (sub = 0; sub < SUBRUNS; sub++) {
            outcome o;
            build_world(&w1, g_seed, sub);
            o = run_slices(&w1, max_insns, g_seed + sub);
            check_invariants(&w1, &o, max_insns);

            build_world(&w2, g_seed, sub);
            replay(&w2, &o);
            if (memcmp(&w1.vm.cpu, &w2.vm.cpu, sizeof w1.vm.cpu) != 0 ||
                memcmp(w1.ram, w2.ram, RAM_SIZE) != 0 || memcmp(w1.mmio, w2.mmio, MMIO_SIZE) != 0 ||
                w1.vm.status != w2.vm.status || w1.vm.insn_count != w2.vm.insn_count ||
                w1.vm.exit_code != w2.vm.exit_code || w1.vm.rng_state != w2.vm.rng_state ||
                w1.hn.out_total != w2.hn.out_total ||
                memcmp(w1.hn.out, w2.hn.out, sizeof w1.hn.out) != 0) {
                fail("nao deterministico: run em fatias != step a step");
            }

            runs++;
            total += o.insns;
            yields += o.yields;
            if (o.status == KSVM_HALTED) halted++;
            else if (o.status == KSVM_FAULT) faults[o.fault]++;
            else limit++;
        }
    }

    printf("fuzz_exec (%s, TRACE=%d NO_INT64=%d MAX_REGIONS=%d): %lu sementes a partir de %lu, "
           "limite %lu passos\n",
           KSVM_FUZZ_STRUCTURED ? "estruturado" : "bytes aleatorios", KSVM_ENABLE_TRACE,
           KSVM_NO_INT64, KSVM_MAX_REGIONS, n, first, (unsigned long)max_insns);
    printf("  execucoes: %lu  instrucoes: %llu  limite: %lu  halted: %lu  yields: %lu\n",
           runs, total, limit, halted, yields);
    printf("  faults:");
    for (f = 1; f < KSVM_FAULT_COUNT; f++) {
        printf(" %s=%lu", ksvm_fault_name((ksvm_fault)f), faults[f]);
    }
    printf("\n");
#if KSVM_FUZZ_STRUCTURED
    printf("  instrucoes F geradas (extensao F %s): %lu\n",
           KSVM_ENABLE_FLOAT ? "ligada" : "desligada", g_float_insns);
#endif
#if KSVM_ENABLE_TRACE
    printf("  chamadas do trace: %lu\n", g_trace_calls);
#endif
    printf("  %s (%lu erros)\n", g_errors ? "FALHOU" : "ok", g_errors);
    return g_errors ? 1 : 0;
}
