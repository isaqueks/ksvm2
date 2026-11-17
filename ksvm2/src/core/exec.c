/*
 * exec.c — ciclo fetch-decode-execute (doc 03 §5) e semântica de cada
 * instrução (doc 04 §5).
 *
 * Contrato de atomicidade: uma instrução que entra em fault não altera
 * registradores, memória, flags nem pc (o pc continua apontando para ela).
 * Por isso cada caso valida/acessa a memória ANTES de escrever registradores.
 *
 * Despacho: o nibble alto do opcode escolhe o grupo do doc 04 §5 (0x0
 * movimentação, 0x1 memória, 0x2 pilha, 0x3-0x4 aritmética, 0x5-0x6 lógica,
 * 0x7 fluxo); dentro do grupo há um switch por opcode.
 *
 * Não implementados nesta versão (geram KSVM_FAULT_BAD_OPCODE): PUSHM/POPM
 * (opcionais, fase 5), e MUL/DIV/CLZ... quando desligados na config.
 */
#include "ksvm/alu.h"
#include "ksvm/bus.h"
#include "ksvm/isa.h"
#include "ksvm/syscall.h"
#include "ksvm/vm.h"

/* ---- Acesso a registradores e operandos ------------------------------- */

static uint32_t reg(const ksvm *vm, uint8_t i)
{
    return vm->cpu.r[i];          /* i vem de campo de 5 bits: sempre < 32 */
}

static void set_reg(ksvm *vm, uint8_t i, uint32_t v)
{
    vm->cpu.r[i] = v;             /* escrita em r0 é desfeita no fim do passo */
}

/* Escreve rd e atualiza Z/N a partir do valor. */
static void set_reg_zn(ksvm *vm, uint8_t i, uint32_t v)
{
    set_reg(vm, i, ksvm_alu_zn(v, &vm->cpu.flags));
}

/* Segundo operando-fonte de "rd, ra, rb|imm": rc (formato R) ou imediato. */
static uint32_t operand2(const ksvm *vm, const ksvm_insn *in)
{
    return (in->format == KSVM_FMT_R) ? reg(vm, in->rc) : in->imm;
}

/* Segundo operando de "ra, rb|imm" sem destino (CMP*, TST*). */
static uint32_t compare_operand(const ksvm *vm, const ksvm_insn *in)
{
    return (in->format == KSVM_FMT_R) ? reg(vm, in->rb) : in->imm;
}

static void bad_opcode(ksvm *vm, const ksvm_insn *in)
{
    ksvm_set_fault(vm, KSVM_FAULT_BAD_OPCODE, in->word);
}

/* ---- Memória e pilha -------------------------------------------------- */

static bool mem_ok(ksvm *vm, ksvm_fault f, uint32_t addr)
{
    if (f == KSVM_FAULT_NONE) {
        return true;
    }
    ksvm_set_fault(vm, f, addr);
    return false;
}

static bool read_sized(ksvm *vm, uint32_t ea, unsigned size, uint32_t *out)
{
    uint8_t b = 0;
    uint16_t h = 0;
    ksvm_fault f;

    switch (size) {
    case 1u:  f = ksvm_bus_read8(&vm->bus, ea, &b);  *out = b; break;
    case 2u:  f = ksvm_bus_read16(&vm->bus, ea, &h); *out = h; break;
    default:  f = ksvm_bus_read32(&vm->bus, ea, out);          break;
    }
    return mem_ok(vm, f, ea);
}

static void load(ksvm *vm, uint8_t rd, uint32_t ea, unsigned size, bool sign)
{
    uint32_t v = 0u;

    if (read_sized(vm, ea, size, &v)) {
        set_reg(vm, rd, sign ? ksvm_sext(v, size * 8u) : v);
    }
}

static void store(ksvm *vm, uint32_t ea, unsigned size, uint32_t v)
{
    ksvm_fault f;

    switch (size) {
    case 1u:  f = ksvm_bus_write8(&vm->bus, ea, (uint8_t)(v & 0xFFu));      break;
    case 2u:  f = ksvm_bus_write16(&vm->bus, ea, (uint16_t)(v & 0xFFFFu));  break;
    default:  f = ksvm_bus_write32(&vm->bus, ea, v);                         break;
    }
    (void)mem_ok(vm, f, ea);
}

/* sp só é atualizado depois que o store deu certo (atomicidade). */
static bool push32(ksvm *vm, uint32_t v)
{
    uint32_t sp = vm->cpu.r[KSVM_REG_SP] - 4u;

#if KSVM_CHECK_STACK
    if (sp < vm->stack_limit) {
        ksvm_set_fault(vm, KSVM_FAULT_STACK, sp);
        return false;
    }
#endif
    if (!mem_ok(vm, ksvm_bus_write32(&vm->bus, sp, v), sp)) {
        return false;
    }
    vm->cpu.r[KSVM_REG_SP] = sp;
    return true;
}

static bool pop32(ksvm *vm, uint32_t *out)
{
    uint32_t sp = vm->cpu.r[KSVM_REG_SP];

    if (!mem_ok(vm, ksvm_bus_read32(&vm->bus, sp, out), sp)) {
        return false;
    }
    vm->cpu.r[KSVM_REG_SP] = sp + 4u;
    return true;
}

/* ---- Grupo 0x0: movimentação ------------------------------------------ */

static void exec_mfsr(ksvm *vm, const ksvm_insn *in)
{
    uint32_t v;

    switch (in->imm) {
    case KSVM_SR_PC:          v = vm->cpu.pc;       break;  /* desta instrução */
    case KSVM_SR_FLAGS:       v = vm->cpu.flags;    break;
    case KSVM_SR_ISA_VERSION: v = KSVM_ISA_VERSION; break;
    case KSVM_SR_INSN_COUNT:  v = vm->insn_count;   break;
    default:                  bad_opcode(vm, in);   return;
    }
    set_reg(vm, in->ra, v);
}

/* Só FLAGS é gravável; os bits reservados continuam lidos como 0. */
static void exec_mtsr(ksvm *vm, const ksvm_insn *in)
{
    if (in->imm != KSVM_SR_FLAGS) {
        bad_opcode(vm, in);
        return;
    }
    vm->cpu.flags = reg(vm, in->rb) & KSVM_FLAGS_MASK;
}

static void exec_move(ksvm *vm, const ksvm_insn *in, uint32_t pc_next)
{
    switch (in->op) {
    case KSVM_OP_NOP:   break;
    case KSVM_OP_MOV:   set_reg(vm, in->ra, reg(vm, in->rb)); break;
    case KSVM_OP_MOVI:
    case KSVM_OP_MOVIL: set_reg(vm, in->ra, in->imm); break;
    case KSVM_OP_ADR:
    case KSVM_OP_ADRL:  set_reg(vm, in->ra, pc_next + in->imm); break;
    case KSVM_OP_MFSR:  exec_mfsr(vm, in); break;
    case KSVM_OP_MTSR:  exec_mtsr(vm, in); break;
    default:            bad_opcode(vm, in); break;
    }
}

/* ---- Grupo 0x1: load/store -------------------------------------------- */

static void exec_mem(ksvm *vm, const ksvm_insn *in)
{
    uint32_t ea = reg(vm, in->rb) + operand2(vm, in);   /* rb + imm14 | rb + rc */

    switch (in->op) {
    case KSVM_OP_LDB:  case KSVM_OP_LDBX:  load(vm, in->ra, ea, 1u, false); break;
    case KSVM_OP_LDBS: case KSVM_OP_LDBSX: load(vm, in->ra, ea, 1u, true);  break;
    case KSVM_OP_LDH:  case KSVM_OP_LDHX:  load(vm, in->ra, ea, 2u, false); break;
    case KSVM_OP_LDHS: case KSVM_OP_LDHSX: load(vm, in->ra, ea, 2u, true);  break;
    case KSVM_OP_LDW:  case KSVM_OP_LDWX:  load(vm, in->ra, ea, 4u, false); break;
    case KSVM_OP_STB:  case KSVM_OP_STBX:  store(vm, ea, 1u, reg(vm, in->ra)); break;
    case KSVM_OP_STH:  case KSVM_OP_STHX:  store(vm, ea, 2u, reg(vm, in->ra)); break;
    case KSVM_OP_STW:  case KSVM_OP_STWX:  store(vm, ea, 4u, reg(vm, in->ra)); break;
    default:                               bad_opcode(vm, in); break;
    }
}

/* ---- Grupo 0x2: pilha ------------------------------------------------- */

/* PUSH sp empilha o sp ANTERIOR (operandos lidos no início da instrução).
 * POP sp termina com sp = valor lido (a escrita do destino prevalece). */
static void exec_stack(ksvm *vm, const ksvm_insn *in)
{
    uint32_t v = 0u;

    switch (in->op) {
    case KSVM_OP_PUSH:
        (void)push32(vm, reg(vm, in->ra));
        break;
    case KSVM_OP_POP:
        if (pop32(vm, &v)) {
            set_reg(vm, in->ra, v);
        }
        break;
    default:                      /* PUSHM/POPM: opcionais, não implementados */
        bad_opcode(vm, in);
        break;
    }
}

/* ---- Grupos 0x3-0x4: aritmética --------------------------------------- */

#if KSVM_ENABLE_MULDIV
static void exec_divide(ksvm *vm, const ksvm_insn *in)
{
    uint32_t a = reg(vm, in->rb), b = reg(vm, in->rc), q = 0u;
    bool ok;

    switch (in->op) {
    case KSVM_OP_DIV:  ok = ksvm_alu_div(a, b, &q);  break;
    case KSVM_OP_DIVU: ok = ksvm_alu_divu(a, b, &q); break;
    case KSVM_OP_REM:  ok = ksvm_alu_rem(a, b, &q);  break;
    default:           ok = ksvm_alu_remu(a, b, &q); break;
    }
    if (!ok) {
        ksvm_set_fault(vm, KSVM_FAULT_DIV_ZERO, vm->cpu.pc);
        return;
    }
    set_reg_zn(vm, in->ra, q);
}
#endif

static void exec_arith(ksvm *vm, const ksvm_insn *in)
{
    uint32_t a = reg(vm, in->rb);
    uint32_t *fl = &vm->cpu.flags;

    switch (in->op) {
    case KSVM_OP_ADD: case KSVM_OP_ADDI: case KSVM_OP_ADDL:
        set_reg(vm, in->ra, ksvm_alu_add(a, operand2(vm, in), fl)); break;
    case KSVM_OP_SUB: case KSVM_OP_SUBI:
        set_reg(vm, in->ra, ksvm_alu_sub(a, operand2(vm, in), fl)); break;
    case KSVM_OP_ADC: set_reg(vm, in->ra, ksvm_alu_adc(a, operand2(vm, in), fl)); break;
    case KSVM_OP_SBC: set_reg(vm, in->ra, ksvm_alu_sbc(a, operand2(vm, in), fl)); break;
    case KSVM_OP_NEG: set_reg(vm, in->ra, ksvm_alu_sub(0u, a, fl)); break;
    case KSVM_OP_CMP: case KSVM_OP_CMPI: case KSVM_OP_CMPL:
        (void)ksvm_alu_sub(reg(vm, in->ra), compare_operand(vm, in), fl); break;
#if KSVM_ENABLE_MULDIV
    case KSVM_OP_MUL: case KSVM_OP_MULI:
        set_reg_zn(vm, in->ra, ksvm_alu_mul(a, operand2(vm, in))); break;
    case KSVM_OP_MULH:  set_reg_zn(vm, in->ra, ksvm_alu_mulh(a, reg(vm, in->rc)));  break;
    case KSVM_OP_MULHU: set_reg_zn(vm, in->ra, ksvm_alu_mulhu(a, reg(vm, in->rc))); break;
    case KSVM_OP_DIV: case KSVM_OP_DIVU: case KSVM_OP_REM: case KSVM_OP_REMU:
        exec_divide(vm, in); break;
#endif
    default: bad_opcode(vm, in); break;
    }
}

/* ---- Grupos 0x5-0x6: lógica, deslocamentos, extensões ----------------- */

/* Operações de um operando "rd, rb" (0x64..0x6A). */
static void exec_unary(ksvm *vm, const ksvm_insn *in)
{
    uint32_t a = reg(vm, in->rb);

    switch (in->op) {
    case KSVM_OP_SXB:    set_reg_zn(vm, in->ra, ksvm_sext8(a));           break;
    case KSVM_OP_SXH:    set_reg_zn(vm, in->ra, ksvm_sext16(a));          break;
    case KSVM_OP_ZXB:    set_reg_zn(vm, in->ra, a & 0xFFu);               break;
    case KSVM_OP_ZXH:    set_reg_zn(vm, in->ra, a & UINT32_C(0xFFFF));    break;
#if KSVM_ENABLE_BITOPS
    case KSVM_OP_CLZ:    set_reg_zn(vm, in->ra, ksvm_alu_clz(a));         break;
    case KSVM_OP_CTZ:    set_reg_zn(vm, in->ra, ksvm_alu_ctz(a));         break;
    case KSVM_OP_POPCNT: set_reg_zn(vm, in->ra, ksvm_alu_popcnt(a));      break;
#endif
    default:             bad_opcode(vm, in);                              break;
    }
}

static void exec_logic(ksvm *vm, const ksvm_insn *in)
{
    uint32_t a = reg(vm, in->rb);
    uint32_t b = operand2(vm, in);
    uint32_t *fl = &vm->cpu.flags;

    switch (in->op) {
    case KSVM_OP_AND: case KSVM_OP_ANDI: case KSVM_OP_ANDL: set_reg_zn(vm, in->ra, a & b); break;
    case KSVM_OP_OR:  case KSVM_OP_ORI:  case KSVM_OP_ORL:  set_reg_zn(vm, in->ra, a | b); break;
    case KSVM_OP_XOR: case KSVM_OP_XORI: case KSVM_OP_XORL: set_reg_zn(vm, in->ra, a ^ b); break;
    case KSVM_OP_NOT: set_reg_zn(vm, in->ra, ~a); break;
    case KSVM_OP_SHL: case KSVM_OP_SHLI: set_reg(vm, in->ra, ksvm_alu_shl(a, b, fl)); break;
    case KSVM_OP_SHR: case KSVM_OP_SHRI: set_reg(vm, in->ra, ksvm_alu_shr(a, b, fl)); break;
    case KSVM_OP_SAR: case KSVM_OP_SARI: set_reg(vm, in->ra, ksvm_alu_sar(a, b, fl)); break;
    case KSVM_OP_ROL: set_reg(vm, in->ra, ksvm_alu_rol(a, b, fl)); break;
    case KSVM_OP_ROR: set_reg(vm, in->ra, ksvm_alu_ror(a, b, fl)); break;
    case KSVM_OP_TST: case KSVM_OP_TSTI:
        (void)ksvm_alu_zn(reg(vm, in->ra) & compare_operand(vm, in), fl); break;
    default: exec_unary(vm, in); break;
    }
}

/* ---- Grupo 0x7: fluxo e sistema --------------------------------------- */

/* Empilha o endereço de retorno; só desvia se o push deu certo. */
static void exec_call(ksvm *vm, uint32_t *next_pc, uint32_t target)
{
    if (push32(vm, *next_pc)) {
        *next_pc = target;
    }
}

static void exec_ret(ksvm *vm, uint32_t *next_pc)
{
    uint32_t target = 0u;

    if (pop32(vm, &target)) {
        *next_pc = target;
    }
}

static void exec_brk(ksvm *vm, const ksvm_insn *in)
{
#if KSVM_ENABLE_TRACE
    if (vm->trace != NULL) {
        vm->trace(vm->trace_ctx, vm, KSVM_TRACE_BRK, vm->cpu.pc, in->word);
    }
#else
    (void)vm;
    (void)in;
#endif
}

/* Alvos desalinhados ou fora de EXEC só falham no próximo fetch (doc 04 §5.6). */
static void exec_flow(ksvm *vm, const ksvm_insn *in, uint32_t *next_pc)
{
    switch (in->op) {
    case KSVM_OP_JMP:     *next_pc += in->imm; break;
    case KSVM_OP_JMPR:    *next_pc = reg(vm, in->rb); break;
    case KSVM_OP_BCOND:
        if (ksvm_cond_holds(vm->cpu.flags, in->cond)) {
            *next_pc += in->imm;
        }
        break;
    case KSVM_OP_CALL:    exec_call(vm, next_pc, *next_pc + in->imm); break;
    case KSVM_OP_CALLR:   exec_call(vm, next_pc, reg(vm, in->rb)); break;
    case KSVM_OP_RET:     exec_ret(vm, next_pc); break;
    case KSVM_OP_HALT:    ksvm_halt(vm, reg(vm, 1u)); break;
    case KSVM_OP_TRAP:    ksvm_set_fault(vm, KSVM_FAULT_TRAP, in->imm); break;
    case KSVM_OP_SYSCALL: ksvm_syscall(vm, ksvm_field_uimm14(in->word)); break;
    case KSVM_OP_BRK:     exec_brk(vm, in); break;
    case KSVM_OP_WFI:     vm->status = KSVM_YIELD; break;
    default:              bad_opcode(vm, in); break;
    }
}

/* ---- Ciclo ------------------------------------------------------------ */

static void execute(ksvm *vm, const ksvm_insn *in, uint32_t *next_pc)
{
    switch (in->op >> 4) {
    case 0x0:           exec_move(vm, in, *next_pc); break;
    case 0x1:           exec_mem(vm, in);            break;
    case 0x2:           exec_stack(vm, in);          break;
    case 0x3: case 0x4: exec_arith(vm, in);          break;
    case 0x5: case 0x6: exec_logic(vm, in);          break;
    case 0x7:           exec_flow(vm, in, next_pc);  break;
    default:            bad_opcode(vm, in);          break;
    }
}

/*
 * Passos 1-6 do doc 03 §5. Qualquer falha de busca é PC_RANGE, antes de
 * qualquer efeito (doc 04 §5.6, "Limites do fetch"):
 *   - a instrução inteira (as duas palavras no formato L) numa única região
 *     EXEC: ksvm_bus_fetch_imm32 exige [pc, pc + 8) numa só região, então
 *     o imm32 não vem de uma região vizinha nem do endereço 0;
 *   - PC_next = pc + len sem passar de 0xFFFFFFFF. Como as regiões terminam
 *     no máximo em 2^32 e a instrução já cabe numa delas, só sobra o caso em
 *     que ela termina exatamente em 2^32 (pc + len == 0): uma comparação.
 * fault_addr: pc, ou pc + 4 se só a segunda palavra falhou (no topo do
 * espaço, onde pc + 4 não existe, pc).
 */
static bool fetch(ksvm *vm, uint32_t pc, ksvm_insn *in)
{
    uint32_t w = 0u, imm32 = 0u;

    if ((pc & 3u) != 0u) {
        ksvm_set_fault(vm, KSVM_FAULT_PC_ALIGN, pc);
        return false;
    }
    if (ksvm_bus_fetch32(&vm->bus, pc, &w) != KSVM_FAULT_NONE) {
        ksvm_set_fault(vm, KSVM_FAULT_PC_RANGE, pc);
        return false;
    }
    if (ksvm_isa_len(ksvm_field_op(w)) == 8u &&
        ksvm_bus_fetch_imm32(&vm->bus, pc, &imm32) != KSVM_FAULT_NONE) {
        ksvm_set_fault(vm, KSVM_FAULT_PC_RANGE, (pc + 4u != 0u) ? pc + 4u : pc);
        return false;
    }
    if (!ksvm_decode(w, imm32, in)) {
        ksvm_set_fault(vm, KSVM_FAULT_BAD_OPCODE, w);
        return false;
    }
    if (pc + in->len < pc) {                    /* PC_next daria a volta */
        ksvm_set_fault(vm, KSVM_FAULT_PC_RANGE, pc);
        return false;
    }
    return true;
}

static void trace_step(ksvm *vm, uint32_t pc, uint32_t word)
{
#if KSVM_ENABLE_TRACE
    if (vm->trace != NULL) {
        vm->trace(vm->trace_ctx, vm, KSVM_TRACE_STEP, pc, word);
    }
#else
    (void)vm;
    (void)pc;
    (void)word;
#endif
}

ksvm_status ksvm_step(ksvm *vm)
{
    ksvm_insn in;
    uint32_t pc = vm->cpu.pc;
    uint32_t next_pc;

    if (vm->status == KSVM_YIELD) {
        vm->status = KSVM_RUNNING;
    }
    if (vm->status != KSVM_RUNNING || !fetch(vm, pc, &in)) {
        return vm->status;
    }
    next_pc = pc + in.len;
    execute(vm, &in, &next_pc);
    vm->cpu.r[KSVM_REG_ZR] = 0u;          /* zr: escritas descartadas */
    if (vm->status != KSVM_FAULT) {       /* fault: pc fica na instrução */
        vm->cpu.pc = next_pc;
        vm->insn_count++;
    }
    trace_step(vm, pc, in.word);
    return vm->status;
}
