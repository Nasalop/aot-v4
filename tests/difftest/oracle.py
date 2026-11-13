#!/usr/bin/env python3
"""Unicorn Engine oracle: executes the same guest addresses as real x86 and prints the
same trace format as the AOT build (see difftest.c), so the two can be diffed line by
line.

Memory model matches difftest.c exactly: preloaded pages come from the same blob, any
other page reads as zeros (here by mapping it zero filled on demand).

One step = one guest instruction with REP running to completion, which is what the
translated loop does. Unicorn counts every REP iteration as an instruction, so a plain
count=1 would stop half way through `rep movsd`; those are run with `until=next address`
instead, which is native speed and needs no Python callback per iteration.
"""
import os
import sys

os.environ.setdefault("UC_IGNORE_REG_BREAK", "1")   # FS/GS base are no-ops in 32-bit mode upstream


def _uc():
    try:
        from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED
        from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                                       UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
                                       UC_X86_REG_EIP, UC_X86_REG_EFLAGS)
    except ImportError:
        sys.exit("unicorn is not installed: python3 -m venv /tmp/dtenv && /tmp/dtenv/bin/pip install unicorn")
    return (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED,
            (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
             UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI),
            UC_X86_REG_EIP, UC_X86_REG_EFLAGS)


def run(init, regions, addrs, max_steps, out, rep=None):
    (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED,
     GREGS, R_EIP, R_EFLAGS) = _uc()
    rep = rep or {}
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    mapped = set()

    def ensure(base, size):
        page = base & ~0xFFF
        end = (base + size + 0xFFF) & ~0xFFF
        for p in range(page, end, 0x1000):
            if p in mapped:
                continue
            try:
                mu.mem_map(p, 0x1000)
            except Exception:      # already mapped
                pass
            mapped.add(p)

    for base, blob in regions:
        ensure(base, len(blob))
        mu.mem_write(base, blob)

    def _unmapped(mu_, access, address, size, value, user):
        ensure(address, size)
        return True

    mu.hook_add(UC_HOOK_MEM_UNMAPPED, _unmapped)

    for k, v in zip(GREGS, (init["eax"], init["ecx"], init["edx"], init["ebx"],
                            init["esp"], init["ebp"], init["esi"], init["edi"])):
        mu.reg_write(k, v)
    eflags = ((init.get("cf", 0) & 1) | ((init.get("pf", 0) & 1) << 2) | ((init.get("zf", 0) & 1) << 6)
              | ((init.get("sf", 0) & 1) << 7) | ((init.get("df", 0) & 1) << 10) | ((init.get("of", 0) & 1) << 11) | 2)
    mu.reg_write(R_EFLAGS, eflags)
    # Segment bases stay 0 on both sides: the AOT runtime models them as c->gs (0 unless a
    # TEB is installed) and Unicorn 2 refuses to write FS_BASE/GS_BASE in 32-bit mode.

    pc = init["pc"]
    steps = 0
    while steps < max_steps and pc in addrs:
        e = mu.reg_read(R_EFLAGS)
        out.append("S %08x %08x %08x %08x %08x %08x %08x %08x %08x %u %u %u %u %u %u %u %u" % (
            pc,
            mu.reg_read(GREGS[0]) & 0xFFFFFFFF, mu.reg_read(GREGS[1]) & 0xFFFFFFFF,
            mu.reg_read(GREGS[2]) & 0xFFFFFFFF, mu.reg_read(GREGS[3]) & 0xFFFFFFFF,
            mu.reg_read(GREGS[4]) & 0xFFFFFFFF, mu.reg_read(GREGS[5]) & 0xFFFFFFFF,
            mu.reg_read(GREGS[6]) & 0xFFFFFFFF, mu.reg_read(GREGS[7]) & 0xFFFFFFFF,
            e & 1, (e >> 6) & 1, (e >> 7) & 1, (e >> 11) & 1, (e >> 2) & 1, (e >> 10) & 1,
            init.get("ftop", 0), init.get("fsw", 0)))
        try:
            if pc in rep:
                mu.emu_start(pc, rep[pc])
            else:
                mu.emu_start(pc, 0, 0, 1)
        except Exception as ex:
            out.append("X %08x %s" % (pc, str(ex).replace("\n", " ")))
            return
        steps += 1
        pc = mu.reg_read(R_EIP) & 0xFFFFFFFF
    out.append("E %08x" % pc)
