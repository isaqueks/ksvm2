/*
 * config.h — chaves de configuração da KSVM em tempo de compilação.
 *
 * Cada chave tem um padrão adequado ao desktop. O host sobrescreve com -D ou
 * fornecendo "ksvm_config_user.h" e definindo KSVM_HAVE_USER_CONFIG.
 *
 * IMPORTANTE: biblioteca e aplicação precisam ver a MESMA configuração, pois
 * algumas chaves (KSVM_MAX_REGIONS, KSVM_ENABLE_TRACE) mudam o layout de
 * struct ksvm.
 */
#ifndef KSVM_CONFIG_H
#define KSVM_CONFIG_H

#ifdef KSVM_HAVE_USER_CONFIG
#include "ksvm_config_user.h"
#endif

/* Regiões no barramento. Três bastam (código, RAM, MMIO); cada uma custa
 * 16 bytes no AVR e 40 bytes em hosts de 64 bits. */
#ifndef KSVM_MAX_REGIONS
#define KSVM_MAX_REGIONS 4
#endif

/* Hook chamado a cada instrução e em BRK. Desligado: zero custo. */
#ifndef KSVM_ENABLE_TRACE
#define KSVM_ENABLE_TRACE 0
#endif

/* MUL/MULI/MULH/MULHU/DIV/DIVU/REM/REMU. Desligar economiza flash no AVR;
 * os opcodes passam a gerar KSVM_FAULT_BAD_OPCODE. */
#ifndef KSVM_ENABLE_MULDIV
#define KSVM_ENABLE_MULDIV 1
#endif

/* CLZ/CTZ/POPCNT. Desligado: KSVM_FAULT_BAD_OPCODE. */
#ifndef KSVM_ENABLE_BITOPS
#define KSVM_ENABLE_BITOPS 1
#endif

/* Extensão F (doc 04 §5.7): FADD..FCMP sobre IEEE 754 binary32 guardado
 * nos registradores inteiros, em soft-float portável (softfloat.c, só
 * inteiros). Desligada: os opcodes 0xA0..0xB1 geram KSVM_FAULT_BAD_OPCODE
 * (continuam na tabela da ISA, para o assembler e o disassembler), CAPS
 * não anuncia F e softfloat.c compila vazio. A porta AVR desliga (flash;
 * doc 08 §4.2). */
#ifndef KSVM_ENABLE_FLOAT
#define KSVM_ENABLE_FLOAT 1
#endif

/* Grupo 3 de syscalls (arquivos). O núcleo só encaminha ao HAL; a chave
 * existe para que HALs saibam se devem implementá-lo. */
#ifndef KSVM_ENABLE_SYSCALL_FS
#define KSVM_ENABLE_SYSCALL_FS 0
#endif

/* PUSH/CALL/CALLR verificam sp - 4 >= stack_limit. */
#ifndef KSVM_CHECK_STACK
#define KSVM_CHECK_STACK 1
#endif

/* Strings com nomes de faults (ksvm_fault_name). */
#ifndef KSVM_FAULT_NAMES
#define KSVM_FAULT_NAMES 1
#endif

/* Strings com nomes de instruções e condições na tabela da ISA. */
#ifndef KSVM_ISA_NAMES
#define KSVM_ISA_NAMES 1
#endif

/* 1 = MULH/MULHU sem uint64_t (quatro produtos parciais de 16 bits), para
 * compiladores embarcados sem inteiros de 64 bits ou para poupar flash.
 * Vale também para FMUL/FDIV do soft-float (produto e quociente só com
 * inteiros de 32 bits). */
#ifndef KSVM_NO_INT64
#define KSVM_NO_INT64 0
#endif

/* Disassembler de uma instrução (disasm.c: ksvm_disasm_one, usado pelo
 * ksdis e pelo trace do runner). Sem stdio; numa biblioteca estática só
 * entra no binário se referenciado, por isso o padrão é 1 também em MCU.
 * 0 = disasm.c compila vazio e disasm.h não declara nada. */
#ifndef KSVM_ENABLE_DISASM
#define KSVM_ENABLE_DISASM 1
#endif

/*
 * Tabelas constantes (ISA, nomes) num espaço de endereços separado, como a
 * flash do AVR. O host que define KSVM_TABLE_PROGMEM=1 também define, no
 * seu ksvm_config_user.h (é lá, e não no núcleo, que <avr/pgmspace.h> é
 * incluído; ver ports/avr/ksvm_config_user.h):
 *   KSVM_TABLE_ATTR          atributo das tabelas (PROGMEM)
 *   KSVM_TABLE_READ_U8(p)    lê um uint8_t da tabela (pgm_read_byte)
 *   KSVM_TABLE_READ_PTR(p)   lê um ponteiro (pgm_read_ptr)
 * O núcleo só lê as tabelas por essas macros (ksvm_isa_get, nomes). Com
 * KSVM_TABLE_PROGMEM=1 os nomes devolvidos por ksvm_op_name, ksvm_cond_name
 * e ksvm_fault_name, se ligados, são endereços na flash (usar as funções
 * *_P da avr-libc), e ksvm_isa[] não pode ser lido diretamente.
 */
#ifndef KSVM_TABLE_PROGMEM
#define KSVM_TABLE_PROGMEM 0
#endif

/* 1 = tabela da ISA compacta para MCU: um byte por opcode (formato e
 * flags; len é derivado do formato), 256 bytes em vez de 768 no AVR. Nesse
 * modo ksvm_isa[] NÃO existe (só ksvm_isa_compact[]): leia a tabela por
 * ksvm_isa_get/ksvm_isa_format/ksvm_isa_len. Ferramentas de desktop usam o
 * padrão 0, em que ksvm_isa[] continua como sempre. */
#ifndef KSVM_ISA_COMPACT
#define KSVM_ISA_COMPACT 0
#endif
#ifndef KSVM_TABLE_ATTR
#define KSVM_TABLE_ATTR
#endif
#if KSVM_TABLE_PROGMEM
#if !defined(KSVM_TABLE_READ_U8) || !defined(KSVM_TABLE_READ_PTR)
#error "KSVM_TABLE_PROGMEM exige KSVM_TABLE_READ_U8/PTR"
#endif
#else
#ifndef KSVM_TABLE_READ_U8
#define KSVM_TABLE_READ_U8(p) (*(p))
#endif
#ifndef KSVM_TABLE_READ_PTR
#define KSVM_TABLE_READ_PTR(p) (*(p))
#endif
#endif

/* memcpy/memset substituíveis em ambientes sem libc. */
#ifndef KSVM_MEMCPY
#define KSVM_MEMCPY(dst, src, n) memcpy((dst), (src), (n))
#endif
#ifndef KSVM_MEMSET
#define KSVM_MEMSET(dst, c, n) memset((dst), (c), (n))
#endif

#if KSVM_MAX_REGIONS < 1 || KSVM_MAX_REGIONS > 255
#error "KSVM_MAX_REGIONS deve estar em 1..255"
#endif

#endif /* KSVM_CONFIG_H */
