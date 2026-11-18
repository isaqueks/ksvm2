/*
 * insn.c — instruções e pseudo-instruções -> itens INSN (ver insn.h).
 *
 * A forma dos operandos de cada opcode vem de ksas_op_shape (mnemonic.c);
 * o mapeamento operando -> campo segue o topo de isa.def:
 *   rd -> ra;  "rd, ra, rb" -> ra, rb, rc;  "rd, ra, imm" -> ra, rb, imm;
 *   "rd, rb" -> ra, rb;  JMPR/CALLR/MTSR -> rb;  stores: fonte em ra.
 */
#include <string.h>

#include "ksvm/isa.h"
#include "insn.h"
#include "mnemonic.h"
#include "parser.h"

typedef struct mem_opnd {
    uint8_t  base, index;
    bool     indexed;     /* [rb + rc] */
    bool     absolute;    /* [expr], base = zr */
    uint32_t expr;        /* KSAS_NONE = deslocamento 0 */
} mem_opnd;

static void insn_init(ksas_insn *in, uint8_t op)
{
    memset(in, 0, sizeof *in);
    in->op = op;
    in->long_op = KSAS_NO_OP;
    in->imm_kind = ksas_op_imm_kind(op);
    in->expr = KSAS_NONE;
}

static bool add_insn(ksas_ctx *c, uint32_t col, const ksas_insn *in)
{
    uint32_t item;

    if (c->section != KSAS_SEC_TEXT) {
        return ksas_perr(c, col, "instrucoes so podem aparecer em .text (secao atual: %s)",
                         ksas_section_name(c->section));
    }
    item = ksas_item_new(c, KSAS_IT_INSN, col);
    if (item == KSAS_NONE) {
        return false;
    }
    c->items[item].u.insn = *in;
    return true;
}

/* Imediato que pode não caber em 14 bits: o layout escolhe I ou L.
 * long_if_unknown: se o valor ainda não é conhecido no passo 1, usa a
 * forma longa (li, cmp/add/... com imediato). Os mnemônicos explícitos
 * "xxxI" preferem a curta e só crescem se o valor conhecido não couber. */
static void make_variable(ksas_insn *in, bool long_if_unknown)
{
    if (in->op == KSVM_OP_SUBI) {
        in->long_op = KSVM_OP_ADDL;          /* não existe SUBL: ADDL com -imm */
        in->flags |= KSAS_INSN_NEG_LONG;
    } else {
        in->long_op = ksas_op_long_form(in->op);
    }
    if (in->long_op != KSAS_NO_OP) {
        in->flags |= KSAS_INSN_VAR;
        if (long_if_unknown) {
            in->flags |= KSAS_INSN_LONG_UNK;
        }
    }
}

static bool comma(ksas_ctx *c, ksas_cursor *cur)
{
    return ksas_expect(c, cur, TK_COMMA, "','");
}

static bool reg_comma(ksas_ctx *c, ksas_cursor *cur, uint8_t *r)
{
    return ksas_expect_reg(c, cur, r) && comma(c, cur);
}

typedef bool (*name_lookup_fn)(const char *s, uint32_t n, uint32_t *v);

/* Operando que também aceita um nome próprio (syscall, registrador
 * especial). O nome tem precedência sobre um símbolo de mesmo nome. */
static bool named_operand(ksas_ctx *c, ksas_cursor *cur, name_lookup_fn lookup, uint32_t *node)
{
    ksas_token next;
    uint32_t v;

    if (cur->tok.kind == TK_IDENT) {
        ksas_cur_peek(cur, &next);
        if ((next.kind == TK_COMMA || next.kind == TK_NEWLINE || next.kind == TK_EOF) &&
            lookup(cur->tok.text, cur->tok.len, &v)) {
            *node = ksas_const_node(c, v, cur->tok.col);
            ksas_advance(c, cur);
            return *node != KSAS_NONE;
        }
    }
    return ksas_parse_expr(c, cur, node);
}

/* Opcode I correspondente a um mnemônico R usado com imediato (ADD -> ADDI). */
static bool imm_variant(ksas_ctx *c, uint8_t op, uint32_t col, uint8_t *iop)
{
    char name[16];

    if (ksas_op_suffixed(op, "I", iop)) {
        return true;
    }
    return ksas_perr(c, col, "'%s' nao aceita imediato", ksas_op_lower(op, name, sizeof name));
}

/* "rd, ra, imm" a partir de um mnemônico R com imediato. */
static bool reg_imm_form(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col,
                         uint8_t rd, uint8_t ra)
{
    ksas_insn in;
    uint8_t iop;

    if (!imm_variant(c, op, cur->tok.col, &iop)) {
        return false;
    }
    insn_init(&in, iop);
    in.ra = rd;
    in.rb = ra;
    if (!ksas_parse_expr(c, cur, &in.expr)) {
        return false;
    }
    make_variable(&in, true);
    return add_insn(c, col, &in);
}

/* ---- Formas de operandos ---------------------------------------------- */

static bool sh_none(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col)
{
    ksas_insn in;

    (void)cur;
    insn_init(&in, op);
    return add_insn(c, col, &in);
}

/* rd, ra, rb | rd, ra, imm */
static bool sh_r3(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col)
{
    ksas_insn in;
    uint8_t rd, ra;

    if (!reg_comma(c, cur, &rd) || !reg_comma(c, cur, &ra)) {
        return false;
    }
    if (cur->tok.kind != TK_REG) {
        return reg_imm_form(c, cur, op, col, rd, ra);
    }
    insn_init(&in, op);
    in.ra = rd;
    in.rb = ra;
    in.rc = (uint8_t)cur->tok.value;
    ksas_advance(c, cur);
    return add_insn(c, col, &in);
}

/* rd, rb | rd, imm | rd (operação no próprio registrador: "not rd") */
static bool sh_r2(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col)
{
    ksas_insn in;
    uint8_t rd;
    char name[16];

    if (!ksas_expect_reg(c, cur, &rd)) {
        return false;
    }
    insn_init(&in, op);
    in.ra = rd;
    in.rb = rd;
    if (ksas_at_eol(cur)) {
        if (op == KSVM_OP_MOV || op == KSVM_OP_CMP || op == KSVM_OP_TST || op == KSVM_OP_FCMP) {
            return ksas_perr(c, cur->tok.col, "'%s' espera dois operandos",
                             ksas_op_lower(op, name, sizeof name));
        }
        return add_insn(c, col, &in);
    }
    if (!comma(c, cur)) {
        return false;
    }
    if (cur->tok.kind != TK_REG) {
        return reg_imm_form(c, cur, op, col, rd, 0);
    }
    in.rb = (uint8_t)cur->tok.value;
    ksas_advance(c, cur);
    return add_insn(c, col, &in);
}

/* push r1[, r2...] / pop r1[, r2...]: um item por registrador, na ordem. */
static bool sh_ra(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col)
{
    ksas_insn in;
    uint8_t r;

    for (;;) {
        if (!ksas_expect_reg(c, cur, &r)) {
            return false;
        }
        insn_init(&in, op);
        in.ra = r;
        if (!add_insn(c, col, &in)) {
            return false;
        }
        if (cur->tok.kind != TK_COMMA) {
            return true;
        }
        ksas_advance(c, cur);
    }
}

static bool sh_rb(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col)
{
    ksas_insn in;

    insn_init(&in, op);
    return ksas_expect_reg(c, cur, &in.rb) && add_insn(c, col, &in);
}

/* [rb] [rb + expr] [rb - expr] [rb + rc] [expr] */
static bool parse_mem(ksas_ctx *c, ksas_cursor *cur, mem_opnd *m)
{
    ksas_token next;

    memset(m, 0, sizeof *m);
    m->expr = KSAS_NONE;
    if (!ksas_expect(c, cur, TK_LBRACK, "'['")) {
        return false;
    }
    if (cur->tok.kind != TK_REG) {
        m->absolute = true;
        if (!ksas_parse_expr(c, cur, &m->expr)) {
            return false;
        }
    } else {
        m->base = (uint8_t)cur->tok.value;
        ksas_advance(c, cur);
        ksas_cur_peek(cur, &next);
        if (cur->tok.kind == TK_PLUS && next.kind == TK_REG) {
            ksas_advance(c, cur);
            m->indexed = true;
            m->index = (uint8_t)cur->tok.value;
            ksas_advance(c, cur);
        } else if (cur->tok.kind == TK_PLUS || cur->tok.kind == TK_MINUS) {
            /* O sinal entra na expressão como unário: [rb - 4 + 2] = rb - 2. */
            if (!ksas_parse_expr(c, cur, &m->expr)) {
                return false;
            }
        }
    }
    return ksas_expect(c, cur, TK_RBRACK, "']'");
}

/* Load/store: [rb + rc] usa a variante X (LDB -> LDBX); as demais, imm14. */
static bool mem_insn(ksas_ctx *c, uint8_t op, uint32_t col, uint8_t reg, const mem_opnd *m)
{
    bool xform = (ksvm_isa[op].format == KSVM_FMT_R);
    ksas_insn in;
    char name[16];

    if (m->indexed && !xform && !ksas_op_suffixed(op, "X", &op)) {
        return ksas_perr(c, col, "'%s' nao aceita [rb + rc]", ksas_op_lower(op, name, sizeof name));
    }
    if (!m->indexed && xform) {
        return ksas_perr(c, col, "'%s' exige endereco [rb + rc]",
                         ksas_op_lower(op, name, sizeof name));
    }
    insn_init(&in, op);
    in.ra = reg;
    in.rb = m->base;
    if (m->indexed) {
        in.rc = m->index;
    } else {
        in.expr = m->expr;
        if (m->absolute) {
            in.flags |= KSAS_INSN_MEMABS;
        }
    }
    return add_insn(c, col, &in);
}

static bool sh_load(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col)
{
    mem_opnd m;
    uint8_t rd;

    return reg_comma(c, cur, &rd) && parse_mem(c, cur, &m) && mem_insn(c, op, col, rd, &m);
}

/* Forma do doc 04: "stb [rb + imm], ra". Também aceita "stb ra, [rb + imm]". */
static bool sh_store(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col)
{
    mem_opnd m;
    uint8_t rs;

    if (cur->tok.kind == TK_LBRACK) {
        if (!parse_mem(c, cur, &m) || !comma(c, cur) || !ksas_expect_reg(c, cur, &rs)) {
            return false;
        }
    } else if (!reg_comma(c, cur, &rs) || !parse_mem(c, cur, &m)) {
        return false;
    }
    return mem_insn(c, op, col, rs, &m);
}

/* rd, ra, imm (opcodes I/L explícitos: addi, andl, shli...) */
static bool sh_rri(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col)
{
    ksas_insn in;
    uint8_t rd, ra;

    if (!reg_comma(c, cur, &rd) || !reg_comma(c, cur, &ra)) {
        return false;
    }
    insn_init(&in, op);
    in.ra = rd;
    in.rb = ra;
    if (!ksas_parse_expr(c, cur, &in.expr)) {
        return false;
    }
    if (ksvm_isa[op].format == KSVM_FMT_I) {
        make_variable(&in, false);
    }
    return add_insn(c, col, &in);
}

/* rd, imm / ra, imm (movi, movil, cmpi, adr, mfsr...) */
static bool sh_ri(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col)
{
    ksas_insn in;
    uint8_t rd;
    bool ok;

    if (!reg_comma(c, cur, &rd)) {
        return false;
    }
    insn_init(&in, op);
    in.ra = rd;
    ok = (op == KSVM_OP_MFSR) ? named_operand(c, cur, ksas_sr_lookup, &in.expr)
                              : ksas_parse_expr(c, cur, &in.expr);
    if (!ok) {
        return false;
    }
    if (ksvm_isa[op].format == KSVM_FMT_I) {
        make_variable(&in, false);
    }
    return add_insn(c, col, &in);
}

/* mtsr sr, rb */
static bool sh_sr_rb(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col)
{
    ksas_insn in;

    insn_init(&in, op);
    return named_operand(c, cur, ksas_sr_lookup, &in.expr) && comma(c, cur) &&
           ksas_expect_reg(c, cur, &in.rb) && add_insn(c, col, &in);
}

/* trap imm, syscall imm|NOME, pushm/popm imm */
static bool sh_imm(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col)
{
    ksas_insn in;
    bool ok;

    insn_init(&in, op);
    ok = (op == KSVM_OP_SYSCALL) ? named_operand(c, cur, ksas_syscall_lookup, &in.expr)
                                 : ksas_parse_expr(c, cur, &in.expr);
    return ok && add_insn(c, col, &in);
}

static bool sh_jump(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col)
{
    ksas_insn in;

    insn_init(&in, op);
    return ksas_parse_expr(c, cur, &in.expr) && add_insn(c, col, &in);
}

static bool sh_branch(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col)
{
    (void)cur; (void)op;
    return ksas_perr(c, col, "use b<cond> (beq, bne, b.lt...) ou b para desvios");
}

typedef bool (*shape_fn)(ksas_ctx *c, ksas_cursor *cur, uint8_t op, uint32_t col);

static const shape_fn shape_fns[] = {
    [KSAS_SH_NONE] = sh_none,     [KSAS_SH_R3] = sh_r3,       [KSAS_SH_R2] = sh_r2,
    [KSAS_SH_RA] = sh_ra,         [KSAS_SH_RB] = sh_rb,       [KSAS_SH_LOAD] = sh_load,
    [KSAS_SH_STORE] = sh_store,   [KSAS_SH_RRI] = sh_rri,     [KSAS_SH_RI] = sh_ri,
    [KSAS_SH_SR_RB] = sh_sr_rb,   [KSAS_SH_IMM] = sh_imm,     [KSAS_SH_BRANCH] = sh_branch,
    [KSAS_SH_JUMP] = sh_jump
};

/* ---- Desvios e pseudo-instruções -------------------------------------- */

static bool branch_to(ksas_ctx *c, ksas_cursor *cur, uint32_t col, uint8_t cond)
{
    ksas_insn in;

    insn_init(&in, KSVM_OP_BCOND);
    in.cond = cond;
    return ksas_parse_expr(c, cur, &in.expr) && add_insn(c, col, &in);
}

/* "beq", "b.eq", "bcs"... -> condição. */
static bool branch_cond(const char *s, uint32_t n, uint8_t *cond)
{
    if (n < 2u || (s[0] != 'b' && s[0] != 'B')) {
        return false;
    }
    s++;
    n--;
    if (s[0] == '.') {
        s++;
        n--;
    }
    return n > 0u && ksas_cond_lookup(s, n, cond);
}

/* li rd, valor: MOVI se cabe em 14 bits com sinal, senão MOVIL. */
static bool ps_li(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    ksas_insn in;
    uint8_t rd;

    if (!reg_comma(c, cur, &rd)) {
        return false;
    }
    insn_init(&in, KSVM_OP_MOVI);
    in.ra = rd;
    if (!ksas_parse_expr(c, cur, &in.expr)) {
        return false;
    }
    make_variable(&in, true);
    return add_insn(c, col, &in);
}

/* la rd, label: MOVIL absoluto, ou ADRL PC-relativo com .pic (no emit). */
static bool ps_la(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    ksas_insn in;
    uint8_t rd;

    if (!reg_comma(c, cur, &rd)) {
        return false;
    }
    insn_init(&in, KSVM_OP_MOVIL);
    in.ra = rd;
    in.flags = KSAS_INSN_LA;
    return ksas_parse_expr(c, cur, &in.expr) && add_insn(c, col, &in);
}

/* fli rd, valor: MOVIL rd, bits binary32 do valor (sempre a forma longa:
 * "fli r1, 1" carrega 1.0 = 0x3F800000, não o inteiro 1). */
static bool ps_fli(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    ksas_insn in;
    uint8_t rd;
    uint32_t bits, vcol;

    /* Avanços sem reportar TK_ERROR: "1.5" não é token (ksas_parse_float). */
    if (cur->tok.kind != TK_REG) {
        return ksas_perr(c, cur->tok.col, "esperado registrador");
    }
    rd = (uint8_t)cur->tok.value;
    ksas_cur_advance(cur);
    if (cur->tok.kind != TK_COMMA) {
        return ksas_perr(c, cur->tok.col, "esperado ','");
    }
    ksas_cur_advance(cur);
    vcol = cur->tok.col;
    if (!ksas_parse_float(c, cur, &bits)) {
        return false;
    }
    insn_init(&in, KSVM_OP_MOVIL);
    in.ra = rd;
    in.expr = ksas_const_node(c, bits, vcol);
    return in.expr != KSAS_NONE && add_insn(c, col, &in);
}

/* clr rd = mov rd, zr */
static bool ps_clr(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    ksas_insn in;

    insn_init(&in, KSVM_OP_MOV);
    return ksas_expect_reg(c, cur, &in.ra) && add_insn(c, col, &in);
}

static bool inc_dec(ksas_ctx *c, ksas_cursor *cur, uint32_t col, uint8_t op)
{
    ksas_insn in;

    insn_init(&in, op);
    if (!ksas_expect_reg(c, cur, &in.ra)) {
        return false;
    }
    in.rb = in.ra;
    in.expr = ksas_const_node(c, 1u, col);
    return in.expr != KSAS_NONE && add_insn(c, col, &in);
}

static bool ps_inc(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    return inc_dec(c, cur, col, KSVM_OP_ADDI);
}

static bool ps_dec(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    return inc_dec(c, cur, col, KSVM_OP_SUBI);
}

static bool ps_b(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    return branch_to(c, cur, col, KSVM_COND_AL);
}

static bool ps_j(ksas_ctx *c, ksas_cursor *cur, uint32_t col)
{
    return sh_jump(c, cur, KSVM_OP_JMP, col);
}

typedef bool (*pseudo_fn)(ksas_ctx *c, ksas_cursor *cur, uint32_t col);

/* Pseudo-instruções (doc 04 §7) que não são mnemônicos da ISA. As demais
 * (not rd, push r1, r2, cmp ra, imm, subi grande...) são variações de
 * operandos tratadas nas formas acima. */
static const struct {
    const char *name;
    pseudo_fn   fn;
} pseudos[] = {
    { "li", ps_li }, { "la", ps_la }, { "fli", ps_fli }, { "clr", ps_clr },
    { "inc", ps_inc }, { "dec", ps_dec }, { "b", ps_b }, { "j", ps_j }
};

bool ksas_parse_insn(ksas_ctx *c, ksas_cursor *cur)
{
    ksas_token t = cur->tok;
    uint8_t op, cond;
    size_t i;

    ksas_advance(c, cur);
    for (i = 0; i < sizeof pseudos / sizeof pseudos[0]; i++) {
        if (ksas_name_eq(t.text, t.len, pseudos[i].name)) {
            return pseudos[i].fn(c, cur, t.col);
        }
    }
    if (ksas_isa_lookup(t.text, t.len, &op)) {
        return shape_fns[ksas_op_shape(op)](c, cur, op, t.col);
    }
    if (branch_cond(t.text, t.len, &cond)) {
        return branch_to(c, cur, t.col, cond);
    }
    return ksas_perr(c, t.col, "mnemonico desconhecido '%.*s'", (int)t.len, t.text);
}
