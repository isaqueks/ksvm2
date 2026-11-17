/*
 * hal_win32.c — HAL Windows (ver hal_win32.h). Só tem conteúdo sob _WIN32.
 *
 * Nenhum ponteiro para a RAM do guest chega aqui: o grupo 3 passa por
 * hal_fs.c, que copia por um buffer local.
 */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hal_fs.h"
#include "ksvm/hal_win32.h"

typedef char ksvm_win32_fds_check[(KSVM_HAL_WIN32_MAX_FDS == KSVM_FS_MAX_FDS) ? 1 : -1];

/* ---- Utilitários ------------------------------------------------------ */

static bool write_all(HANDLE h, const uint8_t *buf, uint32_t len)
{
    while (len > 0u) {
        DWORD put = 0;
        if (!WriteFile(h, buf, (DWORD)len, &put, NULL) || put == 0u) {
            return false;
        }
        buf += put;
        len -= (uint32_t)put;
    }
    return true;
}

static bool is_console(HANDLE h)
{
    DWORD mode = 0;

    return GetFileType(h) == FILE_TYPE_CHAR && GetConsoleMode(h, &mode);
}

/* Há uma linha completa (Enter pressionado) no buffer do console? Só então
 * ReadFile em modo de linha retorna sem bloquear. */
static bool console_line_ready(HANDLE h)
{
    INPUT_RECORD rec[256];
    DWORD n = 0, i;

    if (!GetNumberOfConsoleInputEvents(h, &n) || n == 0u ||
        !PeekConsoleInputA(h, rec, 256u, &n)) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (rec[i].EventType == KEY_EVENT && rec[i].Event.KeyEvent.bKeyDown &&
            rec[i].Event.KeyEvent.uChar.AsciiChar == '\r') {
            return true;
        }
    }
    return false;
}

/* 1 = há dado, 0 = nada agora, -1 = fim (pipe fechado). */
static int pipe_ready(HANDLE h)
{
    DWORD avail = 0;

    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
        return -1;
    }
    return (avail != 0u) ? 1 : 0;
}

void ksvm_hal_win32_flush(ksvm_hal_win32 *hw)
{
    if (hw->out_len != 0u && !write_all((HANDLE)hw->out_h, hw->out, hw->out_len)) {
        hw->out_error = true;
    }
    hw->out_len = 0u;
}

/* ---- Console e tempo -------------------------------------------------- */

static int win_putc(void *ctx, uint8_t c)
{
    ksvm_hal_win32 *hw = (ksvm_hal_win32 *)ctx;

    hw->out[hw->out_len] = c;
    hw->out_len++;
    if (c == '\n' || hw->out_len == KSVM_HAL_WIN32_OUT_BUF) {
        ksvm_hal_win32_flush(hw);
    }
    return hw->out_error ? -1 : 0;
}

/* Lê uma linha do console para hw->line (bloqueia até o Enter). */
static int console_fill(ksvm_hal_win32 *hw)
{
    DWORD got = 0;

    if (!ReadFile((HANDLE)hw->in_h, hw->line, KSVM_HAL_WIN32_LINE, &got, NULL) || got == 0u) {
        hw->in_eof = true;
        return KSVM_HAL_EOF;
    }
    hw->line_len = (uint32_t)got;
    hw->line_pos = 0u;
    return 0;
}

/* Próximo byte da linha do console, convertendo "\r\n" em "\n". */
static int console_getc(ksvm_hal_win32 *hw)
{
    uint8_t c;

    do {
        if (hw->line_pos == hw->line_len) {
            if (!hw->stdin_block && !console_line_ready((HANDLE)hw->in_h)) {
                return KSVM_HAL_NODATA;
            }
            if (console_fill(hw) != 0) {
                return KSVM_HAL_EOF;
            }
        }
        c = hw->line[hw->line_pos];
        hw->line_pos++;
    } while (c == '\r');
    return c;
}

static int stream_getc(ksvm_hal_win32 *hw)
{
    HANDLE h = (HANDLE)hw->in_h;
    uint8_t c = 0;
    DWORD got = 0;

    if (!hw->stdin_block && GetFileType(h) == FILE_TYPE_PIPE) {
        int r = pipe_ready(h);
        if (r <= 0) {
            hw->in_eof = (r < 0);
            return (r < 0) ? KSVM_HAL_EOF : KSVM_HAL_NODATA;
        }
    }
    if (!ReadFile(h, &c, 1u, &got, NULL) || got == 0u) {
        hw->in_eof = true;
        return KSVM_HAL_EOF;
    }
    return c;
}

static int win_getc(void *ctx)
{
    ksvm_hal_win32 *hw = (ksvm_hal_win32 *)ctx;

    ksvm_hal_win32_flush(hw);
    if (hw->in_eof || hw->in_h == NULL) {
        return KSVM_HAL_EOF;
    }
    if (is_console((HANDLE)hw->in_h)) {
        return console_getc(hw);
    }
    return stream_getc(hw);
}

static uint32_t win_millis(void *ctx)
{
    const ksvm_hal_win32 *hw = (const ksvm_hal_win32 *)ctx;

    return (uint32_t)((GetTickCount64() - hw->t0_ms) & UINT32_C(0xFFFFFFFF));
}

static uint32_t win_micros(const ksvm_hal_win32 *hw)
{
    LARGE_INTEGER now;
    uint64_t ticks;

    if (hw->qpc_freq <= 0 || !QueryPerformanceCounter(&now)) {
        return 0u;
    }
    ticks = (uint64_t)(now.QuadPart - hw->qpc0);
    /* segundos inteiros + fração, sem estourar 64 bits */
    return (uint32_t)(((ticks / (uint64_t)hw->qpc_freq) * UINT64_C(1000000) +
                       (ticks % (uint64_t)hw->qpc_freq) * UINT64_C(1000000) /
                           (uint64_t)hw->qpc_freq) & UINT32_C(0xFFFFFFFF));
}

static void win_sleep_ms(void *ctx, uint32_t ms)
{
    ksvm_hal_win32_flush((ksvm_hal_win32 *)ctx);
    Sleep((DWORD)ms);
}

static void win_on_halt(void *ctx, uint32_t code)
{
    (void)code;
    ksvm_hal_win32_flush((ksvm_hal_win32 *)ctx);
}

/* ---- Grupo 3: backend de arquivos ------------------------------------- */

#if KSVM_ENABLE_SYSCALL_FS

static DWORD access_flags(uint32_t f)
{
    DWORD a = 0;

    if ((f & KSVM_FS_RD) != 0u) {
        a |= GENERIC_READ;
    }
    if ((f & KSVM_FS_WR) != 0u) {
        a |= ((f & KSVM_FS_APPEND) != 0u) ? FILE_APPEND_DATA : GENERIC_WRITE;
    }
    return a;
}

static uint32_t fs_open(void *ctx, const char *path, uint32_t flags, uint32_t *fd)
{
    ksvm_hal_win32 *hw = (ksvm_hal_win32 *)ctx;
    DWORD create = ((flags & KSVM_FS_CREAT) != 0u) ? OPEN_ALWAYS : OPEN_EXISTING;
    uint32_t slot;
    HANDLE h;

    for (slot = 3u; slot < KSVM_HAL_WIN32_MAX_FDS && hw->files[slot] != NULL; slot++) {
    }
    if (slot == KSVM_HAL_WIN32_MAX_FDS) {
        return KSVM_ERR_IO;
    }
    h = CreateFileA(path, access_flags(flags), FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    create, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return KSVM_ERR_IO;
    }
    hw->files[slot] = h;
    hw->owned[slot] = true;
    *fd = slot;
    return KSVM_ERR_OK;
}

static uint32_t fs_close(void *ctx, uint32_t fd)
{
    ksvm_hal_win32 *hw = (ksvm_hal_win32 *)ctx;
    HANDLE h = (HANDLE)hw->files[fd];
    bool owned = hw->owned[fd];

    if (h == NULL) {
        return KSVM_ERR_INVAL;
    }
    hw->files[fd] = NULL;
    hw->owned[fd] = false;
    if (owned && !CloseHandle(h)) {
        return KSVM_ERR_IO;
    }
    return KSVM_ERR_OK;
}

static uint32_t fs_read(void *ctx, uint32_t fd, uint8_t *buf, uint32_t len, uint32_t *done)
{
    ksvm_hal_win32 *hw = (ksvm_hal_win32 *)ctx;
    DWORD got = 0;

    if (hw->files[fd] == NULL) {
        return KSVM_ERR_INVAL;
    }
    ksvm_hal_win32_flush(hw);
    if (!ReadFile((HANDLE)hw->files[fd], buf, (DWORD)len, &got, NULL)) {
        if (GetLastError() != ERROR_BROKEN_PIPE) {
            return KSVM_ERR_IO;
        }
        got = 0;                               /* pipe fechado = fim */
    }
    *done = (uint32_t)got;
    return KSVM_ERR_OK;
}

static uint32_t fs_write(void *ctx, uint32_t fd, const uint8_t *buf, uint32_t len,
                         uint32_t *done)
{
    ksvm_hal_win32 *hw = (ksvm_hal_win32 *)ctx;

    if (hw->files[fd] == NULL) {
        return KSVM_ERR_INVAL;
    }
    ksvm_hal_win32_flush(hw);
    if (!write_all((HANDLE)hw->files[fd], buf, len)) {
        return KSVM_ERR_IO;
    }
    *done = len;
    return KSVM_ERR_OK;
}

static uint32_t fs_seek(void *ctx, uint32_t fd, int32_t off, uint32_t whence, uint32_t *pos)
{
    static const DWORD how[3] = { FILE_BEGIN, FILE_CURRENT, FILE_END };
    ksvm_hal_win32 *hw = (ksvm_hal_win32 *)ctx;
    LARGE_INTEGER dist, now;

    if (hw->files[fd] == NULL) {
        return KSVM_ERR_INVAL;
    }
    dist.QuadPart = off;
    if (!SetFilePointerEx((HANDLE)hw->files[fd], dist, &now, how[whence])) {
        return KSVM_ERR_IO;
    }
    if (now.QuadPart < 0 || (uint64_t)now.QuadPart > UINT32_C(0xFFFFFFFF)) {
        return KSVM_ERR_INVAL;
    }
    *pos = (uint32_t)now.QuadPart;
    return KSVM_ERR_OK;
}

static const ksvm_fs_ops win_fs_ops = { fs_open, fs_close, fs_read, fs_write, fs_seek };

#endif /* KSVM_ENABLE_SYSCALL_FS */

/* ---- Syscalls do host ------------------------------------------------- */

static int win_syscall(ksvm *vm, uint32_t num)
{
    ksvm_hal_win32 *hw = (ksvm_hal_win32 *)vm->hal->ctx;

    if (num == KSVM_SYSCALL_MICROS) {
        ksvm_ret(vm, win_micros(hw), KSVM_ERR_OK);
        return KSVM_SYS_OK;
    }
#if KSVM_ENABLE_SYSCALL_FS
    if (hw->fs_enabled) {
        return ksvm_fs_syscall(vm, num, &win_fs_ops, hw);
    }
#endif
    return KSVM_SYS_UNKNOWN;
}

/* ---- API -------------------------------------------------------------- */

static void *std_handle(DWORD which)
{
    HANDLE h = GetStdHandle(which);

    return (h == INVALID_HANDLE_VALUE) ? NULL : (void *)h;
}

void ksvm_hal_win32_init(ksvm_hal_win32 *hw, ksvm_hal *hal)
{
    LARGE_INTEGER li;
    uint32_t i;

    ZeroMemory(hw, sizeof *hw);
    hw->in_h = std_handle(STD_INPUT_HANDLE);
    hw->out_h = std_handle(STD_OUTPUT_HANDLE);
    hw->fs_enabled = true;
    hw->t0_ms = GetTickCount64();
    if (QueryPerformanceFrequency(&li)) {
        hw->qpc_freq = li.QuadPart;
        (void)QueryPerformanceCounter(&li);
        hw->qpc0 = li.QuadPart;
    }
    hw->files[0] = hw->in_h;
    hw->files[1] = hw->out_h;
    hw->files[2] = std_handle(STD_ERROR_HANDLE);
    for (i = 0; i < 3u; i++) {
        hw->owned[i] = false;
    }
    ZeroMemory(hal, sizeof *hal);
    hal->ctx = hw;
    hal->putc = win_putc;
    hal->getc = win_getc;
    hal->millis = win_millis;
    hal->sleep_ms = win_sleep_ms;
    hal->syscall = win_syscall;
    hal->on_halt = win_on_halt;
}

void ksvm_hal_win32_close_all(ksvm_hal_win32 *hw)
{
    uint32_t i;

    ksvm_hal_win32_flush(hw);
    for (i = 3u; i < KSVM_HAL_WIN32_MAX_FDS; i++) {
        if (hw->owned[i] && hw->files[i] != NULL) {
            (void)CloseHandle((HANDLE)hw->files[i]);
        }
        hw->files[i] = NULL;
        hw->owned[i] = false;
    }
}

#else  /* !_WIN32 */

/* ISO C proíbe unidade de tradução vazia. */
typedef int ksvm_hal_win32_unavailable;

#endif /* _WIN32 */
