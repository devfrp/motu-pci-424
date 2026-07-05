#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
vtable-scan.py - static C++ vtable / xref helper for reverse-engineering the
MOTU vendor driver `MOTUAW.sys` (PE32 i386, image base 0x10000).

objdump gives no cross-references, so this reconstructs them: it scans the
.rdata/.data sections for runs of dwords that all point into .text (i.e.
vtables), and lets you resolve a given vtable slot to a function VA. This is the
tool that cracked the IRQ path (hardware-class vtable @ 0x30cc0, ISR = slot
0x28 = 0x2bae0). See docs/register-map.md and docs/vendor-driver-map.md.

Usage:
    python3 tools/re/vtable-scan.py [path-to-MOTUAW.sys]
    python3 tools/re/vtable-scan.py MOTUAW.sys --slot 0x30cc0 0x28

The binary lives in the git-ignored vendor/ dir and is never redistributed.
Section offsets below are for the shipped MOTUAW.sys; adjust from `objdump -h`
if you re-run against a different build.
"""
import struct
import sys

# name -> (virtual_addr, file_offset, size)  from `objdump -h MOTUAW.sys`
SEC = {
    ".text":  (0x11000, 0x00400, 0x1e4ba),
    ".rdata": (0x30000, 0x1ea00, 0x00f34),
    ".data":  (0x31000, 0x1fa00, 0x31198),
}
TEXT_LO, TEXT_HI = 0x11000, 0x11000 + 0x1e4ba


def load(path):
    return open(path, "rb").read()


def va2fo(va):
    for _, (v, fo, sz) in SEC.items():
        if v <= va < v + sz:
            return fo + (va - v)
    return None


def rd32(buf, va):
    fo = va2fo(va)
    if fo is None or fo + 4 > len(buf):
        return None
    return struct.unpack("<I", buf[fo:fo + 4])[0]


def is_code(x):
    return x is not None and TEXT_LO <= x < TEXT_HI


def scan_vtables(buf, sec, min_slots=3):
    """Yield (vtable_va, [slot_targets]) for runs of >=min_slots code ptrs."""
    va, _, sz = SEC[sec]
    i = 0
    while i < sz - 4:
        a = va + i
        vals = []
        j = a
        while True:
            x = rd32(buf, j)
            if is_code(x):
                vals.append(x)
                j += 4
            else:
                break
        if len(vals) >= min_slots:
            yield a, vals
            i = j - va
        else:
            i += 4


def main():
    args = [a for a in sys.argv[1:]]
    path = "vendor/MOTUAW.sys"
    if args and not args[0].startswith("--"):
        path = args.pop(0)
    buf = load(path)

    if len(args) >= 3 and args[0] == "--slot":
        vt = int(args[1], 0)
        off = int(args[2], 0)
        tgt = rd32(buf, vt + off)
        print(f"vtable 0x{vt:x} slot@0x{off:x} -> "
              + (f"0x{tgt:x}" if tgt else "(none)"))
        return

    for sec in (".rdata", ".data"):
        print(f"==== vtable candidates in {sec} ====")
        for a, vals in scan_vtables(buf, sec):
            head = " ".join(f"0x{v:x}" for v in vals[:12])
            print(f"  vtab @0x{a:x} ({len(vals)} slots): {head}"
                  + (" ..." if len(vals) > 12 else ""))


if __name__ == "__main__":
    main()
