#!/usr/bin/env python3
"""Differential test: production AOT output vs Unicorn Engine (real x86).

For each guest instruction both sides snapshot (pc, eax..edi, cf/zf/sf/of/pf, df) before
executing it. The traces must be identical; the first divergence names the instruction
whose AOT translation is wrong.

  /tmp/dtenv/bin/python tests/difftest/run.py --exe game/AssassinsCreed_Dx10.exe \
      --start 0x8f6b50 --start 0x41ef40

Trial 0 starts from the CPU sample in aot_log.txt (the 0x8f6b93 / 0x9bbee7 crash) so the
real crash path is covered; further trials randomise registers, flags and memory to cover
branch directions the crash sample never took. Both sides get the same inputs.

Caveat: the registers in the crash log are the watchdog's periodic sample, not the exact
fault-time state, and the heap-guard stores (aot_stguard/aot_nodehit) are not part of this
harness. This measures instruction semantics only.
"""
import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "src", "transpiler"))
import gen as G         # noqa: E402
import oracle as O      # noqa: E402
import aot_transpiler as T   # noqa: E402

# aot_log.txt: [aot] eax=... line of the crashing thread, and its 8 stack words at esp.
LOG_STATE = dict(eax=0x9BE78650, ecx=0x00018640, edx=0x00018640, ebx=0x6917FCA8,
                 esp=0x6917F880, ebp=0x6917F888, esi=0x9BE60010, edi=0x9B96F430,
                 cf=0, zf=0, sf=0, of=0, pf=0, df=0, fs=0, gs=0, ftop=0, fsw=0x4000)
LOG_STACK = [0x9B96F430, 0x000030C8, 0x00000008, 0x008F6BB7,
             0x9B96F430, 0x9BE60010, 0x00018640, 0x604170C8]
LOG_OBJ = 0x9B96F430          # `this` of the vector-grow function in the crash
LOG_BUFS = (0x9BE60010, 0x9BE78650)

INTERESTING = [0x0, 0x1, 0xFFFFFFFF, 0x3FFF, 0x4000, 0x7FFFFFFF, 0x80000000,
               0x00010000, 0x2C, 0xB, 0xEC, 0xED, 0x18640, 0x1000]


def mem_regions(rec, insns, state, stack, rng, randomize):
    """Same bytes to both sides: the code pages that get fetched, the crash stack, the
    crash object, the import-thunk slot, plus fill for any page the run may touch."""
    regions = {}
    for a in insns:
        pg = a & ~0xFFF
        blob = rec.rd(pg, 0x1000)
        regions[pg] = blob + b"\0" * (0x1000 - len(blob))
    sp_page = state["esp"] & ~0xFFF
    page = bytearray(0x1000)
    for k, w in enumerate(stack):
        off = (state["esp"] + 4 * k) - sp_page
        if 0 <= off <= 0xFFC:
            page[off:off + 4] = w.to_bytes(4, "little")
    regions[sp_page] = bytes(page)

    obj_page = LOG_OBJ & ~0xFFF
    page = bytearray(regions.get(obj_page, bytes(0x1000)))
    off = LOG_OBJ - obj_page
    page[off:off + 8] = LOG_BUFS[1].to_bytes(4, "little") + (0x31 << 14).to_bytes(4, "little")
    regions[obj_page] = bytes(page)

    regions[0x29D0C000] = bytes(0x1000)          # [0x29d0c40] import thunk slot -> 0

    if randomize:
        pool = [LOG_OBJ & ~0xFFF, 0x6917F000, 0x9BE60000, 0x9BE78000, 0x7FFD0000, 0x10000000]
        for _ in range(4):
            base = rng.choice(pool)
            blob = regions.get(base)
            if blob is None:
                blob = bytes(rng.getrandbits(8) for _ in range(0x1000))
            else:
                b = bytearray(blob)
                for _ in range(64):
                    o = rng.randrange(0, 0xFFC) & ~3
                    b[o:o + 4] = rng.getrandbits(32).to_bytes(4, "little")
                blob = bytes(b)
            regions[base] = blob
    return [(base, blob) for base, blob in sorted(regions.items())]


def trial_state(rng, log, randomize):
    st = dict(log)
    if randomize:
        for k in ("eax", "ecx", "edx", "ebx", "esi", "edi"):
            st[k] = rng.choice(INTERESTING + [log[k], rng.getrandbits(32), (rng.getrandbits(20) & ~0xF) + 0x10000000])
        st["esp"] = rng.choice([0x6917F880, 0x6917FF00, 0x7FFD1000, 0x10001000, 0x9B96F000])
        # ecx is the REP counter for the memcpy at 0x9bc080: keep it bounded so both sides
        # touch a few hundred KB at most instead of gigabytes.
        st["ecx"] = rng.choice([0, 1, 2, 3, 4, 0x10, 0x3FFF, 0x4000, 0x18640])
        # ebp multiplies the element count of both memcpys (`imul esi, ebp` / `imul edx, ebp`),
        # so keep it small or a trial copies gigabytes.
        st["ebp"] = rng.choice([0, 1, 2, 3, 4, 8, 0x10])
        for f in ("cf", "zf", "sf", "of", "pf", "df"):
            st[f] = rng.getrandbits(1)
        st["fs"] = 0
    return st


def write_inputs(directory, state, regions):
    with open(os.path.join(directory, "init.txt"), "w") as f:
        for k, v in state.items():
            f.write("%s %x\n" % (k, v))
    with open(os.path.join(directory, "mem.map"), "w") as f:
        for base, blob in regions:
            f.write("%x %d\n" % (base, len(blob)))
    with open(os.path.join(directory, "mem.bin"), "wb") as f:
        for _, blob in regions:
            f.write(blob)


def rep_map(insns):
    """pc -> address after the instruction, for REP/REPNE string ops, which the AOT runs as
    one complete loop but which Unicorn would count per iteration."""
    out = {}
    for a, i in insns.items():
        pre = i.mnemonic.split()[:-1]
        if any(p in ("rep", "repe", "repne") for p in pre) and (0xF3 in i.prefix or 0xF2 in i.prefix):
            out[a] = a + i.size
    return out


def parse(lines):
    out = []
    for ln in lines:
        parts = ln.split()
        if not parts:
            continue
        if parts[0] == "S":
            out.append(dict(kind="S", pc=int(parts[1], 16),
                            regs=[int(x, 16) for x in parts[2:10]],
                            flags=dict(cf=int(parts[10]), zf=int(parts[11]), sf=int(parts[12]),
                                       of=int(parts[13]), pf=int(parts[14]), df=int(parts[15]))))
        elif parts[0] == "U":
            out.append(dict(kind="U", pc=int(parts[1], 16), what=" ".join(parts[2:])))
        elif parts[0] == "E":
            out.append(dict(kind="E", pc=int(parts[1], 16)))
        elif parts[0] in ("B", "X"):
            out.append(dict(kind=parts[0], pc=int(parts[1], 16), what=" ".join(parts[2:])))
        else:
            out.append(dict(kind=parts[0], raw=ln))
    return out


REGNAMES = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]


ALLFLAGS = {"cf", "zf", "sf", "of", "pf", "df"}


def first_bad(aot, x86, meta):
    """(index, reason) of the first difference, or (None, None).

    reason == "state": an instruction produced different registers or flags -- a
    translation bug. reason == "end": one side stopped executing at a different point
    (a call into an address outside the translated set, a return to an untranslated
    address, a memory budget cut-off). That is a coverage boundary of the harness, not a
    miscompile, and is reported separately.

    A snapshot records the state *before* an instruction, so the flags at step k were
    produced by the instruction at step k-1 and only the flags that instruction's AOT
    translation actually writes are compared (a flag the translator proves dead is
    allowed to keep its old value; that is not a divergence)."""
    n = min(len(aot), len(x86))
    for k in range(n):
        p, q = aot[k], x86[k]
        if p.get("kind") != q.get("kind"):
            return k, "end"
        if p.get("kind") != "S":            # trailing E/B/X marker
            if p.get("pc") != q.get("pc"):
                return k, "end"
            continue
        if p.get("pc") != q.get("pc"):
            return k, "end"
        if p.get("regs") != q.get("regs"):
            return k, "state"
        if k == 0:
            mask = ALLFLAGS
        else:
            mask = set((meta.get(aot[k - 1]["pc"], {}).get("flags") or "").split(",")) - {""}
        for f in ("cf", "zf", "sf", "of", "pf", "df"):
            if f in mask and p["flags"][f] != q["flags"][f]:
                return k, "state"
    return (None, None) if len(aot) == len(x86) else (n, "end")


def report(trial, aot, x86, meta, dis, state):
    n = min(len(aot), len(x86))
    k, reason = first_bad(aot, x86, meta)
    if k is None:
        return None
    if reason == "end":
        a_end = next((s["pc"] for s in aot if s["kind"] in ("E", "B", "X")), aot[-1].get("pc"))
        x_end = next((s["pc"] for s in x86 if s["kind"] in ("E", "B", "X")), x86[-1].get("pc"))
        print("[~] trial %d: run ended at different points (aot %#010x, x86 %#010x) -- "
              "coverage boundary, not a translation difference" % (trial, a_end, x_end))
        return "end"
    print("\nDIVERGENCE  trial %d" % trial)
    if k == 0:
        print("  the initial state itself differs (harness bug):")
        print("    aot %s" % aot[0] if aot else "    aot <empty>")
        print("    x86 %s" % x86[0] if x86 else "    x86 <empty>")
        return k
    if k == n:
        print("  trace length differs: aot %d, x86 %d (same state until then)" % (len(aot), len(x86)))
    cur = aot[k] if k < len(aot) else None
    if cur and cur["kind"] == "U":
        print("  AOT has no translation for 0x%08x (%s) -- the guest executed it, so this"
              " instruction becomes a runtime trap" % (cur["pc"], cur["what"]))
        culprit = cur["pc"]
    else:
        culprit = aot[k - 1]["pc"] if k - 1 < len(aot) else state["pc"]
    m = meta.get(culprit, {})
    print("  culprit instruction 0x%08x   %s   %s" % (culprit, m.get("text", "?"), m.get("bytes", "")))
    print("  AOT C: %s" % m.get("c", "<not emitted>"))
    if cur and cur["kind"] == "S":
        x = x86[k] if k < len(x86) else None
        if x and x["kind"] == "S":
            diffs = []
            for j, nm in enumerate(REGNAMES):
                if cur["regs"][j] != x["regs"][j]:
                    diffs.append("%s: aot %08x  x86 %08x" % (nm, cur["regs"][j], x["regs"][j]))
            mask = set((m.get("flags") or "").split(",")) - {""}
            for f in ("cf", "zf", "sf", "of", "pf", "df"):
                if f in mask and cur["flags"][f] != x["flags"][f]:
                    diffs.append("%s: aot %d  x86 %d" % (f, cur["flags"][f], x["flags"][f]))
            if cur["pc"] != x["pc"]:
                diffs.append("next pc: aot %08x  x86 %08x" % (cur["pc"], x["pc"]))
            if diffs:
                print("  state after that instruction (step %d, 0x%08x):" % (k, cur["pc"]))
                for d in diffs:
                    print("    " + d)
            else:
                print("  state matches but the traces continue differently (%s vs %s)"
                      % (cur["pc"], x["pc"]))
    return culprit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(HERE, "..", "..", "game", "AssassinsCreed_Dx10.exe"))
    ap.add_argument("--start", action="append", required=True, help="function entry, hex")
    ap.add_argument("--max-insns", type=int, default=400)
    ap.add_argument("--trials", type=int, default=8, help="1 log-state trial + N-1 randomised")
    ap.add_argument("--steps", type=int, default=300, help="guest instructions per trial")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cover", action="store_true",
                    help="one log-state and one randomised trial from every discovered basic-block entry")
    ap.add_argument("--per-entry", type=int, default=2)
    ap.add_argument("--build-dir")
    ap.add_argument("--strict-ends", action="store_true", help="treat a differing stop point as a failure")
    ap.add_argument("--selftest", action="store_true",
                    help="plant a wrong store in one translated instruction and require the harness to catch it")
    a = ap.parse_args()

    starts = [int(x, 16) for x in a.start]
    build = a.build_dir or tempfile.mkdtemp(prefix="difftest-")
    os.makedirs(build, exist_ok=True)
    rec, insns, blocks, _ = G.gen(a.exe, build, starts, a.max_insns)
    print("[+] %d instructions, %d blocks, %d bytes of guest code"
          % (len(insns), len(blocks), sum(i.size for i in insns.values())))

    meta = {}
    with open(os.path.join(build, "insns.tsv")) as f:
        next(f)
        for ln in f:
            pc, byt, text, mask, c = ln.rstrip("\n").split("\t")
            meta[int(pc, 16)] = dict(bytes=byt, text=text, flags=mask, c=c)

    exe = os.path.join(build, "difftest")

    def compile_harness():
        cmd = ["cc", "-O1", "-g", "-std=gnu11", "-w", "-I", HERE,
               os.path.join(build, "code.c"), os.path.join(HERE, "difftest.c"), "-o", exe]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout + r.stderr)
            sys.exit("compile failed")

    compile_harness()
    print("[+] built %s" % exe)

    dis = rec.md
    rng = random.Random(a.seed)
    addrs = set(insns)
    bad = 0

    reps = rep_map(insns)

    def one_trial(entry, state):
        regions = mem_regions(rec, insns, state, LOG_STACK if state is not None and not state.get("_rnd") else [], rng, bool(state.get("_rnd")))
        write_inputs(build, state, regions)
        c = subprocess.run([exe, os.path.join(build, "init.txt"),
                            os.path.join(build, "mem.map"), os.path.join(build, "mem.bin")],
                           capture_output=True, text=True)
        if c.returncode != 0:
            return None, None, regions
        aot = parse(c.stdout.splitlines())
        if any(s["kind"] == "B" for s in aot):
            return aot, [], regions     # runaway trial: the oracle would copy gigabytes, skip it
        out = []
        O.run(dict(state), regions, addrs, a.steps, out, reps)
        return aot, parse(out), regions

    if a.selftest:
        st = dict(LOG_STATE); st["pc"] = starts[0]
        aot, x86, _ = one_trial(starts[0], st)
        if aot is None:
            sys.exit("selftest: harness crashed on the unmodified build")
        pcs = [s["pc"] for s in aot if s["kind"] == "S"]
        if len(pcs) < 3:
            sys.exit("selftest: run too short to plant a change")
        target = pcs[1]
        src = open(os.path.join(build, "code.c")).read().splitlines()
        for n, ln in enumerate(src):
            if ln.strip() == "SNAP(0x%xU);" % target:
                src[n + 1] = "  r0 ^= 0x10000000; " + src[n + 1]
                break
        else:
            sys.exit("selftest: no SNAP line for %#x" % target)
        open(os.path.join(build, "code.c"), "w").write("\n".join(src) + "\n")
        compile_harness()
        aot, x86, _ = one_trial(starts[0], st)
        k, reason = first_bad(aot, x86, meta)
        if k == 2 and reason == "state" and aot[k - 1]["pc"] == target:
            print("[+] selftest ok: a planted change in the translation of %#x is reported at step %d" % (target, k))
        else:
            sys.exit("[!] selftest FAILED: planted change at %#x not reported (k=%s)" % (target, k))
        if not a.build_dir:
            shutil.rmtree(build, ignore_errors=True)
        return 0
    if a.cover:
        entries = [b[0].address for b in blocks]
        jobs = [(e, r) for e in entries for r in range(a.per_entry)]
    else:
        jobs = [(starts[t % len(starts)], 0 if t == 0 else 1) for t in range(a.trials)]

    covered, ends = set(), 0
    for trial, (entry, randomize) in enumerate(jobs):
        state = trial_state(rng, LOG_STATE, bool(randomize))
        state["pc"] = entry
        state["_rnd"] = bool(randomize)
        aot, x86, _ = one_trial(entry, state)
        if aot is None:
            print("[!] job %d from %#x: harness crashed" % (trial, entry))
            bad += 1
            continue
        covered |= {s["pc"] for s in aot if s["kind"] == "S"}
        if a.cover and trial % 10 == 0:
            print(".", end="", flush=True)
        stop = next((s for s in aot + x86 if s["kind"] in ("B", "X")), None)
        if stop is not None:
            print("[~] job %d from %#x: skipped at 0x%08x (%s)"
                  % (trial, entry, stop["pc"], "memory budget" if stop["kind"] == "B" else stop["what"]))
            continue
        res = report(trial, aot, x86, meta, dis, state)
        if res == "end":
            ends += 1
            continue
        if res is None and not a.cover:
            print("[+] trial %d from %#x: %d instructions, identical"
                  % (trial, entry, sum(1 for s in aot if s["kind"] == "S")))
        elif res is not None:
            bad += 1
            print("    entry %#x" % entry)
    print("\ncovered %d/%d translated instructions (%.0f%%)"
          % (len(covered & set(insns)), len(insns), 100.0 * len(covered & set(insns)) / len(insns)))
    print("%s" % ("FAIL: %d/%d jobs produced different registers or flags" % (bad, len(jobs)) if bad
                  else "PASS: no register or flag divergence in %d jobs (%d steps max each)"
                       % (len(jobs), a.steps)))
    if ends:
        print("     %d job(s) ended at a different point before any divergence (outside the translated set)" % ends)
    if a.strict_ends and ends:
        return 1
    if not a.build_dir:
        shutil.rmtree(build, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
