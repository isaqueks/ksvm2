#!/usr/bin/env python3
"""size_report.py — flash/RAM por módulo a partir do mapa do GNU ld.

    size_report.py [--budget BYTES] [--summary] firmware.map

Lê só a parte "Linker script and memory map" (depois do --gc-sections: o
que foi descartado não aparece) e soma o tamanho das seções de entrada por
arquivo de origem. Flash = .text (inclui .progmem e vetores) + imagem de
.data; RAM estática = .data + .bss + .noinit.

Grupos: módulos do núcleo (src/core), hal (hal_avr/hal_arm_cm), main (porta +
imagem do guest), libgcc, libc (avr-libc/newlib) e crt (vetores/startup).
--budget confere a meta RNF-04 para núcleo + hal + libgcc (rotinas de
divisão/multiplicação que o núcleo puxa) e sai com código 1 se estourar.
Serve também para o mapa da porta Cortex-M.
"""
import argparse
import os
import re
import sys

FLASH_SECTIONS = (".text", ".data")
RAM_SECTIONS = (".data", ".bss", ".noinit")

OUT_RE = re.compile(r"^(\.\S+)\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)")
IN_RE = re.compile(r"^ (\S+)?\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+(\S+)\s*$")
NAME_ONLY_RE = re.compile(r"^ (\S+)\s*$")


def module_of(path):
    m = re.search(r"\(([^)]+)\)$", path)
    member = m.group(1) if m else os.path.basename(path)
    archive = path[: m.start()] if m else ""
    base = os.path.splitext(member)[0]
    if "libgcc" in archive:
        return "libgcc"
    if re.search(r"lib(c|c_nano|m|atmega\w*)\.a$", archive):
        return "libc"
    if base.startswith("crt") or base == "startup":
        return "crt"
    if base.startswith("hal_"):
        return "hal"
    if base.startswith("main-"):
        return "main"
    return base


def parse(path):
    sizes = {}            # módulo -> {"flash": n, "ram": n}
    out_sec = None
    pending = None
    started = False
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not started:
                started = line.startswith("Linker script and memory map")
                continue
            m = OUT_RE.match(line)
            if m and not line.startswith(" "):
                out_sec = m.group(1)
                continue
            if line and not line.startswith(" ") and not line.startswith("\t"):
                nm = re.match(r"^(\.\S+)\s*$", line)
                out_sec = nm.group(1) if nm else out_sec
                continue
            m = IN_RE.match(line)
            if m and m.group(1) is None and pending is not None:
                name, size, src = pending, int(m.group(2), 16), m.group(3)
            elif m and m.group(1) is not None:
                name, size, src = m.group(1), int(m.group(2), 16), m.group(3)
            else:
                nm = NAME_ONLY_RE.match(line)
                pending = nm.group(1) if nm and not nm.group(1).startswith("*") else None
                continue
            pending = None
            if name.startswith("*") or size == 0 or src.startswith("0x"):
                continue
            mod = module_of(src)
            entry = sizes.setdefault(mod, {"flash": 0, "ram": 0})
            if out_sec in FLASH_SECTIONS:
                entry["flash"] += size
            if out_sec in RAM_SECTIONS:
                entry["ram"] += size
    return sizes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map")
    ap.add_argument("--budget", type=int, default=0)
    ap.add_argument("--summary", action="store_true")
    args = ap.parse_args()
    sizes = parse(args.map)
    extra = ("main", "crt", "libc", "libgcc", "hal")
    core = sorted(m for m in sizes if m not in extra)
    order = core + [m for m in extra if m in sizes]

    def total(mods, key):
        return sum(sizes[m][key] for m in mods if m in sizes)

    budget_mods = core + ["hal", "libgcc"]
    core_flash, core_ram = total(core, "flash"), total(core, "ram")
    vm_flash = total(budget_mods, "flash")
    all_flash, all_ram = total(order, "flash"), total(order, "ram")
    if args.summary:
        print("  nucleo: flash %5d  ram %4d | nucleo+hal+libgcc: flash %5d"
              " | firmware: flash %5d  ram %4d"
              % (core_flash, core_ram, vm_flash, all_flash, all_ram))
        return 0
    print("  %-12s %7s %6s" % ("modulo", "flash", "ram"))
    for m in order:
        if m == "hal":
            print("  %-12s %7d %6d" % ("(nucleo)", core_flash, core_ram))
        print("  %-12s %7d %6d" % (m, sizes[m]["flash"], sizes[m]["ram"]))
    print("  %-12s %7d %6d" % ("TOTAL", all_flash, all_ram))
    print("  nucleo + hal + libgcc = %d bytes de flash" % vm_flash, end="")
    if args.budget:
        ok = vm_flash <= args.budget
        print(" (meta RNF-04: <= %d: %s)" % (args.budget, "ok" if ok else "ESTOUROU"))
        return 0 if ok else 1
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
