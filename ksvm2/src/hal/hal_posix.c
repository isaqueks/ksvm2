/*
 * hal_posix.c — HAL POSIX (ver hal_posix.h).
 *
 * Só chamadas POSIX.1-2008: read/write/poll/open/close/lseek,
 * clock_gettime e nanosleep. Nenhum ponteiro para a RAM do guest chega
 * aqui: o grupo 3 passa por hal_fs.c, que copia por um buffer local.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "hal_fs.h"
#include "ksvm/hal_posix.h"

/* A tabela de fds da porta tem o tamanho que hal_fs.c valida. */
typedef char ksvm_posix_fds_check[(KSVM_HAL_POSIX_MAX_FDS == KSVM_FS_MAX_FDS) ? 1 : -1];

/* ---- Utilitários ------------------------------------------------------ */

static uint64_t now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) + (uint64_t)ts.tv_nsec / UINT64_C(1000);
}

/* write(2) completo, repetindo em EINTR e escrita parcial. */
static bool write_all(int fd, const uint8_t *buf, size_t len)
{
    while (len > 0u) {
        ssize_t r = write(fd, buf, len);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        buf += (size_t)r;
        len -= (size_t)r;
    }
    return true;
}

static ssize_t read_retry(int fd, uint8_t *buf, size_t len)
{
    ssize_t r;

    do {
        r = read(fd, buf, len);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* Há algo para ler (ou EOF/erro, que read(2) reporta sem bloquear)? */
static bool input_ready(int fd)
{
    struct pollfd p;

    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1u, 0) > 0 && p.revents != 0;
}

void ksvm_hal_posix_flush(ksvm_hal_posix *hp)
{
    if (hp->out_len != 0u && !write_all(hp->out_fd, hp->out, hp->out_len)) {
        hp->out_error = true;
    }
    hp->out_len = 0u;
}

/* ---- Console e tempo -------------------------------------------------- */

static int posix_putc(void *ctx, uint8_t c)
{
    ksvm_hal_posix *hp = (ksvm_hal_posix *)ctx;

    hp->out[hp->out_len] = c;
    hp->out_len++;
    if (c == '\n' || hp->out_len == KSVM_HAL_POSIX_OUT_BUF) {
        ksvm_hal_posix_flush(hp);
    }
    return hp->out_error ? -1 : 0;
}

static int posix_getc(void *ctx)
{
    ksvm_hal_posix *hp = (ksvm_hal_posix *)ctx;
    uint8_t c = 0u;
    ssize_t r;

    ksvm_hal_posix_flush(hp);                  /* prompt visível antes de ler */
    if (hp->in_eof) {
        return KSVM_HAL_EOF;
    }
    if (!hp->stdin_block && !input_ready(hp->in_fd)) {
        return KSVM_HAL_NODATA;
    }
    r = read_retry(hp->in_fd, &c, 1u);
    if (r == 1) {
        return c;
    }
    if (r == 0) {
        hp->in_eof = true;
        return KSVM_HAL_EOF;
    }
    return KSVM_HAL_NODATA;                    /* EAGAIN ou erro transitório */
}

static uint32_t posix_millis(void *ctx)
{
    const ksvm_hal_posix *hp = (const ksvm_hal_posix *)ctx;

    return (uint32_t)(((now_us() - hp->t0_us) / UINT64_C(1000)) & UINT32_C(0xFFFFFFFF));
}

static void posix_sleep_ms(void *ctx, uint32_t ms)
{
    struct timespec req, rem;

    ksvm_hal_posix_flush((ksvm_hal_posix *)ctx);
    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) {
        req = rem;
    }
}

static void posix_on_halt(void *ctx, uint32_t code)
{
    (void)code;
    ksvm_hal_posix_flush((ksvm_hal_posix *)ctx);
}

/* ---- Grupo 3: backend de arquivos ------------------------------------- */

#if KSVM_ENABLE_SYSCALL_FS

static int open_flags(uint32_t f)
{
    int o;

    if ((f & KSVM_FS_RD) != 0u && (f & KSVM_FS_WR) != 0u) {
        o = O_RDWR;
    } else if ((f & KSVM_FS_WR) != 0u) {
        o = O_WRONLY;
    } else {
        o = O_RDONLY;
    }
    if ((f & KSVM_FS_CREAT) != 0u) {
        o |= O_CREAT;
    }
    if ((f & KSVM_FS_APPEND) != 0u) {
        o |= O_APPEND;
    }
    return o;
}

static uint32_t fs_open(void *ctx, const char *path, uint32_t flags, uint32_t *fd)
{
    ksvm_hal_posix *hp = (ksvm_hal_posix *)ctx;
    uint32_t slot;
    int h;

    for (slot = 3u; slot < KSVM_HAL_POSIX_MAX_FDS && hp->fds[slot] >= 0; slot++) {
    }
    if (slot == KSVM_HAL_POSIX_MAX_FDS) {
        return KSVM_ERR_IO;                    /* sem descritor livre */
    }
    do {
        h = open(path, open_flags(flags), 0666);
    } while (h < 0 && errno == EINTR);
    if (h < 0) {
        return KSVM_ERR_IO;
    }
    hp->fds[slot] = h;
    *fd = slot;
    return KSVM_ERR_OK;
}

/* fd 0/1/2 só são desassociados: o host continua com seu stdin/stdout. */
static uint32_t fs_close(void *ctx, uint32_t fd)
{
    ksvm_hal_posix *hp = (ksvm_hal_posix *)ctx;
    int h = hp->fds[fd];

    if (h < 0) {
        return KSVM_ERR_INVAL;
    }
    hp->fds[fd] = -1;
    if (fd >= 3u && close(h) != 0 && errno != EINTR) {
        return KSVM_ERR_IO;
    }
    return KSVM_ERR_OK;
}

static uint32_t fs_read(void *ctx, uint32_t fd, uint8_t *buf, uint32_t len, uint32_t *done)
{
    ksvm_hal_posix *hp = (ksvm_hal_posix *)ctx;
    ssize_t r;

    if (hp->fds[fd] < 0) {
        return KSVM_ERR_INVAL;
    }
    ksvm_hal_posix_flush(hp);
    r = read_retry(hp->fds[fd], buf, (size_t)len);
    if (r < 0) {
        return KSVM_ERR_IO;
    }
    *done = (uint32_t)r;
    return KSVM_ERR_OK;
}

static uint32_t fs_write(void *ctx, uint32_t fd, const uint8_t *buf, uint32_t len,
                         uint32_t *done)
{
    ksvm_hal_posix *hp = (ksvm_hal_posix *)ctx;

    if (hp->fds[fd] < 0) {
        return KSVM_ERR_INVAL;
    }
    ksvm_hal_posix_flush(hp);                  /* mantém a ordem com PUTC/PUTS */
    if (!write_all(hp->fds[fd], buf, (size_t)len)) {
        return KSVM_ERR_IO;
    }
    *done = len;
    return KSVM_ERR_OK;
}

static uint32_t fs_seek(void *ctx, uint32_t fd, int32_t off, uint32_t whence, uint32_t *pos)
{
    static const int how[3] = { SEEK_SET, SEEK_CUR, SEEK_END };
    ksvm_hal_posix *hp = (ksvm_hal_posix *)ctx;
    off_t r;

    if (hp->fds[fd] < 0) {
        return KSVM_ERR_INVAL;
    }
    r = lseek(hp->fds[fd], (off_t)off, how[whence]);
    if (r < 0) {
        return (errno == EINVAL) ? KSVM_ERR_INVAL : KSVM_ERR_IO;
    }
    if ((uint64_t)r > UINT32_C(0xFFFFFFFF)) {
        return KSVM_ERR_INVAL;                 /* posição não cabe em 32 bits */
    }
    *pos = (uint32_t)r;
    return KSVM_ERR_OK;
}

static const ksvm_fs_ops posix_fs_ops = { fs_open, fs_close, fs_read, fs_write, fs_seek };

#endif /* KSVM_ENABLE_SYSCALL_FS */

/* ---- Syscalls do host ------------------------------------------------- */

static int posix_syscall(ksvm *vm, uint32_t num)
{
    ksvm_hal_posix *hp = (ksvm_hal_posix *)vm->hal->ctx;

    if (num == KSVM_SYSCALL_MICROS) {
        ksvm_ret(vm, (uint32_t)((now_us() - hp->t0_us) & UINT32_C(0xFFFFFFFF)), KSVM_ERR_OK);
        return KSVM_SYS_OK;
    }
#if KSVM_ENABLE_SYSCALL_FS
    if (hp->fs_enabled) {
        return ksvm_fs_syscall(vm, num, &posix_fs_ops, hp);
    }
#endif
    return KSVM_SYS_UNKNOWN;
}

/* ---- API -------------------------------------------------------------- */

void ksvm_hal_posix_init(ksvm_hal_posix *hp, ksvm_hal *hal)
{
    uint32_t i;

    hp->in_fd = 0;
    hp->out_fd = 1;
    hp->stdin_block = false;
    hp->fs_enabled = true;
    hp->in_eof = false;
    hp->out_error = false;
    hp->t0_us = now_us();
    hp->out_len = 0u;
    for (i = 0; i < KSVM_HAL_POSIX_MAX_FDS; i++) {
        hp->fds[i] = (i < 3u) ? (int)i : -1;
    }
    hal->ctx = hp;
    hal->putc = posix_putc;
    hal->getc = posix_getc;
    hal->millis = posix_millis;
    hal->sleep_ms = posix_sleep_ms;
    hal->syscall = posix_syscall;
    hal->on_halt = posix_on_halt;
}

void ksvm_hal_posix_close_all(ksvm_hal_posix *hp)
{
    uint32_t i;

    ksvm_hal_posix_flush(hp);
    for (i = 3u; i < KSVM_HAL_POSIX_MAX_FDS; i++) {
        if (hp->fds[i] >= 0) {
            (void)close(hp->fds[i]);
            hp->fds[i] = -1;
        }
    }
}
