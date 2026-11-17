/*
 * alu.c — operações de 32 bits com flags Z N C V (doc 04 §2, doc 08 §1).
 *
 * Tudo é feito em uint32_t: o wrap é definido pela norma, ao contrário do
 * overflow de int32_t. O sinal é sempre o bit 31; nenhum shift opera sobre
 * valor negativo e nenhuma divisão fica sem guarda.
 */
#include "ksvm/alu.h"
#include "ksvm/isa.h"

#define SIGN_BIT UINT32_C(0x80000000)

uint32_t ksvm_alu_zn(uint32_t res, uint32_t *flags)
{
    uint32_t f = *flags & ~(KSVM_FLAG_Z | KSVM_FLAG_N);

    if (res == 0u) {
        f |= KSVM_FLAG_Z;
    }
    if ((res & SIGN_BIT) != 0u) {
        f |= KSVM_FLAG_N;
    }
    *flags = f;
    return res;
}

/* Z, N e C; preserva V. */
static uint32_t set_znc(uint32_t res, bool c, uint32_t *flags)
{
    ksvm_alu_zn(res, flags);
    *flags = c ? (*flags | KSVM_FLAG_C) : (*flags & ~KSVM_FLAG_C);
    return res;
}

/* Z, N, C e V. */
static uint32_t set_zncv(uint32_t res, bool c, bool v, uint32_t *flags)
{
    set_znc(res, c, flags);
    *flags = v ? (*flags | KSVM_FLAG_V) : (*flags & ~KSVM_FLAG_V);
    return res;
}

/* ---- Soma e subtração ------------------------------------------------- */

/*
 * a + b + cin com carry de 33 bits. O carry sai se qualquer das duas somas
 * parciais der a volta (no máximo uma delas pode). V: a e b com o mesmo
 * sinal e resultado com sinal diferente (vale também com carry de entrada).
 *
 * As quatro operações passam por aqui (um só corpo: no AVR isso poupa
 * ~300 bytes de flash). Subtração é a + ~b + 1 em complemento de dois; o
 * carry de saída é então o "não-borrow" da convenção ARM (C = a >= b) e o
 * V da soma com ~b coincide com o V da subtração ((a ^ b) & (a ^ res)).
 */
static uint32_t add_with_carry(uint32_t a, uint32_t b, uint32_t cin, uint32_t *flags)
{
    uint32_t partial = a + b;
    uint32_t res = partial + cin;
    bool c = (partial < a) || (res < partial);
    bool v = (((a ^ res) & (b ^ res)) >> 31) != 0u;
    return set_zncv(res, c, v, flags);
}

uint32_t ksvm_alu_add(uint32_t a, uint32_t b, uint32_t *flags)
{
    return add_with_carry(a, b, 0u, flags);
}

uint32_t ksvm_alu_sub(uint32_t a, uint32_t b, uint32_t *flags)
{
    return add_with_carry(a, ~b, 1u, flags);
}

static uint32_t carry_in(const uint32_t *flags)
{
    return ((*flags & KSVM_FLAG_C) != 0u) ? 1u : 0u;
}

uint32_t ksvm_alu_adc(uint32_t a, uint32_t b, uint32_t *flags)
{
    return add_with_carry(a, b, carry_in(flags), flags);
}

/*
 * a - b - (1 - C) == a + ~b + C em complemento de dois. Assim o carry de
 * saída já é o "não-borrow" da convenção ARM, e o V da soma com ~b coincide
 * com o V da subtração ((a ^ b) & (a ^ res)).
 */
uint32_t ksvm_alu_sbc(uint32_t a, uint32_t b, uint32_t *flags)
{
    return add_with_carry(a, ~b, carry_in(flags), flags);
}

/* ---- Deslocamentos e rotações ----------------------------------------- */
/* Contagem mascarada com & 31 (shift de uint32_t por 32 seria UB). Com
 * contagem 0 o valor não muda e C = 0 (nenhum bit saiu).
 *
 * Um só corpo para as cinco operações (no AVR cada shift variável de 32
 * bits é um laço; um corpo só poupa ~250 bytes de flash). C é o último bit
 * que saiu: à esquerda é o bit 32 - n de a, que também é o bit 0 do ROL; à
 * direita é o bit n - 1 de a, que também é o bit 31 do ROR. */

#define SHIFT_LEFT   1u
#define SHIFT_ROTATE 2u
#define SHIFT_ARITH  4u

static uint32_t shift(uint32_t a, uint32_t n, unsigned kind, uint32_t *flags)
{
    uint32_t res;
    bool c;

    n &= 31u;
    if (n == 0u) {
        return set_znc(a, false, flags);
    }
    if ((kind & SHIFT_LEFT) != 0u) {
        uint32_t out = a >> (32u - n);          /* bits que saem pelo topo */
        res = a << n;
        c = (out & 1u) != 0u;
        if ((kind & SHIFT_ROTATE) != 0u) {
            res |= out;
        }
    } else {
        res = a >> n;
        c = ((a >> (n - 1u)) & 1u) != 0u;
        if ((kind & SHIFT_ROTATE) != 0u) {
            res |= a << (32u - n);
        } else if ((kind & SHIFT_ARITH) != 0u && (a & SIGN_BIT) != 0u) {
            /* ">>" em int32_t negativo é implementation-defined: replica o
             * bit 31 nos n bits altos com uma máscara explícita. */
            res |= ~(UINT32_C(0xFFFFFFFF) >> n);
        }
    }
    return set_znc(res, c, flags);
}

uint32_t ksvm_alu_shl(uint32_t a, uint32_t n, uint32_t *flags)
{
    return shift(a, n, SHIFT_LEFT, flags);
}

uint32_t ksvm_alu_shr(uint32_t a, uint32_t n, uint32_t *flags)
{
    return shift(a, n, 0u, flags);
}

uint32_t ksvm_alu_sar(uint32_t a, uint32_t n, uint32_t *flags)
{
    return shift(a, n, SHIFT_ARITH, flags);
}

/* ROL: o último bit a sair pelo topo reentra no bit 0. */
uint32_t ksvm_alu_rol(uint32_t a, uint32_t n, uint32_t *flags)
{
    return shift(a, n, SHIFT_LEFT | SHIFT_ROTATE, flags);
}

/* ROR: o último bit a sair por baixo reentra no bit 31. */
uint32_t ksvm_alu_ror(uint32_t a, uint32_t n, uint32_t *flags)
{
    return shift(a, n, SHIFT_ROTATE, flags);
}

/* ---- Multiplicação ---------------------------------------------------- */

/* "1u *" força a multiplicação em aritmética sem sinal mesmo num host onde
 * int tenha mais de 32 bits (lá uint32_t seria promovido a int, e o produto
 * poderia estourar = UB). */
uint32_t ksvm_alu_mul(uint32_t a, uint32_t b)
{
    return (uint32_t)(1u * a * b);
}

#if KSVM_NO_INT64
/* Quatro produtos parciais de 16x16 bits, cada um < 2^32. */
uint32_t ksvm_alu_mulhu(uint32_t a, uint32_t b)
{
    uint32_t al = a & 0xFFFFu, ah = a >> 16;
    uint32_t bl = b & 0xFFFFu, bh = b >> 16;
    uint32_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint32_t mid = (ll >> 16) + (lh & 0xFFFFu) + (hl & 0xFFFFu); /* < 3 * 2^16 */

    return hh + (lh >> 16) + (hl >> 16) + (mid >> 16);
}
#else
uint32_t ksvm_alu_mulhu(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a * b) >> 32);
}
#endif

/*
 * Parte alta com sinal derivada da sem sinal, sem aritmética com sinal:
 * como (int)x = x - 2^32 * [x < 0], os 32 bits altos do produto com sinal
 * são mulhu(a, b) - [a < 0] * b - [b < 0] * a (mod 2^32).
 */
uint32_t ksvm_alu_mulh(uint32_t a, uint32_t b)
{
    uint32_t hi = ksvm_alu_mulhu(a, b);

    if ((a & SIGN_BIT) != 0u) {
        hi -= b;
    }
    if ((b & SIGN_BIT) != 0u) {
        hi -= a;
    }
    return hi;
}

/* ---- Divisão ---------------------------------------------------------- */
/* Com sinal: divide as magnitudes (uint32_t) e aplica o sinal depois, o que
 * dá truncamento para zero sem "/" sobre negativos. */

static uint32_t magnitude(uint32_t x)
{
    return ((x & SIGN_BIT) != 0u) ? (uint32_t)(0u - x) : x;
}

bool ksvm_alu_div(uint32_t a, uint32_t b, uint32_t *out)
{
    uint32_t q;

    if (b == 0u) {
        return false;
    }
    if (a == SIGN_BIT && b == UINT32_C(0xFFFFFFFF)) {
        *out = SIGN_BIT;      /* INT32_MIN / -1 não cabe: definido como INT32_MIN */
        return true;
    }
    q = magnitude(a) / magnitude(b);
    *out = (((a ^ b) & SIGN_BIT) != 0u) ? (uint32_t)(0u - q) : q;
    return true;
}

bool ksvm_alu_divu(uint32_t a, uint32_t b, uint32_t *out)
{
    if (b == 0u) {
        return false;
    }
    *out = a / b;
    return true;
}

/* Resto com o sinal do dividendo; INT32_MIN rem -1 dá 0 pela magnitude. */
bool ksvm_alu_rem(uint32_t a, uint32_t b, uint32_t *out)
{
    uint32_t r;

    if (b == 0u) {
        return false;
    }
    r = magnitude(a) % magnitude(b);
    *out = ((a & SIGN_BIT) != 0u) ? (uint32_t)(0u - r) : r;
    return true;
}

bool ksvm_alu_remu(uint32_t a, uint32_t b, uint32_t *out)
{
    if (b == 0u) {
        return false;
    }
    *out = a % b;
    return true;
}

/* ---- Contagem de bits (portável, sem builtins) ------------------------ */

/* Busca binária: no máximo 5 passos. */
uint32_t ksvm_alu_clz(uint32_t a)
{
    uint32_t n = 0u;

    if (a == 0u) {
        return 32u;
    }
    if ((a & UINT32_C(0xFFFF0000)) == 0u) { n += 16u; a <<= 16; }
    if ((a & UINT32_C(0xFF000000)) == 0u) { n += 8u;  a <<= 8;  }
    if ((a & UINT32_C(0xF0000000)) == 0u) { n += 4u;  a <<= 4;  }
    if ((a & UINT32_C(0xC0000000)) == 0u) { n += 2u;  a <<= 2;  }
    if ((a & SIGN_BIT) == 0u)             { n += 1u; }
    return n;
}

/* a & (0 - a) isola o bit 1 mais baixo (posição k): clz dele = 31 - k.
 * Reaproveita clz em vez de uma segunda busca binária (flash no AVR). */
uint32_t ksvm_alu_ctz(uint32_t a)
{
    if (a == 0u) {
        return 32u;
    }
    return 31u - ksvm_alu_clz(a & (0u - a));
}

/* Soma paralela (SWAR); a soma final dos bytes evita multiplicação. */
uint32_t ksvm_alu_popcnt(uint32_t a)
{
    a = a - ((a >> 1) & UINT32_C(0x55555555));
    a = (a & UINT32_C(0x33333333)) + ((a >> 2) & UINT32_C(0x33333333));
    a = (a + (a >> 4)) & UINT32_C(0x0F0F0F0F);
    return (a + (a >> 8) + (a >> 16) + (a >> 24)) & 0x3Fu;
}
