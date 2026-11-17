/*
 * test_snapshot.c — determinismo e snapshot por memcpy (vm.h: "struct ksvm
 * não contém ponteiros para dentro de si mesma: pode ser copiada com
 * memcpy").
 *
 * Um programa que mistura RANDOM, MILLIS, PUTC, WFI, pilha e
 * leitura-modificação-escrita na RAM roda alguns passos; o estado (struct
 * ksvm + RAM + estado da hal_null) é copiado; executam-se 1000 passos;
 * restaura-se; executam-se 1000 passos de novo (inclusive em fatias
 * diferentes) e o estado final tem de ser idêntico byte a byte.
 */
#include "fixture.h"
#include "miniasm.h"
#include "test.h"

typedef struct snap {
    ksvm          vm;
    uint8_t       ram[FIX_RAM_SIZE];
    ksvm_hal_null hn;
} snap;

static void save(snap *s, const fixture *f)
{
    memcpy(&s->vm, &f->vm, sizeof s->vm);
    memcpy(s->ram, f->ram, sizeof s->ram);
    memcpy(&s->hn, &f->hn, sizeof s->hn);
}

static void restore(fixture *f, const snap *s)
{
    memcpy(&f->vm, &s->vm, sizeof f->vm);
    memcpy(f->ram, s->ram, sizeof f->ram);
    memcpy(&f->hn, &s->hn, sizeof f->hn);
}

/* n instruções com ksvm_step (retoma de YIELD sozinho). */
static void steps(fixture *f, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) (void)ksvm_step(&f->vm);
}

/* Idem em fatias com ksvm_run, retomando após YIELD. */
static void run_slices(fixture *f, unsigned n, unsigned slice)
{
    unsigned done = 0;
    while (done < n) {
        uint32_t before = f->vm.insn_count;
        unsigned want = (n - done < slice) ? n - done : slice;
        (void)ksvm_run(&f->vm, want);
        done += (unsigned)(f->vm.insn_count - before);
        if (f->vm.status == KSVM_HALTED || f->vm.status == KSVM_FAULT) break;
    }
}

static void build(ma *a)
{
    enum { LOOP = 1 };
    ma_init(a);
    ma_emit_l(a, ksvm_enc_l(KSVM_OP_MOVIL, 10, 0), FIX_RAM_BASE);
    ma_emit(a, OP_I(MOVI, 11, 0, 0));
    ma_label(a, LOOP);
    ma_emit(a, OP_I(SYSCALL, 0, 0, KSVM_SYSCALL_RANDOM));
    ma_emit(a, OP_I(ANDI, 2, 1, 0xFC));
    ma_emit(a, OP_R(LDWX, 3, 10, 2));
    ma_emit(a, OP_R(ADD, 3, 3, 1));
    ma_emit(a, OP_R(STWX, 3, 10, 2));
    ma_emit(a, OP_R(XOR, 11, 11, 3));
    ma_emit(a, OP_R(ROL, 11, 11, 1));
    ma_emit(a, OP_R(PUSH, 11, 0, 0));
    ma_emit(a, OP_R(POP, 12, 0, 0));
    ma_emit(a, OP_I(SYSCALL, 0, 0, KSVM_SYSCALL_MILLIS));
    ma_emit(a, OP_R(ADC, 11, 11, 1));
    ma_emit(a, OP_I(ANDI, 1, 11, 0x3F));
    ma_emit(a, OP_I(ADDI, 1, 1, '0'));
    ma_emit(a, OP_I(SYSCALL, 0, 0, KSVM_SYSCALL_PUTC));
    ma_emit(a, OP_R(WFI, 0, 0, 0));
    ma_b(a, KSVM_COND_AL, LOOP);
}

static void start(test_ctx *t_, fixture *f, const ma *a)
{
    ASSERT_TRUE(a->err == 0);
    fix_load(f, a->w, a->n);
}

TEST(snapshot_restore_replays_identically)
{
    static const unsigned warmups[] = { 0u, 1u, 37u, 500u };
    ma a;
    unsigned w;
    build(&a);
    ASSERT_EQ(ma_link(&a), 0);
    for (w = 0; w < sizeof warmups / sizeof warmups[0]; w++) {
        static fixture f;
        static snap s0, s1, s2;
        start(t_, &f, &a);
        if (TEST_FAILED()) return;
        steps(&f, warmups[w]);
        save(&s0, &f);

        steps(&f, 1000u);
        ASSERT_EQ_U32(f.vm.insn_count, warmups[w] + 1000u);   /* nunca parou */
        save(&s1, &f);

        restore(&f, &s0);
        steps(&f, 1000u);
        save(&s2, &f);
        ASSERT_MEM_EQ(&s1, &s2, sizeof s1);

        restore(&f, &s0);
        run_slices(&f, 1000u, 7u);                         /* fatias de 7 */
        save(&s2, &f);
        ASSERT_MEM_EQ(&s1, &s2, sizeof s1);

        restore(&f, &s0);
        run_slices(&f, 1000u, 1000u);
        save(&s2, &f);
        ASSERT_MEM_EQ(&s1, &s2, sizeof s1);
        /* o programa de fato fez coisas: RAM e saída mudaram */
        ASSERT_TRUE(memcmp(s0.ram, s1.ram, sizeof s0.ram) != 0);
        ASSERT_TRUE(s1.hn.out_total > s0.hn.out_total);
        ASSERT_TRUE(s1.vm.rng_state != s0.vm.rng_state);
    }
}

/* Duas instâncias independentes com o mesmo início chegam ao mesmo fim. */
TEST(two_fresh_instances_are_identical)
{
    static fixture f1, f2;
    static snap s1, s2;
    ma a;
    build(&a);
    ASSERT_EQ(ma_link(&a), 0);
    start(t_, &f1, &a);
    start(t_, &f2, &a);
    steps(&f1, 3000u);
    run_slices(&f2, 3000u, 13u);
    save(&s1, &f1);
    save(&s2, &f2);
    /* os ponteiros (barramento, hal) diferem entre instâncias; compara o
     * estado arquitetural e os dados */
    ASSERT_MEM_EQ(&s1.vm.cpu, &s2.vm.cpu, sizeof s1.vm.cpu);
    ASSERT_EQ_U32(s1.vm.insn_count, s2.vm.insn_count);
    ASSERT_EQ_U32(s1.vm.rng_state, s2.vm.rng_state);
    ASSERT_EQ(s1.vm.status, s2.vm.status);
    ASSERT_MEM_EQ(s1.ram, s2.ram, sizeof s1.ram);
    ASSERT_MEM_EQ(s1.hn.out, s2.hn.out, sizeof s1.hn.out);
    ASSERT_EQ_U32(s1.hn.out_total, s2.hn.out_total);
}

/* Snapshot de uma VM parada (fault ou halt) restaura o mesmo estado parado
 * e a VM restaurada continua parada. */
TEST(snapshot_of_stopped_vm)
{
    static fixture f;
    static snap s, s2;
    LOAD(&f, OP_I(MOVI, 1, 0, 9), OP_I(TRAP, 0, 0, 5));
    (void)fix_run(&f);
    ASSERT_EQ(f.vm.status, KSVM_FAULT);
    save(&s, &f);
    steps(&f, 10u);
    save(&s2, &f);
    ASSERT_MEM_EQ(&s, &s2, sizeof s);
    restore(&f, &s);
    ASSERT_EQ(ksvm_step(&f.vm), KSVM_FAULT);
    ASSERT_EQ_U32(f.vm.fault_addr, 5u);
}

SUITE(snapshot)
{
    RUN_TEST(snapshot_restore_replays_identically);
    RUN_TEST(two_fresh_instances_are_identical);
    RUN_TEST(snapshot_of_stopped_vm);
}
