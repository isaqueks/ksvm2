/*
 * ksvm_config_user.h — configuração da KSVM para o ATmega328P (porta AVR).
 *
 * Incluído por include/ksvm/config.h quando KSVM_HAVE_USER_CONFIG está
 * definido (ports/avr/ vem no -I). É aqui, e não no núcleo, que
 * <avr/pgmspace.h> entra: o núcleo só conhece as macros KSVM_TABLE_*.
 *
 * Toda chave usa #ifndef para poder ser sobrescrita com -D na medição de
 * tamanho (mk/ports.mk, alvo size-avr).
 */
#ifndef KSVM_CONFIG_USER_H
#define KSVM_CONFIG_USER_H

#include <avr/pgmspace.h>

/* Tabela da ISA (768 bytes) e nomes na flash, lidos por LPM. */
#ifndef KSVM_TABLE_PROGMEM
#define KSVM_TABLE_PROGMEM 1
#endif
#if KSVM_TABLE_PROGMEM
#define KSVM_TABLE_ATTR          PROGMEM
#define KSVM_TABLE_READ_U8(p)    pgm_read_byte(p)
#define KSVM_TABLE_READ_PTR(p)   pgm_read_ptr(p)
#endif

/* Tabela de 1 byte por opcode (256 B em vez de 768 B de flash). */
#ifndef KSVM_ISA_COMPACT
#define KSVM_ISA_COMPACT 1
#endif

/* Código (ROM na flash), RAM do guest e uma região livre (MMIO). */
#ifndef KSVM_MAX_REGIONS
#define KSVM_MAX_REGIONS 3
#endif

/* Strings que o dispositivo não usa. */
#ifndef KSVM_ISA_NAMES
#define KSVM_ISA_NAMES 0
#endif
#ifndef KSVM_FAULT_NAMES
#define KSVM_FAULT_NAMES 0
#endif

/* MULH/MULHU com uint64_t. Medido (size-avr, variante no-int64): com
 * avr-gcc 7.3 a versão de 64 bits sai 54 bytes MENOR no total, porque a
 * libgcc usa __umulsidi3 (MUL de hardware) e o caminho de 16 bits do
 * núcleo precisa de quatro __mulsi3 + somas; e também é mais rápida. */
#ifndef KSVM_NO_INT64
#define KSVM_NO_INT64 0
#endif

#endif /* KSVM_CONFIG_USER_H */
