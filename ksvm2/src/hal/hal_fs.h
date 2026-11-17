/*
 * hal_fs.h — grupo 3 de syscalls (OPEN/CLOSE/READ/WRITE/SEEK, doc 06 §3)
 * independente de SO, compartilhado por hal_posix e hal_win32. Interno às
 * HALs de desktop (não faz parte da API pública).
 *
 * Este módulo lê os argumentos do guest, valida faixas de memória e o
 * caminho, copia dados em blocos por um buffer próprio (nunca entrega ao
 * host ponteiro cru para a RAM do guest) e chama o backend do SO, que só
 * conhece descritores de 0 a KSVM_FS_MAX_FDS - 1 e buffers do host.
 *
 * Política de caminho (OPEN): relativo ao diretório atual, até
 * KSVM_FS_PATH_MAX bytes, sem '\0', sem começar por '/' ou '\\', sem ':'
 * (letra de unidade, fluxos NTFS), sem componente ".." e sem componente que
 * seja nome de dispositivo do Windows (CON, PRN, AUX, NUL, COM0-9, LPT0-9,
 * CONIN$, CONOUT$; com ou sem extensão, sem diferenciar maiúsculas). Essa
 * última regra vale em todos os hosts: o mesmo programa do guest tem o mesmo
 * resultado no POSIX e no Windows. Não é um sandbox completo (links
 * simbólicos são seguidos), apenas impede os casos óbvios.
 */
#ifndef KSVM_HAL_FS_H
#define KSVM_HAL_FS_H

#include "ksvm/syscall.h"

#define KSVM_FS_MAX_FDS  16u     /* 0, 1, 2 = stdin, stdout, stderr */
#define KSVM_FS_PATH_MAX 255u
#define KSVM_FS_CHUNK    4096u   /* bytes por chamada ao backend */

/* Flags de OPEN (r3). */
#define KSVM_FS_RD     0x1u
#define KSVM_FS_WR     0x2u
#define KSVM_FS_CREAT  0x4u
#define KSVM_FS_APPEND 0x8u
#define KSVM_FS_FLAGS_ALL 0xFu

/* whence de SEEK (r3). */
#define KSVM_FS_SEEK_SET 0u
#define KSVM_FS_SEEK_CUR 1u
#define KSVM_FS_SEEK_END 2u

/*
 * Backend do SO. Toda função devolve KSVM_ERR_OK ou um KSVM_ERR_* (sempre
 * positivo). fd já foi validado (< KSVM_FS_MAX_FDS); "aberto ou não" é
 * estado do backend. read/write podem transferir menos que len (*done).
 */
typedef struct ksvm_fs_ops {
    /* Abre path num descritor livre (>= 3) e o devolve em *fd. */
    uint32_t (*open)(void *ctx, const char *path, uint32_t flags, uint32_t *fd);
    uint32_t (*close)(void *ctx, uint32_t fd);
    uint32_t (*read)(void *ctx, uint32_t fd, uint8_t *buf, uint32_t len, uint32_t *done);
    uint32_t (*write)(void *ctx, uint32_t fd, const uint8_t *buf, uint32_t len,
                      uint32_t *done);
    /* off com sinal (int32 em complemento de dois); *pos = nova posição. */
    uint32_t (*seek)(void *ctx, uint32_t fd, int32_t off, uint32_t whence,
                     uint32_t *pos);
} ksvm_fs_ops;

/* Trata num (0x30..0x34) com o backend; KSVM_SYS_UNKNOWN para os demais. */
int ksvm_fs_syscall(ksvm *vm, uint32_t num, const ksvm_fs_ops *ops, void *ctx);

/* Política de caminho descrita acima (exposta para testes). */
bool ksvm_fs_path_ok(const char *path);

#endif /* KSVM_HAL_FS_H */
