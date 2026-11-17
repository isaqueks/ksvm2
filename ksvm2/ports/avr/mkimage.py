#!/usr/bin/env python3
"""mkimage.py — converte uma imagem .ksx em program_image.h para embutir.

    mkimage.py entrada.ksx saida.h [--name ksvm_program_image]

Valida o magic, a versão major e o checksum Adler-32 (doc 05) antes de
gerar, para que uma imagem corrompida falhe no build e não no dispositivo.

O header gerado declara

    #define <NAME>_SIZE <n>u
    static const uint8_t <name>[<NAME>_SIZE] KSVM_IMAGE_ATTR = { ... };

KSVM_IMAGE_ATTR é definido por quem inclui (PROGMEM no AVR, nada no
Cortex-M, onde a flash é endereçável); se não for, fica vazio.
"""
import argparse
import os
import struct
import sys

HEADER_SIZE = 64
CHECKSUM_OFF = 0x34
CHECKSUM_START = 0x38
VERSION_MAJOR = 2


def adler32(data):
    a, b = 1, 0
    for byte in data:
        a = (a + byte) % 65521
        b = (b + a) % 65521
    return (b << 16) | a


def validate(img, path):
    if len(img) < HEADER_SIZE:
        sys.exit("mkimage: %s: menor que o cabecalho" % path)
    if img[0:4] != b"KSX\x00":
        sys.exit("mkimage: %s: magic invalido" % path)
    if img[4] != VERSION_MAJOR:
        sys.exit("mkimage: %s: versao %d nao suportada" % (path, img[4]))
    (stored,) = struct.unpack_from("<I", img, CHECKSUM_OFF)
    if adler32(img[CHECKSUM_START:]) != stored:
        sys.exit("mkimage: %s: checksum nao confere" % path)


def render(img, src, name):
    guard = "KSVM_" + name.upper() + "_H"
    size = name.upper() + "_SIZE"
    lines = [
        "/* Gerado por ports/avr/mkimage.py a partir de %s. Nao editar. */"
        % os.path.basename(src),
        "#ifndef %s" % guard,
        "#define %s" % guard,
        "",
        "#include <stdint.h>",
        "",
        "#ifndef KSVM_IMAGE_ATTR",
        "#define KSVM_IMAGE_ATTR",
        "#endif",
        "",
        "#define %s %du" % (size, len(img)),
        "",
        "static const uint8_t %s[%s] KSVM_IMAGE_ATTR = {" % (name, size),
    ]
    for i in range(0, len(img), 12):
        chunk = ", ".join("0x%02X" % b for b in img[i:i + 12])
        lines.append("    " + chunk + ",")
    lines += ["};", "", "#endif /* %s */" % guard, ""]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("ksx")
    ap.add_argument("header")
    ap.add_argument("--name", default="ksvm_program_image")
    args = ap.parse_args()
    with open(args.ksx, "rb") as f:
        img = f.read()
    validate(img, args.ksx)
    with open(args.header, "w", encoding="utf-8", newline="\n") as f:
        f.write(render(img, args.ksx, args.name))


if __name__ == "__main__":
    main()
