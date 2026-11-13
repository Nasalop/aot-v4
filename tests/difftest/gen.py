#!/usr/bin/env python3
"""Emit instrumented C for a bounded set of guest basic blocks.

Uses the *production* translator (aot_transpiler.Gen.tr) and the *production* flag
liveness pass copied verbatim from aot_transpiler.emit(), so a divergence reported by
tests/difftest/run.py is a divergence in what the app actually runs.

  gen.py <exe> <outdir> --start 0x8f6b50 [--start ...] [--max-insns 400]

Writes <outdir>/code.c and <outdir>/insns.tsv.
aot_transpiler.py is imported, never modified.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "src", "transpiler"))
import aot_transpiler as T   # noqa: E402


def discover(rec, starts, max_insns):
    """Bounded closure of decodable instructions: follows fallthrough, branches, loop
    targets and direct call targets, and stops at returns, indirect transfers and
    anything outside .text. Never runs Rec.discover() (which needs the whole image)."""
    insns, work = {}, list(starts)
    while work and len(insns) < max_insns:
        a = work.pop(0)
        while a not in insns and len(insns) < max_insns:
            if not rec.in_text(a):
                break
            i = rec.decode(a)
            if i is None:
                break
            insns[a] = i
            mn = i.mnemonic.split()[-1]
            nx = a + i.size
            ops = i.operands
            if mn in ("jmp", "ljmp") and ops and ops[0].type == T.X86_OP_IMM:
                work.append(ops[0].imm)
                break
            if mn in ("call", "lcall") and ops and ops[-1].type == T.X86_OP_IMM:
                work.append(ops[-1].imm)
                a = nx
                continue
            if (mn[0] == "j" and mn[1:] in T.COND) or mn.startswith("loop"):
                work.append(ops[0].imm)
                a = nx
                continue
            if T.is_term(mn):
                break
            a = nx
    return insns


def flag_sets(i):
    """Same overrides aot_transpiler.emit() applies before computing liveness."""
    mn = i.mnemonic.split()[-1]
    w = {f for f in T.F5 if i.eflags & T.WMASK[f]}
    r = {f for f in T.F5 if i.eflags & T.TMASK[f]}
    if mn in ("fcomi", "fucomi", "fcompi", "fucompi", "fcomip", "fucomip"):
        w = set(T.F5)
    elif mn.startswith("f") and not mn.startswith("fcmov"):
        w = set()
    if mn.startswith("fcmov"):
        r = {"cf", "zf", "pf"}
    if mn in ("lahf", "pushfd", "pushf", "pushfq"):
        r = set(T.F5)
    if mn == "test":
        w = set(T.F5)
    if mn == "cmc":
        r = r | {"cf"}
    if mn in ("loope", "loopz", "loopne", "loopnz"):
        r = r | {"zf"}
    if mn in ("rcl", "rcr"):
        r = r | {"cf"}
    return w, r


WDEF = {f: T._fm(("MODIFY", "SET", "RESET"), f) for f in T.F5}


def defined_flags(i):
    """Flags the instruction architecturally defines. UNDEFINED ones are excluded:
    IMUL leaves SF/ZF/PF undefined, SHR/SHL with a count above 1 leaves OF undefined, and
    hardware, QEMU and our translation legitimately disagree there, so a difference is not
    a translation bug."""
    mn = i.mnemonic.split()[-1]
    d = {f for f in T.F5 if i.eflags & WDEF[f]}
    if mn in ("fcomi", "fucomi", "fcompi", "fucompi", "fcomip", "fucomip"):
        d = set(T.F5)
    elif mn.startswith("f") and not mn.startswith("fcmov"):
        d = set()
    if mn == "test":
        d = set(T.F5)   # capstone reports no eflags for test with a memory operand
    if mn in ("imul", "mul", "div", "idiv"):
        d -= {"zf", "sf", "pf"}      # SDM: SF, ZF, AF and PF are undefined
    if mn in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
        cnt = None if len(i.operands) < 2 else (i.operands[1].imm & 0xFF if i.operands[1].type == T.X86_OP_IMM else None)
        if cnt != 1:
            d -= {"of"}               # SDM: OF is defined only for 1-bit shifts/rotates
    return d


def cond_flags(i):
    """True when the translation writes flags only on some paths (`if(cnt)` in the shift
    helper). The mask is derived from the C text, which cannot see that, so such a flag is
    not compared: a translation that leaves it stale is correct when the count is 0, and a
    translation that genuinely drops a live flag still shows up as a wrong branch."""
    mn = i.mnemonic.split()[-1]
    if mn not in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
        return False
    if len(i.operands) < 2:
        return True
    if i.operands[1].type != T.X86_OP_IMM:
        return True                       # count in CL: may be 0
    return (i.operands[1].imm & 0xFF) == 0


def is_var_count(i):
    mn = i.mnemonic.split()[-1]
    return ((mn in ("shl", "sal", "shr", "sar", "rol", "ror") and len(i.operands) > 1 and i.operands[1].type != T.X86_OP_IMM)
            or mn in ("bsf", "bsr", "shld", "shrd", "bt", "bts", "btr", "btc", "cmpxchg")
            or (0xF3 in i.prefix or 0xF2 in i.prefix))


def blocks_of(insns, starts):
    leaders = set(starts)
    for a, i in insns.items():
        mn = i.mnemonic.split()[-1]
        if i.operands and i.operands[0].type == T.X86_OP_IMM and (mn in ("jmp", "ljmp") or (mn[0] == "j" and mn[1:] in T.COND) or mn.startswith("loop")):
            leaders.add(i.operands[0].imm)
    out, cur, prev, pmn = [], [], None, None
    for a in sorted(insns):
        i = insns[a]
        if cur and (a in leaders or prev.address + prev.size != a or T.is_term(pmn)
                    or pmn in ("call", "lcall") or pmn.startswith("loop") or pmn[0] == "j"):
            out.append(cur)
            cur = []
        cur.append(i)
        prev, pmn = i, i.mnemonic.split()[-1]
    if cur:
        out.append(cur)
    return out


def gen(exe, outdir, starts, max_insns):
    rec = T.Rec(exe)   # parses the PE and maps the image; no discovery, so it is quick
    if not rec.m32:
        sys.exit("this harness follows the PE32 (32-bit) path; image is %s" % ("PE32+" if not rec.m32 else "?"))
    insns = discover(rec, starts, max_insns)
    if not insns:
        sys.exit("no instructions decoded at %s" % ", ".join(hex(s) for s in starts))
    blocks = blocks_of(insns, starts)
    entry = set(insns)   # every translated address is dispatchable: a call return or an indirect jump target may land anywhere in the set
    g = T.Gen(rec)
    code, meta, mask_by_pc = [], [], {}

    def tgt(a):
        return "goto L_%x;" % a if a in entry else "{pc=0x%xU;goto dispatch;}" % a

    body = []
    for b in blocks:
        live, needs = set(T.F5), []
        for i in reversed(b):           # verbatim from aot_transpiler.emit()
            mn = i.mnemonic.split()[-1]
            w, r = flag_sets(i)
            if is_var_count(i):
                needs.append(set(T.F5))
                live |= r
                continue
            needs.append(live & w)
            live = (live - w) | r
        needs.reverse()
        for i, nd in zip(b, needs):
            body.append("L_%x:;" % i.address)
            try:
                s = g.tr(i, nd, tgt)
            except Exception as e:      # Unsup or a translator bug: same trap emit() uses
                s = 'aot_unimpl(0x%xULL,"%s %s");' % (i.address, i.mnemonic, i.op_str.replace('"', "'"))
            body.append("  SNAP(0x%xU);" % i.address)
            body.append("  " + s)
            # Compare exactly the flags this translation assigns. Taking the mask from the
            # code itself (rather than from capstone/liveness) means a dead flag the
            # translator deliberately leaves stale is not reported, while a live flag it
            # fails to write still surfaces -- as a wrong branch, which the pc check catches.
            assigned = {f for f in ("cf", "zf", "sf", "of", "pf", "df")
                        if re.search(r"\b%s\s*=" % f, s) or ("c->%s=" % f) in s}
            if cond_flags(i):
                assigned = {"df"} if "df" in assigned else set()   # see cond_flags()
            mask = ",".join(sorted((assigned & (defined_flags(i) | {"df"}))))
            mask_by_pc[i.address] = mask
            meta.append("%08x\t%s\t%s %s\t%s\t%s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str, mask, s))
        # Fall through to the successor explicitly, exactly as aot_transpiler.emit() does:
        # relying on source-order fallthrough would drop into whichever block happens to be
        # emitted next (and off the end of the last block).
        last = b[-1]
        if last.mnemonic.split()[-1] not in ("jmp", "ljmp", "ret", "retf", "iret", "iretd", "iretq",
                                            "call", "lcall", "ud2", "int3", "int1", "hlt"):
            body.append("  " + tgt(last.address + last.size))

    src = ['/* generated by tests/difftest/gen.py from %s -- do not edit */' % os.path.basename(exe),
           '#include "difftest.h"',
           "uint32_t difftest_run(CPU *c) {",
           "  DECL_STATE;",
           "  uint32_t pc = c->pc;",
           "dispatch:",
           "  switch (pc) {"]
    src += ["  case 0x%xU: goto L_%x;" % (a, a) for a in sorted(insns)]
    src += ["  default: goto dtreexit;",
            "  }"]
    src += body
    src += ["dtreexit:;",
            "  SAVES();",
            "  c->pc = pc;",
            "  return pc;",
            "}"]
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "code.c"), "w") as f:
        f.write("\n".join(src) + "\n")
    with open(os.path.join(outdir, "insns.tsv"), "w") as f:
        f.write("pc\tbytes\ttext\tflags_written\tc\n")
        for line in meta:
            f.write(line + "\n")
    return rec, insns, blocks, mask_by_pc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("outdir")
    ap.add_argument("--start", action="append", required=True, help="guest address (hex) of a function entry to diff")
    ap.add_argument("--max-insns", type=int, default=400)
    a = ap.parse_args()
    rec, insns, blocks, _ = gen(a.exe, a.outdir, [int(x, 16) for x in a.start], a.max_insns)
    print("[+] %d instructions, %d blocks, pc range %#x..%#x"
          % (len(insns), len(blocks), min(insns), max(insns)))


if __name__ == "__main__":
    main()
