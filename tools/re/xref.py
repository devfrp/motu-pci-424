#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
xref.py - capstone-based cross-reference / disassembly helper for the MOTU
vendor driver `MOTUAW.sys` (PE32 i386, image base 0x10000).

objdump gives no xrefs; vtable-scan.py resolves vtable slots but not call sites.
This closes the gap: it linearly disassembles .text with capstone, then answers
"who calls VA X", "what does function at VA Y do", and "where is immediate/disp
Z used" - including indirect `call [reg+disp]` virtual dispatch sites.

Needs capstone (pip install capstone, in a venv on PEP-668 distros).

Usage:
    xref.py fn 0x11320            # disassemble the function starting at VA
    xref.py callers 0x11320       # direct callers of VA
    xref.py vcalls                # every indirect `call [reg+disp]`, by disp
    xref.py imm 0x30cc0           # instructions referencing an immediate/disp
    xref.py mem 0x64              # instructions touching [reg+0x64] (disp match)

Section table is for the shipped MOTUAW.sys; adjust from `objdump -h` if needed.
"""
import sys
try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM
except ImportError:
    sys.exit("capstone missing: python3 -m venv v && v/bin/pip install capstone")

PATH = "vendor/MOTUAW.sys"
# name -> (virtual_addr, file_offset, size)  from `objdump -h MOTUAW.sys`
TEXT_VA, TEXT_FO, TEXT_SZ = 0x11000, 0x400, 0x1e4ba


def disasm():
    """Linear sweep that resumes past undecodable bytes (jump tables / data
    embedded in .text) instead of stopping at the first one."""
    buf = open(PATH, "rb").read()[TEXT_FO:TEXT_FO + TEXT_SZ]
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    out, off = [], 0
    while off < len(buf):
        got = False
        for i in md.disasm(buf[off:], TEXT_VA + off):
            out.append(i)
            off = (i.address - TEXT_VA) + i.size
            got = True
        if not got:
            off += 1  # skip the byte capstone choked on, resume
    return out


def fn_body(insns, start):
    """Yield instructions from start until a ret (stops at first ret/retN)."""
    out, seen = [], False
    for ins in insns:
        if ins.address == start:
            seen = True
        if seen:
            out.append(ins)
            if ins.mnemonic.startswith("ret"):
                break
    return out


def fn_start(insns, va):
    """Nearest `push ebp` / `mov edi,edi;push ebp` prologue at or before va."""
    prev = None
    for ins in insns:
        if ins.address > va:
            break
        if ins.mnemonic == "push" and ins.op_str == "ebp":
            prev = ins.address
    return prev


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    ins = disasm()

    if cmd == "fn":
        start = int(sys.argv[2], 0)
        s = fn_start(ins, start) or start
        for i in fn_body(ins, s):
            print(f"  {i.address:#07x}: {i.mnemonic:7s} {i.op_str}")

    elif cmd == "callers":
        tgt = int(sys.argv[2], 0)
        for i in ins:
            if i.mnemonic == "call" and i.op_str == hex(tgt):
                fs = fn_start(ins, i.address)
                print(f"  {i.address:#07x} (in fn {fs:#x}): call {tgt:#x}")

    elif cmd == "vcalls":
        # indirect calls through a register+displacement (virtual dispatch)
        from collections import defaultdict
        by = defaultdict(list)
        for i in ins:
            if i.mnemonic != "call":
                continue
            for op in i.operands:
                if op.type == CS_OP_MEM and op.mem.base != 0:
                    by[op.mem.disp].append(i.address)
        for disp in sorted(by):
            locs = " ".join(f"{a:#x}" for a in by[disp][:20])
            print(f"  disp {disp:#x} ({len(by[disp])}x): {locs}")

    elif cmd == "imm":
        val = int(sys.argv[2], 0)
        for i in ins:
            for op in i.operands:
                if op.type == CS_OP_IMM and op.imm == val:
                    print(f"  {i.address:#07x}: {i.mnemonic} {i.op_str}")
                elif op.type == CS_OP_MEM and op.mem.disp == val:
                    print(f"  {i.address:#07x}: {i.mnemonic} {i.op_str}")

    elif cmd == "mem":
        disp = int(sys.argv[2], 0)
        for i in ins:
            for op in i.operands:
                if op.type == CS_OP_MEM and op.mem.disp == disp and op.mem.base:
                    print(f"  {i.address:#07x}: {i.mnemonic} {i.op_str}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
