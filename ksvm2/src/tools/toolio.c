/*
 * toolio.c — utilitários comuns às ferramentas de host (ver toolio.h).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "toolio.h"

static bool read_stream(FILE *f, uint8_t **data, uint32_t *size)
{
    size_t cap = 4096u, len = 0u;
    uint8_t *buf = (uint8_t *)malloc(cap);

    while (buf != NULL) {
        size_t got = fread(buf + len, 1u, cap - len, f);
        len += got;
        if (got == 0u || len > TOOL_MAX_FILE) {
            break;
        }
        if (len == cap) {
            uint8_t *nb = (uint8_t *)realloc(buf, cap * 2u);
            if (nb == NULL) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2u;
        }
    }
    if (buf == NULL || ferror(f) || len > TOOL_MAX_FILE) {
        free(buf);
        return false;
    }
    *data = buf;
    *size = (uint32_t)len;
    return true;
}

bool tool_read_file(const char *prog, const char *path, uint8_t **data, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    bool ok;

    if (f == NULL) {
        fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(errno));
        return false;
    }
    ok = read_stream(f, data, size);
    fclose(f);
    if (!ok) {
        fprintf(stderr, "%s: %s: falha de leitura ou arquivo maior que %lu bytes\n",
                prog, path, (unsigned long)TOOL_MAX_FILE);
    }
    return ok;
}

static bool digit_value(char c, unsigned base, unsigned *v)
{
    if (c >= '0' && c <= '9') {
        *v = (unsigned)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
        *v = (unsigned)(c - 'a') + 10u;
    } else if (c >= 'A' && c <= 'F') {
        *v = (unsigned)(c - 'A') + 10u;
    } else {
        return false;
    }
    return *v < base;
}

/* Multiplicador do sufixo (1 se não houver); 0 se o sufixo é inválido. */
static uint32_t suffix_mult(const char *s)
{
    if (s[0] == '\0') {
        return 1u;
    }
    if (s[1] != '\0') {
        return 0u;
    }
    if (s[0] == 'k' || s[0] == 'K') {
        return 1024u;
    }
    if (s[0] == 'm' || s[0] == 'M') {
        return UINT32_C(1024) * 1024u;
    }
    return 0u;
}

bool tool_parse_u32(const char *s, uint32_t *out)
{
    unsigned base = 10u, d = 0u;
    uint64_t v = 0u;
    uint32_t mult;
    bool any = false;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16u;
        s += 2;
    } else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        base = 2u;
        s += 2;
    }
    for (; digit_value(*s, base, &d) || *s == '_'; s++) {
        if (*s == '_') {
            continue;
        }
        v = v * base + d;
        any = true;
        if (v > UINT32_C(0xFFFFFFFF)) {
            return false;
        }
    }
    mult = suffix_mult(s);
    if (!any || mult == 0u || v * mult > UINT32_C(0xFFFFFFFF)) {
        return false;
    }
    *out = (uint32_t)(v * mult);
    return true;
}

const char *tool_err_name(int err)
{
    switch (err) {
    case KSVM_OK:               return "OK";
    case KSVM_E_ARG:            return "ARG";
    case KSVM_E_FULL:           return "FULL";
    case KSVM_E_OVERLAP:        return "OVERLAP";
    case KSVM_E_RANGE:          return "RANGE";
    case KSVM_E_IMAGE_MAGIC:    return "IMAGE_MAGIC";
    case KSVM_E_IMAGE_VERSION:  return "IMAGE_VERSION";
    case KSVM_E_IMAGE_CHECKSUM: return "IMAGE_CHECKSUM";
    case KSVM_E_IMAGE_RANGE:    return "IMAGE_RANGE";
    case KSVM_E_IMAGE_ENTRY:    return "IMAGE_ENTRY";
    case KSVM_E_NO_RAM:         return "NO_RAM";
    default:                    return "?";
    }
}

static bool ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool tool_is_ident(const char *s)
{
    if (!ident_start(*s)) {
        return false;
    }
    for (s++; *s != '\0'; s++) {
        if (!ident_start(*s) && !(*s >= '0' && *s <= '9')) {
            return false;
        }
    }
    return true;
}
