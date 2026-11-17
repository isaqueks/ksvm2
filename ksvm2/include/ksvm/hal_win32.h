/*
 * hal_win32.h — HAL para Windows (MSVC e MinGW), equivalente a hal_posix.
 *
 *   putc      buffer próprio despejado com WriteFile (mesmas regras da POSIX)
 *   getc      console: só lê quando já há uma linha completa digitada (não
 *             bloqueia); pipe: PeekNamedPipe; arquivo: ReadFile. Com
 *             stdin_block, sempre ReadFile de 1 byte (bloqueia).
 *   millis    GetTickCount64 desde o init; MICROS por QueryPerformanceCounter
 *   sleep_ms  Sleep
 *   syscall   MICROS e, com KSVM_ENABLE_SYSCALL_FS=1 e fs_enabled, o grupo 3
 *             sobre CreateFileA/ReadFile/WriteFile/SetFilePointerEx
 *
 * Handles são guardados como void* para não exigir <windows.h> aqui. O
 * arquivo .c só tem conteúdo quando compilado com _WIN32.
 */
#ifndef KSVM_HAL_WIN32_H
#define KSVM_HAL_WIN32_H

#include "hal.h"

#define KSVM_HAL_WIN32_MAX_FDS 16u
#define KSVM_HAL_WIN32_OUT_BUF 512u
#define KSVM_HAL_WIN32_LINE    512u

typedef struct ksvm_hal_win32 {
    void    *in_h;                            /* HANDLE de entrada do guest */
    void    *out_h;                           /* HANDLE de saída do guest */
    bool     stdin_block;
    bool     fs_enabled;                      /* padrão: true */
    bool     in_eof;
    bool     out_error;
    uint64_t t0_ms;                           /* GetTickCount64 no init */
    int64_t  qpc_freq, qpc0;                  /* QueryPerformanceCounter */
    void    *files[KSVM_HAL_WIN32_MAX_FDS];    /* NULL = livre */
    bool     owned[KSVM_HAL_WIN32_MAX_FDS];    /* aberto pelo guest (fechar) */
    uint32_t out_len;
    uint8_t  out[KSVM_HAL_WIN32_OUT_BUF];
    uint32_t line_len, line_pos;              /* linha lida do console */
    uint8_t  line[KSVM_HAL_WIN32_LINE];
} ksvm_hal_win32;

void ksvm_hal_win32_init(ksvm_hal_win32 *hw, ksvm_hal *hal);
void ksvm_hal_win32_flush(ksvm_hal_win32 *hw);
void ksvm_hal_win32_close_all(ksvm_hal_win32 *hw);

#endif /* KSVM_HAL_WIN32_H */
