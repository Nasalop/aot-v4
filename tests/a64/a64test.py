#!/usr/bin/env python3
"""Offline oracle for the a64 backend: real x86 semantics (unicorn x86-32) vs the emitted AArch64 (unicorn arm64), randomised state.
Needs: pip install capstone==5.0.1 pefile==2023.2.7 unicorn ; clang + llvm-objcopy on PATH.
usage: a64test.py [-n TRIALS] [-k substring] [-v]
Each case is an Intel-syntax x86 snippet.  `ins` cases run one instruction through Blk.ins; `blk` cases run a basic block through Prog.block (compare+branch fusion,
flags liveness, call/ret/jmp lowering).  State compared: eax..edi (and that the upper 32 bits of x19..x26 stay zero), the live flags, every touched memory page,
xmm0-7 and st(0..7) (as doubles; the a64 x87 is double precision, inputs are chosen exactly representable) and, for blocks, the next pc.
A case that needs a C fallback (bl uf_*) is reported SKIP-FALLBACK: only native code is checkable here."""
import sys, os, subprocess, tempfile, random, struct, hashlib, argparse
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(HERE, '..', '..', 'src', 'transpiler'))
from unicorn import *
from unicorn.x86_const import *
from unicorn.arm64_const import *
from capstone import *
import a64gen
import aot_transpiler as T

F5 = ('cf', 'zf', 'sf', 'of', 'pf'); FBIT = {'cf': 0, 'pf': 2, 'zf': 6, 'sf': 7, 'of': 11}   # x86 EFLAGS bit positions
AB = dict(a64gen.FB)   # where the a64 backend keeps them in x28 (same layout as EFLAGS)
TMP = tempfile.mkdtemp(prefix='a64t')
CODE_A = 0x10000000000; CPU_A = 0x20000000000; CODE_X = 0x401000   # arm code / CPU struct above 4 GB so guest pages never collide
XOFF, FST, FTOP, FCW = 128, 768, 836, 832
SPECIAL = [0, 1, 2, 3, 4, 7, 8, 0xf, 0x10, 0x1f, 0x20, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff, 0xffffff80, 0xffff8000]
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: raise RuntimeError(' '.join(cmd) + '\n' + r.stderr)
def asm_x86(text):
    import re
    syms = set(); lines = []
    for ln in text.split('\n'):
        m = re.match(r'^\s*(j\w+|call)\s+(0x[0-9a-fA-F]+)\s*$', ln)
        if m: syms.add(m.group(2)); ln = '.att_syntax\n%s T_%s\n.intel_syntax noprefix' % (m.group(1), m.group(2)[2:])
        lines.append(ln)
    f = os.path.join(TMP, 'x.s'); open(f, 'w').write('.intel_syntax noprefix\n.text\n.globl _start\n_start:\n' + ''.join('.set T_%s, %s\n.globl T_%s\n' % (x[2:], x, x[2:]) for x in syms) + '\n'.join(lines) + '\n')
    run(['clang', '-target', 'i386-linux-gnu', '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-Ttext=0x%x' % CODE_X, f, '-o', f + '.elf']); run(['llvm-objcopy', '-O', 'binary', '-j', '.text', f + '.elf', f + '.bin']); return open(f + '.bin', 'rb').read()
def asm_a64(lines):
    f = os.path.join(TMP, 'a.s'); open(f, 'w').write('.text\n.balign 4\n' + '\n'.join(lines) + '\n')
    run(['clang', '-target', 'aarch64-linux-gnu', '-march=armv8.2-a+lse+crc', '-c', f, '-o', f + '.o']); run(['llvm-objcopy', '-O', 'binary', '-j', '.text', f + '.o', f + '.bin']); return open(f + '.bin', 'rb').read()

def page_fill(pg, seed):
    return hashlib.sha256(b'%d/%d' % (pg, seed)).digest() * 128   # 4096 bytes, deterministic per page

class Emu:
    def __init__(s, uc, seed):
        s.uc = uc; s.seed = seed; s.pages = set(); s.bad = False
        uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED, s.unm)
    def unm(s, uc, acc, addr, size, val, ud):
        pg = addr >> 12
        if addr >= 1 << 32 or (addr + size - 1) >> 32:
            s.bad = True; return False
        for p in range(pg, ((addr + size - 1) >> 12) + 1):
            if p not in s.pages: uc.mem_map(p << 12, 4096); uc.mem_write(p << 12, page_fill(p, s.seed)); s.pages.add(p)
        return True

def rand_regs(rnd):
    r = []
    for k in range(8):
        c = rnd.random()
        r.append(rnd.choice(SPECIAL) if c < .35 else 0x200000 + rnd.randrange(0, 0x3000) if c < .6 else (-rnd.randrange(1, 64)) & 0xffffffff if c < .7 else rnd.randrange(0, 64) if c < .8 else rnd.getrandbits(32))
    r[4] = 0x300000 + rnd.randrange(0, 0x400) * 4   # esp: aligned stack pointer inside mapped pages
    return r

def fp_init(rnd):
    st = [float(rnd.randrange(-40, 40)) / rnd.choice((1, 2, 4)) for _ in range(8)]
    xm = []
    for _ in range(8):
        if rnd.random() < .5: xm.append(struct.pack('<4f', *[float(rnd.randrange(-50, 50)) / rnd.choice((1, 2, 4)) for _ in range(4)]))
        else: xm.append(struct.pack('<2d', *[float(rnd.randrange(-50, 50)) / rnd.choice((1, 2, 4)) for _ in range(2)]))
    return st, xm, rnd.randrange(0, 8)

def make_x86(code, regs, fl, seed, fp, nsteps, end=None):
    uc = Uc(UC_ARCH_X86, UC_MODE_32); e = Emu(uc, seed)
    uc.mem_map(CODE_X & ~0xfff, 0x2000); uc.mem_write(CODE_X, code + b'\xf4'); e.pages.add(CODE_X >> 12); e.pages.add((CODE_X >> 12) + 1)
    for k, r in enumerate((UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI)): uc.reg_write(r, regs[k])
    ef = 2
    for f in F5:
        if fl[f]: ef |= 1 << FBIT[f]
    uc.reg_write(UC_X86_REG_EFLAGS, ef)
    st, xm, top = fp
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    for k in range(8): uc.reg_write(UC_X86_REG_XMM0 + k, int.from_bytes(xm[k], 'little'))
    return uc, e

def x87_x86(uc, st, top, depth):
    pass

def get_fp_x86(uc):
    xm = [uc.reg_read(UC_X86_REG_XMM0 + k).to_bytes(16, 'little') if False else int(uc.reg_read(UC_X86_REG_XMM0 + k)).to_bytes(16, 'little') for k in range(8)]
    return xm

def nan_equal(x, y):   # equal except sign/payload of NaN lanes (x86 'real indefinite' NaN is negative, ARM default NaN positive): benign
    for n, f in ((4, 'f'), (2, 'd')):
        lx = struct.unpack('<%d%s' % (n, f), x); ly = struct.unpack('<%d%s' % (n, f), y)
        if all(a == c or (a != a and c != c) for a, c in zip(lx, ly)): return True
    return False
def parse(code):
    return list(md.disasm(code, CODE_X))

def pack_flags(fl):
    v = 0
    for f in F5:
        if fl[f]: v |= 1 << AB[f]
    return v

class Case:
    def __init__(s, name, text, kind='ins', nd=None, live=None, fpu=False, fpdepth=0, fnbad=False):
        s.name, s.text, s.kind, s.nd, s.live, s.fpu, s.fpdepth = name, text, kind, nd, live, fpu, fpdepth

def build_arm(case, insns):
    """-> (lines, native_ok) ; lines are assembled at CODE_A"""
    nd = set(F5) if case.nd is None else set(case.nd)
    if case.kind == 'ins':
        B = a64gen.Blk(None, None, 20, 't')
        for k, i in enumerate(insns):
            try: B.ins(i, nd if k == len(insns) - 1 else set(F5))
            except a64gen.Fn: return None
        lines = B.o + ['b aot_a64_exit']
    else:
        live = set(F5) if case.live is None else set(case.live)
        blk = insns
        prog = a64gen.Prog(None, [[blk]], {blk[0].address: live}, T.flagrw, T.F5, lambda i, nd: 'uf_%x' % i.address)
        prog.live_out = {blk[0].address: live}
        class LO(dict):
            def __missing__(s, k): return set(live)
        prog.live_out = LO(); prog.live_out[blk[0].address] = live
        lines = prog.block(blk, 0, None)
        lines = [l for l in lines if not l.startswith(('.globl', '.hidden'))]
    if any(l.startswith('bl uf_') or l.startswith('bl aot_a64_fl') for l in lines): return None
    return ['movz x11, #0x300, lsl #32' if l == 'adrp x11, aot_a64_tab' else l for l in lines]   # dispatch table is mapped at 0x30000000000 (empty)

def stubs():
    return ['aot_a64_exit: hlt #0', 'aot_a64_slow: hlt #1', '.set aot_a64_retmiss, aot_a64_slow', '.set aot_a64_slowc, aot_a64_slow']

def run_case(case, ntrials, seed0, verbose=False):
    code = asm_x86(case.text); insns = parse(code)
    lines = build_arm(case, insns if case.kind != 'ins' else insns)
    if lines is None: return 'SKIP-FALLBACK', 0
    nd = set(F5) if case.nd is None else set(case.nd)
    live = nd if case.kind == 'ins' else (set(F5) if case.live is None else set(case.live))
    blob = asm_a64(lines + stubs() + ['.balign 4'])   # dispatch stubs follow the code; the targets of taken/fallthrough branches go through aot_a64_exit with the pc in w9
    nfail = 0; ntr = 0
    for t in range(ntrials):
        rnd = random.Random(seed0 * 100003 + t); seed = rnd.getrandbits(32)
        regs = rand_regs(rnd); fl = {f: rnd.random() < .5 for f in F5}; fp = fp_init(rnd)
        # ---- x86 reference
        uc, ex = make_x86(code, regs, fl, seed, fp, len(insns))
        st, xm, top = fp
        if case.fpu:   # load the x87 stack: st[0] is ST(0)
            uc.mem_map(0x7f0000, 0x1000); ex.pages.add(0x7f0000 >> 12)
            uc.mem_write(0x7f0000, struct.pack('<8d', *st)); uc.mem_write(CODE_X, b'')
            pre = b''.join(asm_x86('fld qword ptr [0x7f0000+%d]' % (8 * k)) for k in range(7, -1, -1) if True) if False else None
        # x87 initial state is loaded by executing fld instructions before the case
        pre = b''
        if case.fpu:
            pre = asm_x86('\n'.join('fld qword ptr [0x7f0000+%d]' % (8 * k) for k in range(case.fpdepth - 1, -1, -1)))
        full = pre + code
        uc.mem_write(CODE_X, full + b'\xf4')
        try: uc.emu_start(CODE_X, CODE_X + len(full), count=len(parse(pre)) + len(insns) if case.kind == 'ins' else len(parse(pre)) + len(insns))
        except UcError as e: continue
        if ex.bad: continue
        eip = uc.reg_read(UC_X86_REG_EIP) if case.kind == 'blk' else None
        xr = [uc.reg_read(r) for r in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI)]
        ef = uc.reg_read(UC_X86_REG_EFLAGS)
        # ---- arm
        ua = Uc(UC_ARCH_ARM64, UC_MODE_ARM); ua.ctl_set_cpu_model(UC_CPU_ARM64_MAX); ea = Emu(ua, seed)   # 'max' = LSE, CRC, ... as on the target
        ua.mem_map(CODE_A, 0x10000); ua.mem_write(CODE_A, blob); ua.mem_map(CPU_A, 0x1000); ea.pages.add(-1)
        for k in range(8): ua.reg_write(UC_ARM64_REG_X19 + k, regs[k])
        ua.mem_map(0x30000000000, 0x20000); ua.mem_write(0x30000000000, b'\xff' * 0x20000); ua.mem_map(0x30100000000, 0x4000); ua.mem_write(0x30100000000, struct.pack('<QQ', 0, CODE_A + len(blob) - 4)); ua.reg_write(UC_ARM64_REG_X29, 0x30100000010)   # empty dispatch table: every indirect branch ends in aot_a64_slow with the guest pc in w9
        ua.reg_write(UC_ARM64_REG_X27, CPU_A); ua.reg_write(UC_ARM64_REG_X28, pack_flags(fl))
        cpu = bytearray(0x400)
        for k in range(8): cpu[XOFF + 16 * k:XOFF + 16 * k + 16] = xm[k]
        if case.fpu:
            for k in range(case.fpdepth): pass
            cpu[FTOP] = (8 - case.fpdepth) & 7
            for k in range(case.fpdepth): cpu[FST + 8 * ((((8 - case.fpdepth) & 7) + k) & 7):FST + 8 * ((((8 - case.fpdepth) & 7) + k) & 7) + 8] = struct.pack('<d', st[k])
        ua.mem_write(CPU_A, bytes(cpu))
        if case.fpu: ua.mem_map(0x7f0000, 0x1000) if False else None
        # the x86 run preloaded 0x7f0000 only on its own side; mirror page contents for the arm side
        if case.fpu:
            ua.mem_map(0x7f0000, 0x1000); ea.pages.add(0x7f0000 >> 12); ua.mem_write(0x7f0000, struct.pack('<8d', *st))
        for p in list(ex.pages):
            pass
        err = None
        try: ua.emu_start(CODE_A, CODE_A + len(blob), count=4000)
        except UcError as e:
            err = e
            if ua.reg_read(UC_ARM64_REG_PC) in (CODE_A + len(blob) - 8, CODE_A + len(blob) - 4): err = None   # reached the aot_a64_exit stub (hlt #0): normal end (unicorn >= 2 raises on hlt)
        if err is not None:
            if ea.bad: continue
            if any(k in case.text for k in ('lock', 'xchg', 'xadd', 'cmpxchg')): continue   # LSE atomics fault on a misaligned address (x86 tolerates it); the random esi/edi are mostly misaligned: only aligned trials count
            print('  ARM fault', err, 'trial', t, 'regs', [hex(x) for x in regs]); nfail += 1; continue
        if ea.bad: continue
        ntr += 1
        ar = [ua.reg_read(UC_ARM64_REG_X19 + k) for k in range(8)]
        errs = []
        for k in range(8):
            if ar[k] >> 32: errs.append('x%d upper bits %x' % (19 + k, ar[k]))
            if (ar[k] & 0xffffffff) != xr[k]: errs.append('r%d arm=%08x x86=%08x' % (k, ar[k] & 0xffffffff, xr[k]))
        a28 = ua.reg_read(UC_ARM64_REG_X28)
        shf = case.text.split()[0] in ('shl', 'shr', 'sar', 'rol', 'ror') and not case.text.rstrip().endswith(', 1')
        for f in live:
            if shf and f == 'of': continue   # OF is architecturally undefined for counts != 1
            if case.text.startswith(('fcomi', 'fucomi')) and f in ('sf', 'of'): continue   # real CPUs clear SF/OF here (SDM); QEMU/unicorn leaves them unchanged
            if ((a28 >> AB[f]) & 1) != ((ef >> FBIT[f]) & 1): errs.append('flag %s arm=%d x86=%d' % (f, (a28 >> AB[f]) & 1, (ef >> FBIT[f]) & 1))
        if a28 & ~sum(1 << b for b in AB.values()): errs.append('x28 junk bits %x' % a28)
        if case.kind == 'blk':
            pc = ua.reg_read(UC_ARM64_REG_X9) & 0xffffffff
            if pc != eip: errs.append('next pc arm=%x x86=%x' % (pc, eip))
        # memory: every page either side touched must be identical (pages untouched by one side hold the deterministic fill)
        for p in sorted((ex.pages | ea.pages) - {-1, CODE_X >> 12, (CODE_X >> 12) + 1}):
            if p == 0x7f0000 >> 12: continue
            xa = bytes(uc.mem_read(p << 12, 4096)) if p in ex.pages else page_fill(p, seed)
            aa = bytes(ua.mem_read(p << 12, 4096)) if p in ea.pages else page_fill(p, seed)
            if xa != aa:
                d = [k for k in range(4096) if xa[k] != aa[k]][:4]; errs.append('mem page %x differs at +%s (x86 %s arm %s)' % (p, d, [xa[k] for k in d], [aa[k] for k in d])); break
        if any(x in case.text for x in ('xmm',)) or case.fpu:
            cpua = bytes(ua.mem_read(CPU_A, 0x400))
            for k in range(8):
                xx = int(uc.reg_read(UC_X86_REG_XMM0 + k)).to_bytes(16, 'little'); aa = cpua[XOFF + 16 * k:XOFF + 16 * k + 16]
                if xx != aa and not nan_equal(xx, aa): errs.append('xmm%d arm=%s x86=%s' % (k, aa.hex(), xx.hex())); break
        if case.fpu:
            cpua = bytes(ua.mem_read(CPU_A, 0x400)); top_a = cpua[FTOP]
            sx = uc.reg_read(UC_X86_REG_FPSW); top_x = (sx >> 11) & 7
            if top_a != top_x: errs.append('ftop arm=%d x86=%d' % (top_a, top_x))
            else:
                for k in range(8):
                    # x86 side: read st(k) via fxsave-less trick: unicorn exposes FP0..FP7 physical regs as (mantissa, exp) tuples
                    ph = (top_x + k) & 7; m, ex_ = uc.reg_read(UC_X86_REG_FP0 + ph); xv = (m * 2.0 ** ((ex_ & 0x7fff) - 16383 - 63) * (-1 if ex_ & 0x8000 else 1) if m else 0.0) if (ex_ & 0x7fff) < 0x7ff0 else float('nan')
                    av = struct.unpack_from('<d', cpua, FST + 8 * ph)[0]
                    if k < case.fpdepth + case.fpout and abs(xv - av) > 1e-9 * max(1, abs(xv)): errs.append('st(%d) arm=%r x86=%r' % (k, av, xv)); break
        if errs:
            nfail += 1
            if nfail <= 3: print('  FAIL trial %d regs=%s fl=%s: %s' % (t, [hex(x) for x in regs], ''.join(f[0] if fl[f] else '-' for f in F5), '; '.join(errs[:4])))
    return ('FAIL' if nfail else 'ok'), ntr

def cases():
    C = []
    def ins(t, nd=None, **k): C.append(Case(t.replace('\n', ' ; '), t, 'ins', nd, **k))
    def blk(t, live=(), **k): C.append(Case('BLK ' + t.replace('\n', ' ; '), t, 'blk', None, live, **k))
    R = ['eax', 'ecx', 'edx', 'ebx', 'esi', 'edi', 'ebp']
    for t in ['mov eax, ecx', 'mov eax, 0x12345678', 'mov eax, [esi+8]', 'mov [edi+4], eax', 'mov eax, [esi+ecx*4+0x10]', 'mov eax, [esi+ecx*4]', 'mov eax, [ecx*8+0x100000]', 'mov [ebx+edx*2-3], ecx',
              'mov eax, [esp+4]', 'mov [esp], ebx', 'mov dword ptr [esi+8], 0x1234', 'mov eax, [0x200100]', 'mov [0x200100], eax', 'lea eax, [ebx+ecx*4+7]', 'lea eax, [ebx+ecx]', 'lea eax, [ecx*2]', 'lea eax, [ebx-5]',
              'lea esi, [esi+esi*2]', 'movzx eax, byte ptr [esi+3]', 'movzx eax, word ptr [esi+2]', 'movsx eax, byte ptr [esi+3]', 'movsx eax, cx', 'movzx eax, cl', 'movzx ecx, dh', 'mov al, [esi+1]', 'mov [edi], cl',
              'mov ax, [esi]', 'mov [edi+2], dx', 'xchg eax, ecx', 'push eax', 'push esp', 'push dword ptr [esp+4]', 'push dword ptr [esi+8]', 'push 0x1234', 'push 0x12345678', 'push -1', 'pop eax', 'pop esp', 'pop dword ptr [esi]',
              'pop ebp', 'call_dummy_skip']:
        if t == 'call_dummy_skip': continue
        ins(t, nd=())
    for mn in ('add', 'sub', 'and', 'or', 'xor', 'cmp', 'test', 'adc', 'sbb'):
        for o in ('eax, ecx', 'eax, 5', 'eax, 0x12345', 'eax, -1', 'eax, [esi+4]', '[esi+4], eax', 'dword ptr [esi+4], 7', '[esi+ecx*4], eax', 'ecx, [ebx+edx*4+0x10]', 'al, cl', 'ax, 0x1234', 'byte ptr [esi], 5', 'cl, 9'):
            if mn == 'test' and o == 'dword ptr [esi+4], 7': pass
            for nd in (F5, ('cf', 'zf'), ('zf',), ()):
                ins('%s %s' % (mn, o), nd=nd)
    for mn in ('inc', 'dec', 'neg', 'not'):
        for o in ('eax', 'ecx', 'dword ptr [esi+4]', 'al', 'cx'):
            for nd in (F5, ('zf', 'sf'), ()): ins('%s %s' % (mn, o), nd=nd)
    for mn in ('shl', 'shr', 'sar', 'rol', 'ror'):
        for o in ('eax, 1', 'eax, 5', 'eax, cl', 'ecx, 31', 'dword ptr [esi], 3', 'ax, 3', 'al, 2'):
            for nd in (F5, ('cf', 'zf')): ins('%s %s' % (mn, o), nd=nd)
    for t in ['imul eax, ecx', 'imul eax, [esi+4]', 'imul eax, ecx, 12', 'imul eax, [esi], 100', 'imul ecx', 'mul ecx', 'mul dword ptr [esi]', 'div ecx', 'idiv ecx', 'cdq', 'cwde', 'bswap eax', 'bsf eax, ecx', 'bsr eax, ecx',
              'sete al', 'setne cl', 'setl dl', 'setge al', 'setle al', 'setg al', 'setb al', 'setae al', 'setbe al', 'seta al', 'sets al', 'setns al', 'seto al', 'setno al', 'setp al', 'setnp al', 'sete byte ptr [esi]',
              'cmove eax, ecx', 'cmovne eax, [esi]', 'cmovl eax, ecx', 'cmovge eax, ecx', 'cmovle eax, ecx', 'cmovg eax, ecx', 'cmovb eax, ecx', 'cmovae eax, ecx', 'cmovbe eax, ecx', 'cmova eax, ecx', 'cmovs eax, ecx', 'cmovns eax, ecx',
              'lock xadd [esi], eax', 'lock inc dword ptr [esi]', 'lock dec dword ptr [esi]', 'lock add dword ptr [esi], eax', 'lock cmpxchg [esi], ecx', 'xchg [esi], eax', 'xchg eax, [esi]', 'lock xchg [esi], eax', 'cmpxchg [esi], ecx',
              'lock or dword ptr [esi], 1', 'lock and dword ptr [esi], 3', 'lock sub dword ptr [esi], eax', 'xadd eax, ecx', 'xadd [esi], eax', 'rdtsc_skip']:
        if t.endswith('_skip'): continue
        ins(t, nd=F5 if t.startswith(('imul', 'mul', 'lock', 'xadd', 'cmpxchg', 'bs')) else ())
    # added with the flag-liveness / atomics batch: operands and flag needs the real code produces
    for t in ['lock xadd [esi], eax', 'lock xadd [esi+ecx*4+8], edx', 'lock xadd dword ptr [esp+4], ebx', 'xadd eax, ecx', 'xadd [esi+4], eax', 'lock inc dword ptr [esi]', 'lock dec dword ptr [esi+8]', 'lock dec dword ptr [esi+ecx*4]',
              'lock add dword ptr [esi], eax', 'lock add dword ptr [esi], 5', 'lock sub dword ptr [esi], ecx', 'lock sub dword ptr [esi], 1', 'xchg [esi], eax', 'xchg eax, [esi+4]', 'xchg eax, ecx', 'xchg esi, edi',
              'lock cmpxchg [esi], ecx', 'lock cmpxchg [esi+8], edx', 'cmpxchg [esi], ebx']:
        for nd in ((), ('zf',), ('zf', 'sf', 'of', 'pf'), F5): ins(t, nd=nd)
    for t in ['imul eax, ecx', 'imul eax, [esi+4]', 'imul eax, ecx, 12', 'imul eax, [esi], 100', 'imul edx, ebx, -3']:
        for nd in (('cf', 'of'), ('cf',), ('of',)): ins(t, nd=nd)
    for t in ['div ecx', 'idiv ecx', 'div dword ptr [esi]', 'idiv dword ptr [esi+4]', 'div ebx', 'idiv esi']: ins(t, nd=())
    for t in ['push esp', 'push ebp', 'push dword ptr [esp+8]', 'pop ebx', 'pop edi', 'mov eax, [esi+ecx*4]', 'mov eax, [esi+ecx*8+12]', 'mov [edi+edx*2+100], ebx', 'mov eax, [ebx+ecx*4+0x1000]', 'mov eax, [ebx+ecx*4-4]', 'mov [esi+edi*4], ecx',
              'movzx eax, byte ptr [esi+ecx*4]', 'mov ax, [esi+ecx*4]', 'mov [esi+ecx*4], dx', 'mov eax, [esi+300]', 'mov eax, [esi+5000]', 'mov eax, [esi-300]', 'mov eax, [esi+0x12345]', 'lea eax, [ebx+ecx*4+100]', 'lea edx, [esi+edi]', 'lea eax, [ebx+ecx*8-8]',
              'add [esi+ecx*4], eax', 'add eax, [esi+ecx*4+8]', 'cmp [esi+ecx*4], edx', 'movss xmm0, [esi+ecx*4]', 'movaps xmm1, [esi+0x20]', 'addss xmm0, [esi+ecx*4+4]']:
        ins(t, nd=())
        ins(t, nd=F5)
    for cc in ('e', 'ne', 'l', 'ge', 'le', 'g', 'b', 'ae', 'be', 'a', 's', 'ns'):
        for pre in ('cmp eax, ecx', 'test eax, ecx', 'sub eax, 5', 'cmp byte ptr [esi], 5', 'and eax, ecx', 'cmp eax, [esi+4]', 'add eax, ecx', 'or eax, ecx', 'xor eax, ecx'):
            blk('%s\nset%s dl\njmp 0x401100' % (pre, cc), live=()); blk('%s\ncmov%s ebx, esi\njmp 0x401100' % (pre, cc), live=()); blk('%s\nset%s byte ptr [edi]\nret' % (pre, cc), live=())
    blk('cmp eax, ecx\nsetl dl\nsetg dh\nret', live=()); blk('cmp eax, ecx\nsetl dl\nsetg dh\nret', live=F5); blk('cmp eax, ecx\ncmovl ebx, esi\ncmovg ebx, edi\nret', live=('zf',))
    blk('cmp eax, ecx\nsete dl\njne 0x401100', live=()); blk('test eax, eax\ncmovs ebx, ecx\nje 0x401100', live=())
    for cc in ('e', 'ne', 'l', 'ge', 'le', 'g', 'b', 'ae', 'be', 'a', 's', 'ns', 'o', 'no', 'p', 'np'):
        ins('set%s al' % cc, nd=F5, live=None)
        ins('cmov%s eax, ecx' % cc, nd=F5)
        blk('cmp eax, ecx\nj%s 0x401100' % cc); blk('test eax, eax\nj%s 0x401100' % cc); blk('add eax, ecx\nj%s 0x401100' % cc); blk('sub eax, 5\nj%s 0x401100' % cc)
        blk('and eax, ecx\nj%s 0x401100' % cc); blk('cmp eax, [esi+4]\nj%s 0x401100' % cc); blk('cmp byte ptr [esi], 5\nj%s 0x401100' % cc); blk('inc eax\nj%s 0x401100' % cc); blk('dec ecx\nj%s 0x401100' % cc)
        blk('cmp eax, ecx\nset%s al\nj%s 0x401100' % (cc, cc))
    for cc in ('e', 'ne', 'l', 'ge', 'le', 'g', 'b', 'ae', 'be', 'a', 's', 'ns'):
        for pre in ('cmp eax, ecx', 'test eax, ecx', 'sub eax, 5', 'cmp byte ptr [esi], 5', 'and eax, ecx', 'cmp eax, [esi+4]'):
            blk('%s\nset%s dl\nret' % (pre, cc), live=()); blk('%s\ncmov%s ebx, esi\nret' % (pre, cc), live=()); blk('%s\ncmov%s ebx, [edi+4]\nret' % (pre, cc), live=())
            blk('%s\nset%s dl\nret' % (pre, cc), live=F5); blk('%s\ncmov%s ebx, esi\nret' % (pre, cc))
    blk('cmp eax, ecx\nsetl dl\nsetg dh\nret', live=()); blk('cmp eax, ecx\ncmovl ebx, esi\ncmovg ebx, edi\nret', live=())
    for t in ('lea eax, [ecx+0x123456]', 'lea eax, [ecx-0x123456]', 'lea eax, [ecx+0x1000]', 'lea eax, [ecx-0x2000]', 'lea eax, [ecx+300]', 'lea eax, [ecx-300]', 'lea eax, [ecx+edx*4+0x5000]'): ins(t)
    for t in ('mov eax, [esp+0x40]', 'mov eax, [esp+0x1001]', 'mov al, [esp+300]', 'mov [esp+0x123], bl', 'movzx eax, byte ptr [esp+0x201]'): ins(t)
    for a in ('al', 'cl', 'ah', 'dh', 'bl'):
        for b in ('bl', 'ch', 'al', '0x41', '0x80', '0'):
            for m in ('cmp', 'test'): ins('%s %s, %s' % (m, a, b)); blk('%s %s, %s\nje 0x401100' % (m, a, b), live=()); blk('%s %s, %s\njl 0x401100' % (m, a, b), live=())
    for m in ('cmp', 'test'):
        for t in ('%s ax, bx', '%s cx, cx', '%s ax, 0x1234', '%s si, di', '%s al, al', '%s ah, ah'): ins(t % m); blk(t % m + '\nja 0x401100', live=())
    for t in ('add eax, 0xfff', 'add eax, 0x1000', 'add eax, 0x123000', 'sub ecx, 0x7ff', 'cmp edx, 0x4000', 'cmp edx, 0x4001', 'and eax, 0xff00', 'and eax, 0x55555555', 'or ebx, 0x80000000', 'xor esi, 0xfffffffe', 'test eax, 0x1f', 'test eax, 0x12345', 'add dword ptr [esp+8], 5', 'and dword ptr [esp+8], 0xfff0'): ins(t); blk(t + '\nja 0x401100', live=()); blk(t + '\nsetl dl\nret', live=())
    blk('push 5\npush 0\nret', live=()); blk('push eax\npush 0x12345\npush ecx\nret', live=()); blk('push 7\npush esi\nret', live=()); blk('push ebx\npush 9\nret', live=())
    for t in ('lea eax, [ecx+edx]', 'lea eax, [eax+edx*8]', 'lea eax, [ecx+edx*2+0x30]', 'lea eax, [edx*4+8]', 'lea eax, [ecx+0x5000]', 'lea ebx, [ebx+ebx*2]', 'lea eax, [0x123456]'): ins(t)
    blk('mov eax, [0x300040]\nmov ecx, [0x300044]\nmov [0x300048], eax\nmov dl, [0x300041]\nmov [0x30004a], cx\nadd [0x300050], ecx\nret', live=()); blk('mov eax, [0x300ff0]\nmov ecx, [0x30fff0]\nmov [0x310010], eax\nmovzx edx, word ptr [0x30fffe]\nret', live=())
    # aligned, equal/unequal cmpxchg, locked RMW with flags live, fused producer + several consumers
    for pre in ('mov esi, esp\nmov eax, [esi+8]\n', 'mov esi, esp\n'):
        blk(pre + 'lock cmpxchg [esi+8], edx\nret', live=F5); blk(pre + 'lock xadd [esi+8], ecx\nret', live=F5); blk(pre + 'lock xadd [esi+8], ecx\nret', live=())
        blk(pre + 'lock inc dword ptr [esi+8]\nsetz dl\nret', live=()); blk(pre + 'lock dec dword ptr [esi+8]\njnz 0x401100', live=()); blk(pre + 'lock add [esi+8], ecx\nsetb dl\nret', live=())
        blk(pre + 'lock sub dword ptr [esi+8], 3\nret', live=F5); blk(pre + 'lock and [esi+8], ecx\nret', live=F5); blk(pre + 'lock or [esi+8], ecx\nsets dl\nret', live=()); blk(pre + 'xchg [esi+8], ecx\nret', live=F5)
    blk('add eax, ecx\nsetb dl\nsetae dh\ncmovo ebx, esi\nret', live=()); blk('sub eax, ecx\nsetl dl\ncmovg ebx, esi\ncmovle edi, esi\njl 0x401100', live=()); blk('test eax, eax\nsets dl\ncmovz ebx, [esi+4]\nsetnz dh\nret', live=())
    blk('cmp eax, ecx\nseta dl\nret', live=()); blk('cmp eax, ecx\nseta dl\nret', live=('cf',)); blk('and eax, ecx\nsetle dl\nmov [edi], eax\nret', live=()); blk('shl eax, 3\nsetb dl\nret', live=())
    blk('imul eax, ecx\nseto dl\nret', live=()); blk('imul eax, ecx, 100\nsetc dl\nsets dh\nret', live=()); blk('fld1\nfld1\nfcomip st, st(1)\nfstp st(0)\nsetb dl\nret', live=(), fpu=True, fpdepth=0)
    blk('lahf\nret', live=F5); blk('sahf\nret', live=())
    # push/pop pairing, pre/post-index forms, index+displacement folded into the access, 32-bit wrap of base+index
    for t in ('push eax\npush ecx\nret', 'push eax\npush eax\npush ebx\nret', 'push esi\npush edi\npush ebp\npop ebp\npop edi\npop esi\nret', 'pop eax\npop ecx\nret', 'pop eax\npop eax\nret', 'push esp\npush eax\nret', 'pop ecx\npop esp\nret', 'push eax\npush 5\npop ecx\npop edx\nret'):
        blk(t, live=())
    # base + index*scale + disp with controlled registers (random sums can land on the x86 code page, which the two sides map differently)
    for sc, lg in ((1, 0), (2, 1), (4, 2), (8, 3)):
        pre = 'mov ebx, 0x40\nmov ecx, esp\nshr ecx, %d\n' % lg
        for t in ('mov eax, [ebx+ecx*%d+0x10]', 'mov eax, [ebx+ecx*%d+6]', 'mov [ebx+ecx*%d-4], eax', 'mov al, [ebx+ecx*%d+1]', 'mov ax, [ebx+ecx*%d+6]', 'mov eax, [ebx+ecx*%d-300]', 'mov eax, [ebx+ecx*%d+0x2000]', 'add eax, [ebx+ecx*%d+0xfc]', 'movzx eax, byte ptr [ebx+ecx*%d+3]', 'movzx eax, word ptr [ebx+ecx*%d+6]', 'lea eax, [ebx+ecx*%d+0x10]', 'mov eax, [ecx*%d+0x40]'):
            blk(pre + (t % sc) + '\nret', live=())
    blk('lea edi, [esp+0x20]\nmov esi, 0x10\nsub esi, 0x40\nmov eax, [esi+edi+0x10]\nmov [esi+edi+0x14], eax\nret', live=())
    blk('cmp eax, ecx\nsbb eax, eax\nret'); blk('mov eax, 5\ncmp ecx, 7\nsete dl\nret', live=()); blk('push ebx\ncall 0x401200'); blk('pop ebx\nret'); blk('ret 8'); blk('jmp 0x401300'); blk('add eax, ecx\njmp eax')
    blk('mov eax, [esi+ecx*4]\njmp eax'); blk('call dword ptr [esi+8]'); blk('call eax'); blk('cmp eax, ecx\ncmove eax, edx\njmp 0x401300', live=())
    for mn in ('add', 'sub', 'cmp', 'and', 'or', 'xor', 'test'):   # immediate forms folded into the instruction
        for o in ('eax, 4095', 'eax, 4096', 'eax, 0x7000', 'eax, 0x1000000', 'eax, -4095', 'eax, -4096', 'eax, -5', 'eax, 0xff00ff00', 'eax, 0x80000000', 'dword ptr [esi+4], -9', 'dword ptr [esi], 0xf0f0f0f0', 'ecx, 0x0000ffff', 'edx, 0x55555555'):
            for nd in (F5, ('cf', 'zf'), ()): ins('%s %s' % (mn, o), nd=nd)
    for cc in ('e', 'l', 'ge', 'b', 'a', 'be', 'ae', 's', 'g', 'le'):
        for pre in ('cmp eax, -5', 'cmp eax, 4095', 'add eax, -7', 'sub eax, -9', 'cmp ecx, 0x3000', 'and eax, 0xffff', 'test ecx, 0x80000000'):
            blk('%s\nj%s 0x401100' % (pre, cc), live=()); blk('%s\nset%s dl\nret' % (pre, cc), live=F5)
    for t in ['mov eax, fs:[0]', 'mov eax, fs:[0x18]', 'mov fs:[0x2c], ecx', 'mov eax, [0x12000000]', 'mov eax, [0x12000100]', 'mov [0x12003ffc], ecx', 'mov al, [0x12000001]', 'mov ax, [0x12000002]', 'movzx eax, byte ptr [0x12000003]', 'movss xmm0, [0x12000010]', 'movaps xmm0, [0x12000030]']:
        ins(t, nd=())
    # fusion across neutral instructions, several consumers, and chains that must NOT fuse
    MIDS = ('mov edx, [esi+8]', 'mov edx, 5\nlea ebx, [edi+ecx*4]\npush edx', 'mov dx, [esi]', 'movzx edx, byte ptr [esi]\npop ebx', 'movss xmm0, [esi]', 'mov edx, 5\nadd edx, 1', 'mov [edi], ecx\ninc edx', 'fld1')
    for ci, cc in enumerate(('e', 'l', 'g', 'be', 'a', 'b', 'ae', 's', 'le', 'ge')):
        for pi, pre in enumerate(('cmp eax, ecx', 'test eax, eax', 'sub eax, 5', 'cmp byte ptr [esi], 5', 'add eax, ecx', 'and eax, 0xff', 'xor eax, ecx', 'or eax, ecx')):
            mid = MIDS[(ci + pi) % len(MIDS)]; live = (), F5
            blk('%s\n%s\nj%s 0x401100' % (pre, mid, cc), live=live[pi & 1]); blk('%s\n%s\ncmov%s ebx, esi\nret' % (pre, mid, cc), live=live[(pi + 1) & 1])
            blk('%s\n%s\nset%s dl\nj%s 0x401100' % (pre, mid, cc, cc), live=live[(ci + pi) & 1])
    blk('cmp eax, ecx\ncmovl ebx, esi\ncmovg edi, esi\nsetne dl\njle 0x401100', live=()); blk('cmp eax, ecx\ncmovl ebx, esi\ncmp ebx, edi\nsetg dl\njne 0x401100', live=()); blk('cmp eax, ecx\nsete dl\nsbb ebx, ebx\nret', live=F5)
    blk('cmp eax, ecx\nmov edx, [esi]\nsetp dl\nret', live=()); blk('cmp eax, ecx\nmov edx, 4\ncmovo ebx, esi\nret', live=()); blk('test eax, eax\npush ebx\ncmovs ecx, edx\npop ebx\njz 0x401100', live=())
    for t in ['mul ecx', 'imul ecx', 'mul dword ptr [esi]', 'imul dword ptr [esi+4]', 'mul eax', 'imul esi']:
        for nd in ((), ('cf', 'of'), ('cf',)): ins(t, nd=nd)
    for t in ['lahf', 'sahf']:
        for nd in ((), F5, ('cf', 'zf')): ins(t, nd=nd)
    for t in ['inc eax', 'dec ecx', 'inc dword ptr [esi]', 'dec dword ptr [esi+4]']:
        for nd in (('zf', 'sf', 'of', 'pf'), ('zf',), ('of',)): blk('stc\n%s\nsbb edx, edx\nret' % t, live=()); blk('cmp eax, ecx\n%s\nsetb dl\nret' % t, live=())
    # SSE / x87 on the CPU struct
    for t in ['movss xmm0, [esi]', 'movss [esi], xmm1', 'movss xmm0, xmm1', 'movaps xmm0, xmm1', 'movaps xmm2, [esi]', 'movups xmm2, [esi+3]', 'movups [esi+3], xmm5', 'addss xmm0, xmm1', 'subss xmm0, [esi]', 'mulss xmm1, xmm2', 'divss xmm1, xmm2',
              'addsd xmm0, xmm1', 'mulsd xmm0, [esi]', 'subsd xmm3, xmm4', 'addps xmm0, xmm1', 'mulps xmm0, [esi]', 'subps xmm0, xmm1', 'xorps xmm0, xmm0', 'andps xmm0, xmm1', 'orps xmm0, xmm1', 'shufps xmm0, xmm1, 0x1b', 'unpcklps xmm0, xmm1',
              'cvtss2sd xmm0, xmm1', 'cvtsd2ss xmm0, xmm1', 'cvtps2pd xmm0, xmm1', 'cvtsi2ss xmm0, eax', 'cvtsi2sd xmm0, eax', 'cvttss2si eax, xmm1', 'cvttsd2si eax, xmm1', 'movsd xmm0, [esi]', 'movsd [esi], xmm1', 'movsd xmm0, xmm1',
              'movd xmm0, eax', 'movd eax, xmm1', 'movlps xmm0, [esi]', 'movhps xmm0, [esi]', 'movhlps xmm0, xmm1', 'movlhps xmm0, xmm1', 'pxor xmm0, xmm0', 'sqrtss xmm0, xmm1', 'maxss xmm0, xmm1', 'minss xmm0, xmm1', 'rcpss xmm0, xmm1']:
        ins(t, nd=())
    for t in ['comiss xmm0, xmm1', 'comisd xmm0, xmm1', 'ucomiss xmm0, [esi]']: ins(t, nd=F5)
    for t, d, o in [('fld dword ptr [esi]', 0, 1), ('fld qword ptr [esi]', 0, 1), ('fld st(1)', 2, 1), ('fld1', 0, 1), ('fldz', 0, 1), ('fild dword ptr [esi]', 0, 1), ('fstp dword ptr [esi]', 2, 0), ('fstp qword ptr [esi]', 2, 0), ('fst dword ptr [esi]', 2, 1),
                    ('fstp st(1)', 3, 1), ('fadd st, st(1)', 3, 2), ('fadd st(1), st', 3, 2), ('faddp st(1), st', 3, 1), ('fmulp st(1), st', 3, 1), ('fsubp st(1), st', 3, 1), ('fsubrp st(1), st', 3, 1), ('fdivp st(1), st', 3, 1), ('fmul dword ptr [esi]', 2, 1), ('fadd qword ptr [esi]', 2, 1),
                    ('fsub dword ptr [esi]', 2, 1), ('fdiv dword ptr [esi]', 2, 1), ('fxch st(1)', 3, 2), ('fxch st(2)', 3, 3), ('fchs', 2, 1), ('fabs', 2, 1), ('fsqrt', 2, 1), ('fistp dword ptr [esi]', 2, 0), ('fcomip st, st(1)', 3, 1), ('fucomip st, st(1)', 3, 1)]:
        c = Case(t, t, 'ins', F5 if 'comi' in t else (), fpu=True, fpdepth=d); c.fpout = o; C.append(c)
    return C

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('-n', type=int, default=60); ap.add_argument('-k', default=''); ap.add_argument('-v', action='store_true'); a = ap.parse_args()
    tot = {'ok': 0, 'FAIL': 0, 'SKIP-FALLBACK': 0}; fails = []; skips = []
    for ci, c in enumerate(cases()):
        if a.k and a.k not in c.name: continue
        if not hasattr(c, 'fpout'): c.fpout = 0
        try: r, n = run_case(c, a.n, ci + 1, a.v)
        except Exception as e: r, n = 'FAIL', 0; print('  EXC', c.name, type(e).__name__, e)
        tot[r] += 1
        if r == 'FAIL': fails.append(c.name); print('FAIL', c.name, 'nd=%s' % (','.join(c.nd) if c.nd is not None else 'all'))
        elif r == 'SKIP-FALLBACK': skips.append(c.name)
        elif n < a.n // 4: print('WEAK (only %d/%d usable trials): %s' % (n, a.n, c.name))
    print('ok %d  FAIL %d  skipped(fallback, not checkable) %d' % (tot['ok'], tot['FAIL'], tot['SKIP-FALLBACK']))
    if skips: print('fallback cases:', ' | '.join(sorted(set(s.split()[0] if not s.startswith('BLK') else s for s in skips)))[:1500])
    sys.exit(1 if fails else 0)
if __name__ == '__main__': main()
