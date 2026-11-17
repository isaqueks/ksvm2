/*
 * hal_fs.c — grupo 3 de syscalls sobre um backend de SO (ver hal_fs.h).
 *
 * Convenção do doc 06: r1 = resultado (-1 em erro), r2 = KSVM_ERR_*. Toda
 * faixa do guest é validada inteira antes da primeira chamada ao SO; os
 * dados passam por um buffer local de KSVM_FS_CHUNK bytes.
 */
#include <string.h>

#include "hal_fs.h"
#include "ksvm/bytes.h"

static void ret_err(ksvm *vm, uint32_t err)
{
    ksvm_ret(vm, KSVM_SYS_RET_ERROR, err);
}

static void ret_result(ksvm *vm, uint32_t err, uint32_t value)
{
    if (err == KSVM_ERR_OK) {
        ksvm_ret(vm, value, KSVM_ERR_OK);
    } else {
        ret_err(vm, err);
    }
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

/* ---- Caminho ---------------------------------------------------------- */

/* Componente só de '.' e ' ' com 2+ caracteres: ".." (e variantes que o
 * Windows normaliza para "..", como ". ." ou "..."). */
static bool dotdot_like(const char *s, size_t len)
{
    size_t i, dots = 0u;

    for (i = 0; i < len; i++) {
        if (s[i] == '.') {
            dots++;
        } else if (s[i] != ' ') {
            return false;
        }
    }
    return len >= 2u && dots >= 2u;
}

static char ascii_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* Nome de dispositivo reservado do Windows: CON, PRN, AUX, NUL, COM0..9,
 * LPT0..9, CONIN$, CONOUT$, sem diferenciar maiúsculas e com ou sem
 * extensão. O Windows olha só o nome antes do primeiro '.', sem espaços no
 * fim: "nul.txt", "Con " e "aux.tar.gz" abrem o dispositivo, não um arquivo.
 * (COM0/LPT0 também são reservados segundo a documentação da Microsoft.) */
static bool device_name(const char *s, size_t len)
{
    static const char *const names[] = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };
    char base[8];
    size_t n = 0u, i;

    while (n < len && s[n] != '.') {
        n++;
    }
    while (n > 0u && s[n - 1u] == ' ') {
        n--;
    }
    if (n < 3u || n >= sizeof base) {
        return false;
    }
    for (i = 0; i < n; i++) {
        base[i] = ascii_upper(s[i]);
    }
    base[n] = '\0';
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strcmp(base, names[i]) == 0) {
            return true;
        }
    }
    return n == 4u && (memcmp(base, "COM", 3u) == 0 || memcmp(base, "LPT", 3u) == 0) &&
           base[3] >= '0' && base[3] <= '9';
}

bool ksvm_fs_path_ok(const char *path)
{
    size_t i, start = 0u;

    if (path[0] == '\0' || path[0] == '/' || path[0] == '\\') {
        return false;
    }
    for (i = 0;; i++) {
        char c = path[i];
        if (c == ':') {
            return false;
        }
        if (c == '/' || c == '\\' || c == '\0') {
            if (dotdot_like(path + start, i - start) ||
                device_name(path + start, i - start)) {
                return false;
            }
            if (c == '\0') {
                return true;
            }
            start = i + 1u;
        }
    }
}

/* ---- Syscalls --------------------------------------------------------- */

static bool fd_valid(uint32_t fd)
{
    return fd < KSVM_FS_MAX_FDS;
}

static bool open_flags_ok(uint32_t flags)
{
    if ((flags & ~KSVM_FS_FLAGS_ALL) != 0u || (flags & (KSVM_FS_RD | KSVM_FS_WR)) == 0u) {
        return false;
    }
    return (flags & KSVM_FS_APPEND) == 0u || (flags & KSVM_FS_WR) != 0u;
}

/* r1 = ptr do caminho, r2 = len, r3 = flags -> r1 = fd. */
static void fs_open(ksvm *vm, const ksvm_fs_ops *ops, void *ctx)
{
    char path[KSVM_FS_PATH_MAX + 1u];
    uint32_t ptr = ksvm_arg(vm, 1u), len = ksvm_arg(vm, 2u), flags = ksvm_arg(vm, 3u);
    uint32_t fd = 0u, err;

    if (len == 0u || len > KSVM_FS_PATH_MAX || !open_flags_ok(flags)) {
        ret_err(vm, KSVM_ERR_INVAL);
        return;
    }
    if (ksvm_copy_from_guest(vm, path, ptr, len) != KSVM_OK) {
        ret_err(vm, KSVM_ERR_FAULT);
        return;
    }
    path[len] = '\0';
    if (strlen(path) != (size_t)len || !ksvm_fs_path_ok(path)) {
        ret_err(vm, KSVM_ERR_INVAL);
        return;
    }
    err = ops->open(ctx, path, flags, &fd);
    ret_result(vm, err, fd);
}

/* r1 = fd. */
static void fs_close(ksvm *vm, const ksvm_fs_ops *ops, void *ctx)
{
    uint32_t fd = ksvm_arg(vm, 1u);

    ret_result(vm, fd_valid(fd) ? ops->close(ctx, fd) : KSVM_ERR_INVAL, 0u);
}

/* r1 = fd, r2 = ptr, r3 = len -> bytes lidos (0 = fim do arquivo). */
static void fs_read(ksvm *vm, const ksvm_fs_ops *ops, void *ctx)
{
    uint8_t buf[KSVM_FS_CHUNK];
    uint32_t fd = ksvm_arg(vm, 1u), ptr = ksvm_arg(vm, 2u), len = ksvm_arg(vm, 3u);
    uint32_t total = 0u, err = KSVM_ERR_OK;

    if (!fd_valid(fd)) {
        ret_err(vm, KSVM_ERR_INVAL);
        return;
    }
    if (len != 0u && ksvm_bus_check(&vm->bus, ptr, len, KSVM_MEM_WRITE) != KSVM_FAULT_NONE) {
        ret_err(vm, KSVM_ERR_FAULT);
        return;
    }
    while (total < len) {
        uint32_t want = min_u32(len - total, KSVM_FS_CHUNK), got = 0u;
        err = ops->read(ctx, fd, buf, want, &got);
        if (err != KSVM_ERR_OK || got == 0u) {
            break;
        }
        (void)ksvm_copy_to_guest(vm, ptr + total, buf, got);  /* faixa já validada */
        total += got;
        if (got < want) {
            break;                              /* leitura curta: não bloquear de novo */
        }
    }
    ret_result(vm, (total != 0u) ? KSVM_ERR_OK : err, total);
}

/* r1 = fd, r2 = ptr, r3 = len -> bytes escritos. */
static void fs_write(ksvm *vm, const ksvm_fs_ops *ops, void *ctx)
{
    uint8_t buf[KSVM_FS_CHUNK];
    uint32_t fd = ksvm_arg(vm, 1u), ptr = ksvm_arg(vm, 2u), len = ksvm_arg(vm, 3u);
    uint32_t total = 0u, err = KSVM_ERR_OK;

    if (!fd_valid(fd)) {
        ret_err(vm, KSVM_ERR_INVAL);
        return;
    }
    if (len != 0u && ksvm_bus_check(&vm->bus, ptr, len, KSVM_MEM_READ) != KSVM_FAULT_NONE) {
        ret_err(vm, KSVM_ERR_FAULT);
        return;
    }
    while (total < len) {
        uint32_t want = min_u32(len - total, KSVM_FS_CHUNK), put = 0u;
        (void)ksvm_copy_from_guest(vm, buf, ptr + total, want);
        err = ops->write(ctx, fd, buf, want, &put);
        if (err != KSVM_ERR_OK) {
            break;
        }
        total += put;
        if (put < want) {
            break;
        }
    }
    ret_result(vm, (total != 0u) ? KSVM_ERR_OK : err, total);
}

/* r1 = fd, r2 = offset (com sinal), r3 = whence -> nova posição. */
static void fs_seek(ksvm *vm, const ksvm_fs_ops *ops, void *ctx)
{
    uint32_t fd = ksvm_arg(vm, 1u), whence = ksvm_arg(vm, 3u), pos = 0u, err;
    int32_t off = ksvm_u32_to_i32(ksvm_arg(vm, 2u));

    if (!fd_valid(fd) || whence > KSVM_FS_SEEK_END) {
        err = KSVM_ERR_INVAL;
    } else {
        err = ops->seek(ctx, fd, off, whence, &pos);
    }
    ret_result(vm, err, pos);
}

int ksvm_fs_syscall(ksvm *vm, uint32_t num, const ksvm_fs_ops *ops, void *ctx)
{
    switch (num) {
    case KSVM_SYSCALL_OPEN:  fs_open(vm, ops, ctx);  break;
    case KSVM_SYSCALL_CLOSE: fs_close(vm, ops, ctx); break;
    case KSVM_SYSCALL_READ:  fs_read(vm, ops, ctx);  break;
    case KSVM_SYSCALL_WRITE: fs_write(vm, ops, ctx); break;
    case KSVM_SYSCALL_SEEK:  fs_seek(vm, ops, ctx);  break;
    default:                 return KSVM_SYS_UNKNOWN;
    }
    return KSVM_SYS_OK;
}
