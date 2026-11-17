/*
 * loader.h — formato .ksx (doc 05) e carregamento da imagem na VM.
 *
 * Cabeçalho fixo de 64 bytes, todos os campos little-endian, lidos byte a
 * byte (nunca memcpy para struct: padding e endianness variam por host).
 *
 * CHECKSUM: Adler-32 padrão (RFC 1950: A = 1, B = 0, módulo 65521; valor
 * = B << 16 | A) sobre os bytes [0x38, fim do arquivo). Escolhido em vez de
 * CRC32 porque não precisa de tabela (bom para AVR) e, com subtração
 * condicional em vez de "%", não usa divisão.
 */
#ifndef KSVM_LOADER_H
#define KSVM_LOADER_H

#include "bus.h"
#include "vm.h"

#define KSVM_KSX_HEADER_SIZE    64u
#define KSVM_KSX_VERSION_MAJOR  2u
#define KSVM_KSX_VERSION_MINOR  0u

/* Bits do campo flags. */
#define KSVM_KSX_FLAG_PIC   0x0001u
#define KSVM_KSX_FLAG_SYMS  0x0002u

/* Offsets dos campos no cabeçalho. */
#define KSVM_KSX_OFF_MAGIC      0x00u
#define KSVM_KSX_OFF_VER_MAJOR  0x04u
#define KSVM_KSX_OFF_VER_MINOR  0x05u
#define KSVM_KSX_OFF_FLAGS      0x06u
#define KSVM_KSX_OFF_ENTRY      0x08u
#define KSVM_KSX_OFF_CODE_OFF   0x0Cu
#define KSVM_KSX_OFF_CODE_SIZE  0x10u
#define KSVM_KSX_OFF_DATA_OFF   0x14u
#define KSVM_KSX_OFF_DATA_SIZE  0x18u
#define KSVM_KSX_OFF_BSS_SIZE   0x1Cu
#define KSVM_KSX_OFF_RAM_BASE   0x20u
#define KSVM_KSX_OFF_RAM_MIN    0x24u
#define KSVM_KSX_OFF_STACK_SIZE 0x28u
#define KSVM_KSX_OFF_SYM_OFF    0x2Cu
#define KSVM_KSX_OFF_SYM_SIZE   0x30u
#define KSVM_KSX_OFF_CHECKSUM   0x34u
#define KSVM_KSX_CHECKSUM_START 0x38u

/* Cabeçalho decodificado (campos do doc 05 §2). */
typedef struct ksvm_image_header {
    uint8_t  ver_major;
    uint8_t  ver_minor;
    uint16_t flags;
    uint32_t entry;
    uint32_t code_off,  code_size;
    uint32_t data_off,  data_size;
    uint32_t bss_size;
    uint32_t ram_base;
    uint32_t ram_min;
    uint32_t stack_size;
    uint32_t sym_off,   sym_size;
    uint32_t checksum;
} ksvm_image_header;

#define KSVM_ADLER32_INIT UINT32_C(1)

/* Acumula um byte no Adler-32. */
uint32_t ksvm_adler32_update(uint32_t adler, uint8_t byte);

/*
 * Lê e valida o cabeçalho: magic, versão major, checksum, faixas das seções
 * dentro do arquivo e entry (alinhado, dentro do código). read8 NULL =
 * imagem acessível diretamente; senão todo byte é lido por read8 (flash).
 */
int ksvm_image_parse(const uint8_t *image, uint32_t image_size,
                     ksvm_read8_fn read8, ksvm_image_header *hdr);

/*
 * Carrega a imagem (doc 05 §3): valida, mapeia CODE em 0 como ROM apontando
 * para dentro de "image" (sem cópia; o buffer deve viver tanto quanto a VM),
 * mapeia a RAM, copia .data, zera .bss, define stack_limit e heap_start e
 * chama ksvm_reset(entry, sp). Regiões já mapeadas (ex.: MMIO) são mantidas.
 * Erros adicionais: KSVM_E_NO_RAM, KSVM_E_OVERLAP, KSVM_E_FULL.
 */
int ksvm_load_image(ksvm *vm, const uint8_t *image, uint32_t image_size,
                    uint8_t *ram, uint32_t ram_size);

/* Idem, lendo cabeçalho, checksum, código e .data por read8 (Harvard). */
int ksvm_load_image_ex(ksvm *vm, const uint8_t *image, uint32_t image_size,
                       uint8_t *ram, uint32_t ram_size, ksvm_read8_fn read8);

#endif /* KSVM_LOADER_H */
