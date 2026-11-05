#!/usr/bin/env python3
"""Static recompiler: x86_64 Windows PE -> portable C (guest addr == host addr, mapped at ImageBase).
usage: aot_transpiler.py game.exe out_dir"""
import sys, os, struct, bisect
from collections import Counter
import pefile
from capstone import *
from capstone.x86 import *

F5 = ('cf', 'zf', 'sf', 'of', 'pf')
N64 = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi'] + ['r%d' % i for i in range(8, 16)]
REG = {}
for i, n in enumerate(N64): REG[n] = (i, 8, 0)
for i, n in enumerate(['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'] + ['r%dd' % i for i in range(8, 16)]): REG[n] = (i, 4, 0)
for i, n in enumerate(['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di'] + ['r%dw' % i for i in range(8, 16)]): REG[n] = (i, 2, 0)
for i, n in enumerate(['al', 'cl', 'dl', 'bl', 'spl', 'bpl', 'sil', 'dil'] + ['r%db' % i for i in range(8, 16)]): REG[n] = (i, 1, 0)
for i, n in enumerate(['ah', 'ch', 'dh', 'bh']): REG[n] = (i, 1, 1)

COND = {'o': 'of', 'no': '!of', 'b': 'cf', 'c': 'cf', 'nae': 'cf', 'ae': '!cf', 'nb': '!cf', 'nc': '!cf', 'e': 'zf', 'z': 'zf',
        'ne': '!zf', 'nz': '!zf', 'be': '(cf|zf)', 'na': '(cf|zf)', 'a': '!(cf|zf)', 'nbe': '!(cf|zf)', 's': 'sf', 'ns': '!sf',
        'p': 'pf', 'pe': 'pf', 'np': '!pf', 'po': '!pf', 'l': '(sf!=of)', 'nge': '(sf!=of)', 'ge': '(sf==of)', 'nl': '(sf==of)',
        'rcxz': '(r1==0)', 'ecxz': '((uint32_t)r1==0)', 'le': '(zf|(sf!=of))', 'ng': '(zf|(sf!=of))', 'g': '!(zf|(sf!=of))', 'nle': '!(zf|(sf!=of))'}

def _fm(kinds, f): return sum(getattr(sys.modules['capstone.x86'], 'X86_EFLAGS_%s_%s' % (k, f.upper())) for k in kinds)
WMASK = {f: _fm(('MODIFY', 'SET', 'RESET', 'UNDEFINED'), f) for f in F5}  # any of these bits is non-overlapping per flag
TMASK = {f: _fm(('TEST',), f) for f in F5}
MASK64 = (1 << 64) - 1

def image_fingerprint(path):
    value = 14695981039346656037
    with open(path, 'rb') as image:
        while True:
            chunk = image.read(1024 * 1024)
            if not chunk: break
            for byte in chunk:
                value = ((value ^ byte) * 1099511628211) & MASK64
    return value

class Unsup(Exception): pass

def is_term(mn): return mn in ('jmp', 'ljmp', 'ret', 'retf', 'iret', 'iretd', 'iretq', 'ud2', 'int3', 'hlt') or (mn[0] == 'j' and mn[1:] in COND)

class Rec:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        pe = self.pe
        mg, mc = pe.OPTIONAL_HEADER.Magic, pe.FILE_HEADER.Machine
        if not ((mg == 0x20b and mc == 0x8664) or (mg == 0x10b and mc == 0x14c)):
            sys.exit('ERROR: unsupported PE (machine 0x%x, magic 0x%x). Supported: x64 PE32+ and x86 PE32.' % (mc, mg))
        self.m32 = mg == 0x10b; self.psz = 4 if self.m32 else 8
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.image_hash = image_fingerprint(path)
        self.img = bytes(pe.get_memory_mapped_image())
        self.md = Cs(CS_ARCH_X86, CS_MODE_32 if self.m32 else CS_MODE_64); self.md.detail = True
        self.text = [(s.VirtualAddress + self.base, s.VirtualAddress + self.base + max(s.Misc_VirtualSize, s.SizeOfRawData))
                     for s in pe.sections if s.Characteristics & 0x20000000]
        self.insns = {}; self.leaders = set(); self.unsup = Counter(); self.funcs = []
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint

    def in_text(self, a): return any(lo <= a < hi for lo, hi in self.text)
    def rd(self, a, n):
        o = a - self.base
        return self.img[o:o + n] if 0 <= o < len(self.img) else b''
    def decode(self, a):
        b = self.rd(a, 15)
        return next(self.md.disasm(b, a, 1), None) if b else None

    # ---- discovery -------------------------------------------------------
    def seeds(self):
        s = {self.entry}
        pe = self.pe
        try:
            if self.m32: raise KeyError  # no .pdata in PE32
            d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]  # .pdata RUNTIME_FUNCTION
            for off in range(0, d.Size - 11, 12):
                b, e, _ = struct.unpack_from('<III', self.img, d.VirtualAddress + off)
                if b == 0: break
                self.funcs.append((b + self.base, e + self.base))
                s.add(b + self.base)
        except Exception: pass
        self.funcs.sort(); self.fstarts = [f[0] for f in self.funcs]
        for sec in pe.sections:  # absolute code pointers in data (vtables, callbacks)
            if sec.Characteristics & 0x20000000: continue
            o = sec.VirtualAddress; n = min(sec.Misc_VirtualSize, sec.SizeOfRawData) & ~(self.psz - 1)
            lo_, hi_ = self.base, self.base + len(self.img)
            for v in memoryview(self.img[o:o + n]).cast('I' if self.m32 else 'Q'):
                if lo_ <= v < hi_ and self.in_text(v): s.add(v)
        try:
            tls = pe.DIRECTORY_ENTRY_TLS.struct
            cb = tls.AddressOfCallBacks
            while cb:
                v = struct.unpack_from('<I' if self.m32 else '<Q', self.img, cb - self.base)[0]
                if not v: break
                s.add(v); cb += self.psz
        except Exception: pass
        return {a for a in s if self.in_text(a)}

    def func_range(self, a):
        k = bisect.bisect_right(self.fstarts, a) - 1
        if k >= 0 and self.funcs[k][0] <= a < self.funcs[k][1]: return self.funcs[k]
        return (a - 0x4000, a + 0x4000)

    def table_targets(self, jm, recent):
        """Heuristic jump-table recovery for `jmp reg` / `jmp [tbl+idx*8]`."""
        lo, hi = self.func_range(jm.address)
        lo = max(lo, self.text[0][0]); out = set()
        cands = []  # (table address, kind) kind 'rva' | 'abs' | 'rel:<base>'
        ops = jm.operands
        if ops[0].type == X86_OP_MEM and ops[0].mem.scale == self.psz and ops[0].mem.base == 0 and ops[0].mem.index:
            cands.append((ops[0].mem.disp & MASK64, 'abs'))
        leas = {}
        for i in recent:
            if i.mnemonic == 'lea' and i.operands[1].mem.base == X86_REG_RIP:
                leas[i.operands[0].reg] = i.address + i.size + i.operands[1].mem.disp
        for i in reversed(recent):
            for o in i.operands:
                if o.type == X86_OP_MEM and o.mem.scale == 4 and o.mem.index and o.mem.base != X86_REG_RIP:
                    if o.mem.base in leas: cands.append((leas[o.mem.base], 'rel:%d' % leas[o.mem.base]))
                    if o.mem.disp > 0 and not self.m32: cands.append((self.base + o.mem.disp, 'rva'))
        for tab, kind in cands:
            found = []
            for k in range(0, 2048):
                if kind == 'abs': v = struct.unpack('<Q', self.rd(tab + self.psz * k, self.psz).ljust(8, b'\0'))[0]
                else:
                    raw = self.rd(tab + 4 * k, 4)
                    if len(raw) < 4: break
                    v = struct.unpack('<I', raw)[0] + self.base if kind == 'rva' else (tab + struct.unpack('<i', raw)[0]) & MASK64
                if lo <= v < hi + 1 and self.in_text(v): found.append(v)
                else: break
            if found: out.update(found); break
        return out

    def discover(self):
        work = list(self.seeds()); self.leaders |= set(work)
        while work:
            a = work.pop(); recent = []
            while a not in self.insns:
                i = self.decode(a)
                if i is None or not self.in_text(a): break
                self.insns[a] = i; recent = (recent + [i])[-120:]
                mn = i.mnemonic.split()[-1]; nxt = a + i.size
                direct = i.operands and i.operands[0].type == X86_OP_IMM
                if mn in ('lea', 'mov', 'movabs', 'push') and len(i.operands) > 0:  # code pointers materialised by code
                    o = i.operands[-1]
                    t = (i.address + i.size + o.mem.disp if mn == 'lea' and o.mem.base == X86_REG_RIP else o.imm if o.type == X86_OP_IMM else 0)
                    if t and t not in self.leaders and self.in_text(t) and t != nxt: self.leaders.add(t); work.append(t)
                if mn in ('jmp', 'ljmp'):
                    target = i.operands[-1] if mn == 'ljmp' else i.operands[0]
                    if target.type == X86_OP_IMM: self.leaders.add(target.imm); work.append(target.imm)
                    elif mn == 'jmp':
                        for t in self.table_targets(i, recent): self.leaders.add(t); work.append(t)
                    break
                if (mn[0] == 'j' and mn[1:] in COND) or mn.startswith('loop'):
                    t = i.operands[0].imm; self.leaders.update((t, nxt)); work.append(t)
                elif mn in ('call', 'lcall'):
                    self.leaders.add(nxt)
                    target = i.operands[-1] if mn == 'lcall' else i.operands[0]
                    if target.type == X86_OP_IMM: self.leaders.add(target.imm); work.append(target.imm)
                elif is_term(mn): break
                a = nxt
            else:
                # Decoding ran into an already-decoded address: two instruction chains converge here (overlapping decodes from a
                # mid-instruction seed). That address must be a block entry, or the real chain falls through into a block that
                # swallowed it and has no case label.
                self.leaders.add(a)
        return self

    def blocks(self):
        bl = []; cur = None; prev = None
        for a in sorted(self.insns):
            i = self.insns[a]
            if cur is None or a in self.leaders or prev is None or prev.address + prev.size != a or is_term(prev.mnemonic.split()[-1]) \
               or prev.mnemonic.split()[-1] in ('call', 'lcall') or prev.mnemonic.split()[-1].startswith('loop') or (prev.mnemonic.split()[-1][0] == 'j'):
                cur = []; bl.append(cur)
            cur.append(i); prev = i
        return bl

# ---------------------------------------------------------------------------
class Gen:
    def __init__(self, rec):
        self.rec = rec; self.i = None; self.m32 = bool(getattr(rec, 'm32', False))

    # operands ---------------------------------------------------------------
    def reg(self, r):
        n = self.i.reg_name(r)
        if n not in REG: raise Unsup('reg ' + n)
        return REG[n]
    def xr(self, op):
        n = self.i.reg_name(op.reg)
        if not n.startswith('xmm'): raise Unsup('reg ' + n)
        return 'c->x[%s]' % n[3:]
    def mmr(self, op):
        n = self.i.reg_name(op.reg)
        if not n.startswith('mm') or not n[2:].isdigit(): raise Unsup('reg ' + n)
        return 'c->mm[%s]' % n[2:]
    def sreg(self, op):
        n = self.i.reg_name(op.reg)
        if n not in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'): raise Unsup('reg ' + n)
        return 'c->sregs[%d]' % ('es cs ss ds fs gs'.split().index(n))
    def addr(self, op):
        m = op.mem; i = self.i
        if self.m32:
            p = []
            if m.base: p.append('r%d' % self.reg(m.base)[0])
            if m.index: p.append('r%d*%d' % (self.reg(m.index)[0], m.scale))
            if m.disp or not p: p.append('0x%xULL' % (m.disp & 0xFFFFFFFF))
            e = '(uint64_t)(uint32_t)(%s)' % '+'.join(p)
        elif m.base == X86_REG_RIP:
            e = '0x%xULL' % ((i.address + i.size + m.disp) & MASK64)
        else:
            p = []
            if m.base: p.append('r%d' % self.reg(m.base)[0])
            if m.index: p.append('r%d*%d' % (self.reg(m.index)[0], m.scale))
            if m.disp or not p: p.append('0x%xULL' % (m.disp & MASK64))
            e = '(uint64_t)(%s)' % '+'.join(p)
        if m.segment in (X86_REG_GS, X86_REG_FS): e = '(%s+c->gs)' % e
        return e
    def rdop(self, op, S=None):
        S = S or op.size; T = 'uint%d_t' % (8 * S)
        if op.type == X86_OP_REG:
            n, s, h = self.reg(op.reg)
            return '(uint8_t)(r%d>>8)' % n if h else '(%s)r%d' % (T if s == S else 'uint%d_t' % (8 * s), n)
        if op.type == X86_OP_IMM: return '((%s)0x%xULL)' % (T, op.imm & ((1 << (8 * S)) - 1))
        return 'LD(%s,%s)' % (T, self.addr(op))
    def wrop(self, op, v, S=None):
        S = S or op.size
        if op.type == X86_OP_MEM: return 'ST(uint%d_t,%s,%s);' % (8 * S, self.addr(op), v)
        n, s, h = self.reg(op.reg)
        if h: return 'r%d=(r%d&~0xFF00ULL)|((uint64_t)(uint8_t)(%s)<<8);' % (n, n, v)
        if s == 8: return 'r%d=(uint64_t)(%s);' % (n, v)
        if s == 4: return 'r%d=(uint32_t)(%s);' % (n, v)
        return 'r%d=(r%d&~0x%xULL)|(uint%d_t)(%s);' % (n, n, (1 << 8 * s) - 1, 8 * s, v)

    def fl(self, need, **kw):  # emit flag assignments that are live
        return ''.join('%s=%s;' % (f, e) for f, e in kw.items() if f in need)

    # ALU -----------------------------------------------------------------
    def alu(self, mn, S, a, b, dst, need, nowrite=False):
        T = 'uint%d_t' % (8 * S); sb = 8 * S - 1; ST_ = 'int%d_t' % (8 * S)
        com = dict(zf='r==0', sf='(r>>%d)&1' % sb, pf='PAR(r)')
        pre = '%s a=%s,b=%s;' % (T, a, b)
        if mn == 'add': core, f = 'T r=a+b;', dict(cf='r<a', of='(((a^r)&(b^r))>>%d)&1' % sb)
        elif mn in ('sub', 'cmp'): core, f = 'T r=a-b;', dict(cf='a<b', of='(((a^b)&(a^r))>>%d)&1' % sb)
        elif mn == 'adc': core, f = 'T cin=cf; T r=a+b+cin;', dict(cf='cin?r<=a:r<a', of='(((a^r)&(b^r))>>%d)&1' % sb)
        elif mn == 'sbb': core, f = 'T cin=cf; T r=a-b-cin;', dict(cf='cin?a<=b:a<b', of='(((a^b)&(a^r))>>%d)&1' % sb)
        else:
            op = {'and': '&', 'test': '&', 'or': '|', 'xor': '^'}[mn]
            core, f = 'T r=a%sb;' % op, dict(cf='0', of='0')
        s = '{%s%s%s' % (pre, core.replace('T', T), self.fl(need, **{**f, **com}))
        if mn not in ('cmp', 'test') and not nowrite: s += self.wrop(dst, 'r', S)
        return s + '}'

    def shift(self, mn, S, ops, need):
        T = 'uint%d_t' % (8 * S); ST_ = 'int%d_t' % (8 * S); B = 8 * S; sb = B - 1
        a = self.rdop(ops[0], S)
        cnt = self.rdop(ops[1], 1) if len(ops) > 1 else '1'
        mask = 63 if S == 8 else 31
        pre = '{%s a=%s; unsigned cnt=(unsigned)(%s)&%d; if(cnt){ uint64_t w=a;' % (T, a, cnt, mask)
        zsp = dict(zf='r==0', sf='(r>>%d)&1' % sb, pf='PAR(r)')
        if mn in ('shl', 'sal'):
            body = '%s r=(%s)(w<<cnt);' % (T, T) + self.fl(need, cf='cnt<=%d?(w>>(%d-cnt))&1:0' % (B, B), of='((r>>%d)&1)^(cnt<=%d?(w>>(%d-cnt))&1:0)' % (sb, B, B), **zsp)
        elif mn == 'shr':
            body = '%s r=(%s)(w>>cnt);' % (T, T) + self.fl(need, cf='(w>>(cnt-1))&1', of='(a>>%d)&1' % sb, **zsp)
        elif mn == 'sar':
            body = '%s s=(%s)a; unsigned k=cnt>%d?%d:cnt; %s r=(%s)(s>>k);' % (ST_, ST_, sb, sb, T, T) + \
                   self.fl(need, cf='(s>>(k-1))&1', of='0', **zsp)
        elif mn in ('rol', 'ror'):
            sh = ('(w<<k)|(w>>(%d-k))' if mn == 'rol' else '(w>>k)|(w<<(%d-k))') % B
            body = 'unsigned k=cnt%%%d; %s r=k?(%s)(%s):a;' % (B, T, T, sh)
            if mn == 'rol': body += self.fl(need, cf='r&1', of='((r>>%d)&1)^(r&1)' % sb)
            else: body += self.fl(need, cf='(r>>%d)&1' % sb, of='((r>>%d)^(r>>%d))&1' % (sb, sb - 1))
        elif mn in ('rcl', 'rcr'):
            mod = '%%%d' % (B + 1) if B < 32 else ''
            if mn == 'rcl':
                body = '%s r=a; unsigned k=cnt%s; while(k--){unsigned nc=(r>>%d)&1;r=(%s)((r<<1)|cf);cf=nc;}' % (T, mod, sb, T)
                body += self.fl(need, of='((r>>%d)&1)^cf' % sb)
            else:
                body = '%s r=a; unsigned k=cnt%s; while(k--){unsigned nc=r&1;r=(%s)((r>>1)|((%s)cf<<%d));cf=nc;}' % (T, mod, T, T, sb)
                body += self.fl(need, of='((r>>%d)^(r>>%d))&1' % (sb, sb - 1))
        else: raise Unsup(mn)
        return pre + body + self.wrop(ops[0], 'r', S) + '}}'

    def cond(self, mn, pre):
        return COND[mn[len(pre):]]

    # main translate ---------------------------------------------------------
    def tr(self, i, need, tgt):
        """returns C statements. tgt(addr)-> goto statement for a code address."""
        self.i = i; mn = i.mnemonic.split()[-1]; pre = i.mnemonic.split()[:-1]
        ops = i.operands; n = len(ops); S = ops[0].size if n else 8; nx = i.address + i.size
        rep = 0xF3 in i.prefix or 0xF2 in i.prefix; repne = 0xF2 in i.prefix
        lock = 'lock' in pre or 0xF0 in i.prefix
        xmm0 = n and ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg).startswith('xmm')
        xmm1 = n > 1 and ops[1].type == X86_OP_REG and self.i.reg_name(ops[1].reg).startswith('xmm')
        mm0 = n and ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg).startswith('mm')
        mm1 = n > 1 and ops[1].type == X86_OP_REG and self.i.reg_name(ops[1].reg).startswith('mm')
        fence = '__atomic_thread_fence(__ATOMIC_SEQ_CST);' if lock else ''
        if mn in ('nop', 'endbr64', 'pause', 'fnop', 'prefetcht0', 'prefetchnta', 'prefetcht1', 'prefetcht2', 'prefetchw', 'lfence', 'sfence', 'ud0') or mn.startswith('nop'): return ''
        if mn in ('cli', 'sti', 'invd'): return ''  # privileged/no-op in the hosted user-mode model
        if mn == 'mfence': return '__atomic_thread_fence(__ATOMIC_SEQ_CST);'
        if mn in ('mov', 'movabs') and not xmm0 and not xmm1 and not mm0 and not mm1:
            if ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg) in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'):
                return '%s=(uint16_t)%s;' % (self.sreg(ops[0]), self.rdop(ops[1], 2))
            if ops[1].type == X86_OP_REG and self.i.reg_name(ops[1].reg) in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'):
                return self.wrop(ops[0], self.sreg(ops[1]), S)
            return self.wrop(ops[0], self.rdop(ops[1], S), S)
        if mn in ('add', 'sub', 'and', 'or', 'xor', 'cmp', 'test', 'adc', 'sbb'): return fence + self.alu(mn, S, self.rdop(ops[0], S), self.rdop(ops[1], S), ops[0], need)
        if mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'): return self.shift(mn, S, ops, need)
        if mn in ('movzx', 'movsx', 'movsxd'):
            S1 = ops[1].size; T0 = 'uint%d_t' % (8 * S)
            v = '(%s)%s' % (T0, self.rdop(ops[1], S1)) if mn == 'movzx' else '(%s)(int%d_t)%s' % (T0, 8 * S1, self.rdop(ops[1], S1))
            return self.wrop(ops[0], v, S)
        if mn == 'lea': return self.wrop(ops[0], '(uint%d_t)%s' % (8 * S, self.addr(ops[1])), S)
        m32 = self.m32; PS = 4 if m32 else 8; PU, PO = ('PUSH32', 'POP32') if m32 else ('PUSH', 'POP')
        if mn == 'push':
            v = self.sreg(ops[0]) if ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg) in ('es', 'cs', 'ss', 'ds', 'fs', 'gs') else self.rdop(ops[0], PS)
            return '%s(%s);' % (PU, v)
        if mn == 'pop':
            if ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg) in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'):
                return '%s=(uint16_t)%s();' % (self.sreg(ops[0]), PO)
            return self.wrop(ops[0], '%s()' % PO, PS)
        if mn == 'leave': return 'r4=r5; r5=%s();' % PO
        if mn == 'enter':
            alloc = ops[0].imm & 0xffff; nesting = ops[1].imm & 31
            return '{uint%d_t frame; %s(r5); frame=r4;' % (8 * PS, PU) + \
                   ''.join('frame-=%d;%s(LD(uint%d_t,frame));' % (PS, PU, 8 * PS) for _ in range(max(0, nesting - 1))) + \
                   ('%s(frame);' % PU if nesting else '') + 'r5=r4;r4=(uint%d_t)(r4-%d);}' % (8 * PS, alloc)
        if m32 and mn in ('les', 'lds'):
            dst = self.wrop(ops[0], 'LD(uint32_t,%s)' % self.addr(ops[1]), 4)
            return '{%sc->sregs[%d]=LD(uint16_t,%s+4);}' % (dst, 0 if mn == 'les' else 3, self.addr(ops[1]))
        if mn == 'xlatb':
            a = '(uint32_t)(r3+(uint8_t)r0)' if m32 else '(r3+(uint8_t)r0)'
            return 'r0=(r0&~0xFFULL)|LD(uint8_t,%s);' % a
        if mn == 'salc': return 'r0=(r0&~0xFFULL)|(cf?0xFF:0);'
        if mn in ('aaa', 'aas'):
            adj = 6 if mn == 'aaa' else -6; carry = 1 if mn == 'aaa' else -1
            return '{uint8_t al=(uint8_t)r0;if((al&15)>9){r0=(r0&~0xFFFFULL)|(uint16_t)(r0%+d);cf=1;}else cf=0;r0=(r0&~0xFFULL)|((uint8_t)r0&15);}' % (adj + (carry << 8))
        if mn == 'aam':
            base = ops[0].imm & 255 if n else 10
            if not base: return 'aot_unimpl(0x%xULL,"aam divide by zero");' % i.address
            return '{uint8_t al=(uint8_t)r0;uint16_t ax=(uint16_t)((al/%d)<<8)|(al%%%d);r0=(r0&~0xFFFFULL)|ax;zf=(uint8_t)ax==0;sf=((uint8_t)ax>>7)&1;pf=PAR(ax);}' % (base, base)
        if mn == 'aad':
            base = ops[0].imm & 255 if n else 10
            return '{uint8_t al=(uint8_t)r0+(uint8_t)(r0>>8)*%d;r0=(r0&~0xFFFFULL)|al;zf=al==0;sf=al>>7;pf=PAR(al);}' % base
        if mn in ('daa', 'das'):
            if mn == 'daa':
                return '{uint8_t al=(uint8_t)r0,old=al;unsigned oc=cf;cf=0;if((al&15)>9)al+=6;if(oc||old>0x99){al+=0x60;cf=1;}r0=(r0&~0xFFULL)|al;zf=al==0;sf=al>>7;pf=PAR(al);}'
            return '{uint8_t al=(uint8_t)r0,old=al;unsigned oc=cf;if((al&15)>9)al-=6;if(oc||old>0x99){al-=0x60;cf=1;}r0=(r0&~0xFFULL)|al;zf=al==0;sf=al>>7;pf=PAR(al);}'
        if mn == 'arpl':
            return '{uint16_t a=%s,b=%s;unsigned na=(a&3),nb=(b&3);zf=na<nb;if(zf)a=(a&~3)|nb;%s}' % (self.rdop(ops[0], 2), self.rdop(ops[1], 2), self.wrop(ops[0], 'a', 2))
        if mn == 'bound':
            return '{int%d_t v=(int%d_t)%s,lo=LD(int%d_t,%s),hi=LD(int%d_t,%s+%d);if(v<lo||v>hi)aot_unimpl(0x%xULL,"bound range exception");}' % ((8 * S, 8 * S, self.rdop(ops[0], S), 8 * S, self.addr(ops[1]), 8 * S, self.addr(ops[1]), S, i.address))
        if m32 and mn in ('pushal', 'pushad'): return '{uint32_t sp=(uint32_t)r4;PUSH32(r0);PUSH32(r1);PUSH32(r2);PUSH32(r3);PUSH32(sp);PUSH32(r5);PUSH32(r6);PUSH32(r7);}'
        if m32 and mn in ('popal', 'popad'): return 'r7=POP32();r6=POP32();r5=POP32();(void)POP32();r3=POP32();r2=POP32();r1=POP32();r0=POP32();'
        if m32 and mn in ('pushfd', 'pushf'): return 'PUSH32(cf|(pf<<2)|(zf<<6)|(sf<<7)|((uint32_t)c->df<<10)|(of<<11)|2);'
        if m32 and mn in ('popfd', 'popf'): return '{uint32_t f=POP32();cf=f&1;pf=(f>>2)&1;zf=(f>>6)&1;sf=(f>>7)&1;c->df=(f>>10)&1;of=(f>>11)&1;}'
        if m32 and mn == 'lahf': return 'r0=(r0&~0xFF00ULL)|((uint64_t)(uint8_t)(cf|(pf<<2)|(zf<<6)|(sf<<7)|2)<<8);'
        if m32 and mn == 'sahf': return '{uint8_t f=(uint8_t)(r0>>8);cf=f&1;pf=(f>>2)&1;zf=(f>>6)&1;sf=(f>>7)&1;}'
        if lock and ops[0].type == X86_OP_MEM and mn in ('add', 'sub', 'and', 'or', 'xor', 'inc', 'dec', 'xadd', 'cmpxchg'):
            T = 'uint%d_t' % (8 * S); p = '(%s*)%s' % (T, self.addr(ops[0])); one = mn in ('inc', 'dec')
            if mn == 'cmpxchg':
                return '{%s e=(%s)r0,*p=%s;int ok=__atomic_compare_exchange_n(p,&e,%s,0,5,5);%s if(!ok){%s}}' % (T, T, p, self.rdop(ops[1], S), self.alu('cmp', S, '(%s)r0' % T, 'e', None, F5), self.wrop(REGOP_RAX[S], 'e', S))
            b = '1' if one else self.rdop(ops[1], S); f = {'inc': 'add', 'dec': 'sub', 'xadd': 'add'}.get(mn, mn)
            fn = {'add': 'add', 'sub': 'sub', 'and': 'and', 'or': 'or', 'xor': 'xor'}[f]
            s_ = '{%s ob=%s;%s old=__atomic_fetch_%s(%s,ob,5);' % (T, b, T, fn, p)
            if mn == 'xadd': s_ += self.wrop(ops[1], 'old', S)
            return s_ + self.alu(f, S, 'old', 'ob', ops[0], need, nowrite=True) + '}'
        if mn == 'xchg' and any(o.type == X86_OP_MEM for o in ops):
            T = 'uint%d_t' % (8 * S); m, r = (ops[0], ops[1]) if ops[0].type == X86_OP_MEM else (ops[1], ops[0])
            return '{%s old=__atomic_exchange_n((%s*)%s,%s,5);%s}' % (T, T, self.addr(m), self.rdop(r, S), self.wrop(r, 'old', S))
        if mn == 'xchg': return '{uint%d_t a=%s,b=%s;%s%s}__atomic_thread_fence(__ATOMIC_SEQ_CST);' % (8 * S, self.rdop(ops[0], S), self.rdop(ops[1], S), self.wrop(ops[0], 'b', S), self.wrop(ops[1], 'a', S))
        if mn in ('inc', 'dec'):
            T = 'uint%d_t' % (8 * S); sb = 8 * S - 1; d = '+' if mn == 'inc' else '-'
            ov = 'r==(%s)(1ULL<<%d)' % (T, sb) if mn == 'inc' else 'a==(%s)(1ULL<<%d)' % (T, sb)
            return '{%s a=%s,r=a%s1;%s%s}' % (T, self.rdop(ops[0], S), d, self.fl(need, zf='r==0', sf='(r>>%d)&1' % sb, pf='PAR(r)', of=ov), self.wrop(ops[0], 'r', S)) if not lock else \
                   '{%s a=%s,r=a%s1;%s%s}' % (T, self.rdop(ops[0], S), d, self.fl(need, zf='r==0', sf='(r>>%d)&1' % sb, pf='PAR(r)', of=ov), self.wrop(ops[0], 'r', S))
        if mn == 'neg':
            T = 'uint%d_t' % (8 * S); sb = 8 * S - 1
            return '{%s a=%s,r=0-a;%s%s}' % (T, self.rdop(ops[0], S), self.fl(need, cf='a!=0', of='a==(%s)(1ULL<<%d)' % (T, sb), zf='r==0', sf='(r>>%d)&1' % sb, pf='PAR(r)'), self.wrop(ops[0], 'r', S))
        if mn == 'not': return self.wrop(ops[0], '~' + self.rdop(ops[0], S), S)
        if mn in ('mul', 'imul', 'div', 'idiv') and (n == 1 or (mn in ('mul', 'div', 'idiv'))): return self.muldiv(mn, S, ops[0], need)
        if mn == 'imul':
            T = 'uint%d_t' % (8 * S); ST_ = 'int%d_t' % (8 * S)
            a, b = (self.rdop(ops[0], S), self.rdop(ops[1], S)) if n == 2 else (self.rdop(ops[1], S), self.rdop(ops[2], S))
            return '{%s a=%s,b=%s,r=(%s)(a*b); int ov=((__int128)(%s)a*(%s)b)!=(__int128)(%s)r;%s%s}' % (T, a, b, T, ST_, ST_, ST_, self.fl(need, cf='ov', of='ov', zf='0', sf='0', pf='0'), self.wrop(ops[0], 'r', S))
        if mn in ('cdq', 'cqo', 'cwd'):
            return {'cdq': 'r2=(uint32_t)((int32_t)r0>>31);', 'cqo': 'r2=(uint64_t)((int64_t)r0>>63);', 'cwd': 'r2=(r2&~0xFFFFULL)|(uint16_t)((int16_t)r0>>15);'}[mn]
        if mn == 'cdqe': return 'r0=(uint64_t)(int64_t)(int32_t)r0;'
        if mn == 'cwde': return 'r0=(uint32_t)(int32_t)(int16_t)r0;'
        if mn == 'cbw': return 'r0=(r0&~0xFFFFULL)|(uint16_t)(int16_t)(int8_t)r0;'
        if mn == 'bswap': return self.wrop(ops[0], '__builtin_bswap%d(%s)' % (8 * S, self.rdop(ops[0], S)), S)
        if mn in ('bt', 'bts', 'btr', 'btc'):
            T = 'uint%d_t' % (8 * S); bit = '(%s)%%%d' % (self.rdop(ops[1], S), 8 * S)
            w = {'bts': '|(1ULL<<k)', 'btr': '&~(1ULL<<k)', 'btc': '^(1ULL<<k)'}.get(mn)
            return '{%s a=%s; unsigned k=%s; cf=(a>>k)&1;%s}' % (T, self.rdop(ops[0], S), bit, self.wrop(ops[0], '(%s)(a%s)' % (T, w), S) if w else '')
        if mn in ('bsf', 'bsr', 'tzcnt', 'lzcnt', 'popcnt'):
            T = 'uint%d_t' % (8 * S); B = 8 * S; s = '%s v=%s;' % (T, self.rdop(ops[1], S))
            if mn == 'bsf': return '{%sif(v){zf=0;%s}else zf=1;}' % (s, self.wrop(ops[0], '__builtin_ctzll(v)', S))
            if mn == 'bsr': return '{%sif(v){zf=0;%s}else zf=1;}' % (s, self.wrop(ops[0], '63-__builtin_clzll(v)', S))
            if mn == 'tzcnt': return '{%scf=v==0;zf=0;%s}' % (s, self.wrop(ops[0], 'v?__builtin_ctzll(v):%d' % B, S))
            if mn == 'lzcnt': return '{%scf=v==0;zf=0;%s}' % (s, self.wrop(ops[0], 'v?__builtin_clzll(v)-%d:%d' % (64 - B, B), S))
            return '{%szf=v==0;cf=0;of=0;sf=0;pf=0;%s}' % (s, self.wrop(ops[0], '__builtin_popcountll(v)', S))
        if mn in ('cmpxchg', 'xadd'):
            T = 'uint%d_t' % (8 * S)
            if mn == 'xadd': return '{%s xa=%s,xb=%s;%s%s}' % (T, self.rdop(ops[0], S), self.rdop(ops[1], S), self.wrop(ops[1], 'xa', S), self.alu('add', S, 'xa', 'xb', ops[0], need))
            return '{%s ca=%s,acc=(%s)r0; %s if(acc==ca){%s}else{%s}}' % (T, self.rdop(ops[0], S), T, self.alu('cmp', S, 'acc', 'ca', None, F5), self.wrop(ops[0], self.rdop(ops[1], S), S), self.wrop(REGOP_RAX[S], 'ca', S))
        if mn[:3] == 'set' and mn[3:] in COND: return self.wrop(ops[0], '(%s)' % COND[mn[3:]], 1)
        if mn[:4] == 'cmov' and mn[4:] in COND:
            T = 'uint%d_t' % (8 * S)
            el = ''
            if S == 4: el = 'else r%d=(uint32_t)r%d;' % (self.reg(ops[0].reg)[0], self.reg(ops[0].reg)[0])
            return '{%s v=%s; if(%s){%s}%s}' % (T, self.rdop(ops[1], S), COND[mn[4:]], self.wrop(ops[0], 'v', S), el)
        if mn in ('clc', 'stc', 'cmc'): return {'clc': 'cf=0;', 'stc': 'cf=1;', 'cmc': 'cf=!cf;'}[mn]
        if mn in ('cld', 'std'): return 'c->df=%d;' % (mn == 'std')
        if mn == 'cpuid': return 'r0=r1=r2=r3=0;'
        if mn == 'rdtsc': return '{uint64_t t=aot_rdtsc(); r0=(uint32_t)t; r2=t>>32;}'
        if mn in ('shld', 'shrd'):
            T = 'uint%d_t' % (8 * S); B = 8 * S; cnt = '(unsigned)(%s)&%d' % (self.rdop(ops[2], 1), 63 if S == 8 else 31)
            sh = '(a<<k)|(b>>(%d-k))' % B if mn == 'shld' else '(a>>k)|(b<<(%d-k))' % B
            return '{%s a=%s,b=%s; unsigned k=%s; if(k){ %s r=(%s)(%s); cf=%s;zf=r==0;sf=(r>>%d)&1;pf=PAR(r);%s}}' % (
                T, self.rdop(ops[0], S), self.rdop(ops[1], S), cnt, T, T, sh, '(a>>(%d-k))&1' % B if mn == 'shld' else '(a>>(k-1))&1', B - 1, self.wrop(ops[0], 'r', S))
        # control flow -----------------------------------------------------------
        if mn == 'ret': return ('{pc=POP32();%s goto dispatch;}' % ('r4=(uint32_t)(r4+%d);' % ops[0].imm if n else '')) if m32 else '{pc=POP();%s goto dispatch;}' % ('r4+=%d;' % ops[0].imm if n else '')
        if m32 and mn == 'retf': return '{pc=POP32();(void)POP32();%s goto dispatch;}' % ('r4=(uint32_t)(r4+%d);' % ops[0].imm if n else '')
        if m32 and mn in ('iret', 'iretd'):
            return '{pc=POP32();(void)POP32();uint32_t f=POP32();cf=f&1;pf=(f>>2)&1;zf=(f>>6)&1;sf=(f>>7)&1;c->df=(f>>10)&1;of=(f>>11)&1;goto dispatch;}'
        if mn in ('jmp', 'ljmp'):
            op = ops[-1] if mn == 'ljmp' else ops[0]
            if op.type == X86_OP_IMM: return tgt(op.imm)
            return '{pc=%s; goto dispatch;}' % self.rdop(op, PS)
        if mn[0] == 'j' and mn[1:] in COND: return 'if(%s){%s}' % (COND[mn[1:]], tgt(ops[0].imm))
        if mn.startswith('loop'):
            dec = 'r1=(uint32_t)(r1-1)' if m32 else 'r1--'
            cc = 'r1' if mn == 'loop' else 'r1&&%s' % ('zf' if mn == 'loope' else '!zf')
            return '%s;if(%s){%s}' % (dec, cc, tgt(ops[0].imm))
        if mn in ('call', 'lcall'):
            op = ops[-1] if mn == 'lcall' else ops[0]
            if mn == 'lcall':
                if not m32: raise Unsup(mn)
                if op.type == X86_OP_IMM: return 'PUSH32(c->sregs[1]);PUSH32(0x%xULL);%s' % (nx, tgt(op.imm))
                return '{uint64_t a=%s;uint32_t t=LD(uint32_t,a);PUSH32(c->sregs[1]);PUSH32(0x%xULL);pc=t;goto dispatch;}' % (self.addr(op), nx)
            if op.type == X86_OP_IMM: return '%s(0x%xULL);%s' % (PU, nx, tgt(op.imm))
            return '{uint64_t t=%s; %s(0x%xULL); pc=t; goto dispatch;}' % (self.rdop(op, PS), PU, nx)
        if mn == 'into': return 'if(of)aot_unimpl(0x%xULL,"into overflow exception");' % i.address
        if mn == 'sidt': return 'ST(uint16_t,%s,0);ST(uint32_t,%s+2,0);' % (self.addr(ops[0]), self.addr(ops[0]))
        if mn == 'str': return self.wrop(ops[0], '0', S)
        if mn in ('ud2', 'int3', 'int1', 'hlt', 'int', 'syscall', 'in', 'out'): return 'aot_unimpl(0x%xULL,"%s %s");' % (i.address, mn, i.op_str)
        # string ops (hosted I/O: output is discarded, input bytes are zeroed)
        if mn in ('outsb', 'outsw', 'outsd', 'insb', 'insw', 'insd'):
            z = mn[-1]; sz = {'b': 1, 'w': 2, 'd': 4}[z]
            if mn.startswith('outs'):
                body = 'r6=(uint32_t)(r6+(c->df?-%d:%d));' % (sz, sz) if m32 else 'r6+=c->df?-%d:%d;' % (sz, sz)
            else:
                body = 'ST(uint%d_t,r7,0);' % (8 * sz) + ('r7=(uint32_t)(r7+(c->df?-%d:%d));' % (sz, sz) if m32 else 'r7+=c->df?-%d:%d;' % (sz, sz))
            return '{while(r1){%sr1--;}}' % body if rep else '{' + body + '}'
        sm = mn[:-1] if mn[:-1] in ('movs', 'stos', 'lods', 'scas', 'cmps') and mn[-1] in 'bwdq' and not (xmm0 or xmm1) else None
        if sm:
            S = {'b': 1, 'w': 2, 'd': 4, 'q': 8}[mn[-1]]; T = 'uint%d_t' % (8 * S)
            step = 'c->df?-%d:%d' % (S, S); acc = {1: '(uint8_t)r0', 2: '(uint16_t)r0', 4: '(uint32_t)r0', 8: 'r0'}[S]
            body = {'movs': 'ST(%s,r7,LD(%s,r6));r6+=%s;r7+=%s;' % (T, T, step, step), 'stos': 'ST(%s,r7,%s);r7+=%s;' % (T, acc, step),
                    'lods': {1: 'r0=(r0&~0xFFULL)|LD(%s,r6);' % T, 2: 'r0=(r0&~0xFFFFULL)|LD(%s,r6);' % T, 4: 'r0=LD(%s,r6);' % T, 8: 'r0=LD(%s,r6);' % T}[S] + 'r6+=%s;' % step,
                    'scas': '{%s v=LD(%s,r7);%s}r7+=%s;' % (T, T, self.alu('cmp', S, acc, 'v', None, F5), step),
                    'cmps': '{%s a=LD(%s,r6),b=LD(%s,r7);%s}r6+=%s;r7+=%s;' % (T, T, T, self.alu('cmp', S, 'a', 'b', None, F5), step, step)}[sm]
            if not rep: return '{' + body + '}'
            brk = ''
            if sm in ('scas', 'cmps'): brk = 'if(%s) break;' % ('zf' if repne else '!zf')
            return '{while(r1){%sr1--;%s}}' % (body, brk)
        if mn[0] == 'f' or mn == 'wait': return self.x87(i, mn, ops, n, need)
        if mn[0] == 'v' and mn not in ('verr', 'verw'): return self.avx(i, mn, ops, n, need)
        # SSE ------------------------------------------------------------------
        return self.sse(i, mn, ops, n, xmm0, xmm1, need)

    def muldiv(self, mn, S, op, need):
        T = 'uint%d_t' % (8 * S); ST_ = 'int%d_t' % (8 * S); B = 8 * S
        sg = mn in ('imul', 'idiv'); U = '__int128' if sg else 'unsigned __int128'; T_ = ST_ if sg else T
        v = self.rdop(op, S)
        if mn in ('mul', 'imul'):
            if S == 1: wr = 'r0=(r0&~0xFFFFULL)|(uint16_t)p;'; lo = '(uint8_t)p'; hi = '(uint8_t)(p>>8)'
            elif S == 2: wr = 'r0=(r0&~0xFFFFULL)|(uint16_t)p;r2=(r2&~0xFFFFULL)|(uint16_t)(p>>16);'
            elif S == 4: wr = 'r0=(uint32_t)p;r2=(uint32_t)(p>>32);'
            else: wr = 'r0=(uint64_t)p;r2=(uint64_t)(p>>64);'
            acc = {1: '(uint8_t)r0', 2: '(uint16_t)r0', 4: '(uint32_t)r0', 8: 'r0'}[S]
            ov = '(p!=(%s)(%s)p)' % (U, T_) if sg else '((p>>%d)!=0)' % B
            return '{%s p=(%s)(%s)%s*(%s)(%s)%s;%s%s}' % (U, U, T_, acc, U, T_, v, wr, self.fl(need, cf=ov, of=ov))
        if S == 1: num = '(%s)(%s)(uint16_t)r0' % (U, 'int16_t' if sg else 'uint16_t'); wr = 'r0=(r0&~0xFFFFULL)|(uint8_t)q|((uint16_t)(uint8_t)m<<8);'
        elif S == 2: num = '(%s)(%s)(((uint32_t)(uint16_t)r2<<16)|(uint16_t)r0)' % (U, 'int32_t' if sg else 'uint32_t'); wr = 'r0=(r0&~0xFFFFULL)|(uint16_t)q;r2=(r2&~0xFFFFULL)|(uint16_t)m;'
        elif S == 4: num = '(%s)(%s)(((uint64_t)(uint32_t)r2<<32)|(uint32_t)r0)' % (U, 'int64_t' if sg else 'uint64_t'); wr = 'r0=(uint32_t)q;r2=(uint32_t)m;'
        else: num = '(%s)(((unsigned __int128)r2<<64)|r0)' % U; wr = 'r0=(uint64_t)q;r2=(uint64_t)m;'
        return '{%s d=(%s)(%s)%s; %s n=%s; %s q=n/d, m=n%%d; %s}' % (U, U, T_, v, U, num, U, wr)

    def sse(self, i, mn, ops, n, xmm0, xmm1, need):
        D = self.xr(ops[0]) if xmm0 else None
        def xs(op, kind):  # scalar source of float 'f' / double 'g'
            if op.type == X86_OP_REG: return '%s.%s[0]' % (self.xr(op), kind)
            return 'LD(%s,%s)' % ('float' if kind == 'f' else 'double', self.addr(op))
        def xv(op): return self.xr(op) if op.type == X86_OP_REG else 'LDX(%s)' % self.addr(op)
        mm0 = n and ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg).startswith('mm')
        mm1 = n > 1 and ops[1].type == X86_OP_REG and self.i.reg_name(ops[1].reg).startswith('mm')
        def mmv(op):
            return self.mmr(op) if op.type == X86_OP_REG else '({MM _v={.q=LD(uint64_t,%s)};_v;})' % self.addr(op)
        if mn == 'emms': return ''
        if mn in ('movd', 'movq') and (mm0 or mm1):
            S = 4 if mn == 'movd' else 8; T = 'uint%d_t' % (8 * S)
            if mm0:
                if mm1: return '%s.q=%s.q;' % (self.mmr(ops[0]), self.mmr(ops[1]))
                if xmm1: return '%s.q=%s.q[0];' % (self.mmr(ops[0]), self.xr(ops[1]))
                return '%s.q=(uint64_t)(%s)%s;' % (self.mmr(ops[0]), T, self.rdop(ops[1], S))
            if xmm0: return '%s.q[0]=%s.q;%s.q[1]=0;' % (D, self.mmr(ops[1]), D)
            return self.wrop(ops[0], '(%s)%s.q' % (T, self.mmr(ops[1])), S)
        if mn == 'pextrw':
            wide = ops[1].type == X86_OP_REG and self.i.reg_name(ops[1].reg).startswith('xmm')
            src = self.xr(ops[1]) if wide else self.mmr(ops[1])
            return self.wrop(ops[0], '%s.w[%d]' % (src, ops[2].imm & (7 if wide else 3)), ops[0].size)
        if mn == 'pinsrw':
            dst = D if xmm0 else self.mmr(ops[0])
            src = 'LD(uint16_t,%s)' % self.addr(ops[1]) if ops[1].type == X86_OP_MEM else self.rdop(ops[1], 2)
            return '%s.w[%d]=(uint16_t)%s;' % (dst, ops[2].imm & (7 if xmm0 else 3), src)
        if mn == 'cvttps2pi':
            if ops[1].type == X86_OP_MEM:
                a, b = 'LD(float,%s)' % self.addr(ops[1]), 'LD(float,%s+4)' % self.addr(ops[1])
            else:
                a, b = '%s.f[0]' % self.xr(ops[1]), '%s.f[1]' % self.xr(ops[1])
            return '%s.d[0]=(uint32_t)aot_d2i(%s,32);%s.d[1]=(uint32_t)aot_d2i(%s,32);' % (self.mmr(ops[0]), a, self.mmr(ops[0]), b)
        if mn == 'cvtpi2ps':
            src = mmv(ops[1])
            return '{MM s=%s;%s.f[0]=(float)(int32_t)s.d[0];%s.f[1]=(float)(int32_t)s.d[1];}' % (src, D, D)
        if mn == 'pshufw':
            return '{MM s=%s;unsigned m=%d;for(int j=0;j<4;j++)%s.w[j]=s.w[(m>>(2*j))&3];}' % (mmv(ops[1]), ops[2].imm & 255, self.mmr(ops[0]))
        if mm0:
            M = self.mmr(ops[0])
            if mn in ('por', 'pand', 'pxor', 'pandn'):
                op = {'por': '|', 'pand': '&', 'pxor': '^'}.get(mn)
                e = 'd.q&~s.q' if mn == 'pandn' else 'd.q%ss.q' % op
                return '{MM s=%s,d=%s;%s.q=%s;}' % (mmv(ops[1]), M, M, e)
            lanes = {'paddb': ('b', 8, '+'), 'psubb': ('b', 8, '-'), 'paddw': ('w', 4, '+'), 'psubw': ('w', 4, '-'),
                     'paddd': ('d', 2, '+'), 'psubd': ('d', 2, '-'), 'pmullw': ('w', 4, '*'), 'pcmpgtw': ('w', 4, '>')}
            if mn in lanes:
                k, cnt, op = lanes[mn]
                e = '-((int16_t)d.w[j]>(int16_t)s.w[j])' if mn == 'pcmpgtw' else 'd.%s[j]%ss.%s[j]' % (k, op, k)
                return '{MM s=%s,d=%s;for(int j=0;j<%d;j++)%s.%s[j]=%s;}' % (mmv(ops[1]), M, cnt, M, k, e)
            if mn in ('paddq', 'psubq'):
                return '{MM s=%s,d=%s;%s.q=d.q%ss.q;}' % (mmv(ops[1]), M, M, '+' if mn == 'paddq' else '-')
            if mn in ('paddsb', 'psubsb'):
                op = '+' if mn == 'paddsb' else '-'
                return '{MM s=%s,d=%s;for(int j=0;j<8;j++)%s.b[j]=({int v=(int8_t)d.b[j]%s(int8_t)s.b[j];(uint8_t)(v>127?127:v<-128?-128:v);});}' % (mmv(ops[1]), M, M, op)
            if mn in ('pmulhw', 'pmaddwd', 'paddsw', 'psubsw', 'paddusw', 'psubusb'):
                if mn == 'pmulhw': k, cnt, e = 'w', 4, '(uint16_t)(((int32_t)(int16_t)d.w[j]*(int16_t)s.w[j])>>16)'
                elif mn == 'pmaddwd': k, cnt, e = 'd', 2, '(uint32_t)((int32_t)(int16_t)d.w[2*j]*(int16_t)s.w[2*j]+(int32_t)(int16_t)d.w[2*j+1]*(int16_t)s.w[2*j+1])'
                elif mn in ('paddsw', 'psubsw'):
                    op = '+' if mn == 'paddsw' else '-'; k, cnt = 'w', 4
                    e = '({int v=(int16_t)d.w[j]%s(int16_t)s.w[j];(uint16_t)(v>32767?32767:v<-32768?-32768:v);})' % op
                elif mn == 'paddusw': k, cnt, e = 'w', 4, '(int)d.w[j]+s.w[j]>65535?65535:d.w[j]+s.w[j]'
                else: k, cnt, e = 'b', 8, 'd.b[j]>s.b[j]?d.b[j]-s.b[j]:0'
                return '{MM s=%s,d=%s;for(int j=0;j<%d;j++)%s.%s[j]=%s;}' % (mmv(ops[1]), M, cnt, M, k, e)
            if mn.startswith('ps') and mn[2:5] in ('lld', 'rld', 'rad', 'llq', 'rlq', 'llw', 'rlw', 'raw'):
                L = mn[-1]; k = {'d': 'd', 'q': 'q', 'w': 'w'}[L]; B = {'d': 32, 'q': 64, 'w': 16}[L]; cntn = B // 8
                o1 = ops[1]
                cnt = '%d' % (o1.imm & 255) if o1.type == X86_OP_IMM else ('%s.q' % self.mmr(o1) if o1.type == X86_OP_REG else 'LD(uint64_t,%s)' % self.addr(o1))
                if mn[2] == 'l': e = 'cnt>%d?0:d.q<<cnt' % (B - 1) if L == 'q' else 'cnt>%d?0:d.%s[j]<<cnt' % (B - 1, k)
                elif mn.startswith('psra'): e = '(%s)((int%d_t)d.%s[j]>>(cnt>%d?%d:cnt))' % ({'w': 'uint16_t', 'd': 'uint32_t'}[L], B, k, B - 1, B - 1)
                else: e = 'cnt>%d?0:d.q>>cnt' % (B - 1) if L == 'q' else 'cnt>%d?0:d.%s[j]>>cnt' % (B - 1, k)
                lp = '%s.q=%s;' % (M, e) if L == 'q' else 'for(int j=0;j<%d;j++)%s.%s[j]=%s;' % (cntn, M, k, e)
                return '{MM d=%s;uint64_t cnt=%s;%s}' % (M, cnt, lp)
            if mn in ('packssdw', 'packsswb'):
                if mn == 'packssdw':
                    return '{MM s=%s,d=%s;for(int j=0;j<2;j++){int v=(int32_t)d.d[j];%s.w[j]=(uint16_t)(v>32767?32767:v<-32768?-32768:v);v=(int32_t)s.d[j];%s.w[j+2]=(uint16_t)(v>32767?32767:v<-32768?-32768:v);}}' % (mmv(ops[1]), M, M, M)
                return '{MM s=%s,d=%s;for(int j=0;j<4;j++){int v=(int16_t)d.w[j];%s.b[j]=(uint8_t)(v>127?127:v<-128?-128:v);v=(int16_t)s.w[j];%s.b[j+4]=(uint8_t)(v>127?127:v<-128?-128:v);}}' % (mmv(ops[1]), M, M, M)
            unpacks = {'punpcklwd': ('w', 0), 'punpckhwd': ('w', 2), 'punpckldq': ('d', 0), 'punpckhdq': ('d', 1), 'punpcklbw': ('b', 0), 'punpckhbw': ('b', 4)}
            if mn in unpacks:
                k, off = unpacks[mn]; cnt = 4 // {'b': 1, 'w': 2, 'd': 4}[k]
                return '{MM s=%s,d=%s;for(int j=0;j<%d;j++){%s.%s[2*j]=d.%s[j+%d];%s.%s[2*j+1]=s.%s[j+%d];}}' % (mmv(ops[1]), M, cnt, M, k, k, off, M, k, k, off)
        if mn in ('movaps', 'movups', 'movdqa', 'movdqu', 'movapd', 'movupd', 'lddqu'): 
            return 'STX(%s,%s);' % (self.addr(ops[0]), self.xr(ops[1])) if not xmm0 else '%s=%s;' % (D, xv(ops[1]))
        if mn in ('movss', 'movsd'):
            k, S = ('f', 4) if mn == 'movss' else ('g', 8); T = 'float' if k == 'f' else 'double'
            if not xmm0: return 'ST(%s,%s,%s);' % (T, self.addr(ops[0]), self.xr(ops[1]) + '.%s[0]' % k)
            if ops[1].type == X86_OP_MEM: return '{%s v=LD(%s,%s);%s.q[0]=0;%s.q[1]=0;%s.%s[0]=v;}' % (T, T, self.addr(ops[1]), D, D, D, k)
            return '%s.%s[0]=%s.%s[0];' % (D, k, self.xr(ops[1]), k)
        if mn in ('movd', 'movq'):
            S = 4 if mn == 'movd' else 8; T = 'uint%d_t' % (8 * S)
            if xmm0:
                if xmm1: return '{uint64_t v=%s.q[0];%s.q[0]=v;%s.q[1]=0;}' % (self.xr(ops[1]), D, D)
                return '{%s v=%s;%s.q[0]=v;%s.q[1]=0;}' % (T, self.rdop(ops[1], S), D, D)
            return self.wrop(ops[0], '(%s)%s.q[0]' % (T, self.xr(ops[1])), S)
        if mn == 'stmxcsr': return 'ST(uint32_t,%s,c->mxcsr);' % self.addr(ops[0])
        if mn == 'ldmxcsr': return 'c->mxcsr=LD(uint32_t,%s)&0xFFFF;' % self.addr(ops[0])
        PRED = {'eq': 'a==b', 'lt': 'a<b', 'le': 'a<=b', 'unord': '(a!=a||b!=b)', 'neq': '!(a==b)', 'nlt': '!(a<b)', 'nle': '!(a<=b)', 'ord': '(a==a&&b==b)'}
        if mn.startswith('cmp') and mn[-2:] in ('ps', 'pd', 'ss', 'sd') and mn[3:-2] in PRED:
            suffix = mn[-2:]; dbl = suffix in ('pd', 'sd'); scalar = suffix in ('ss', 'sd'); pred = mn[3:-2]
            k = 'g' if dbl else 'f'; lane = 'q' if dbl else 'd'; mask = 'UINT64_MAX' if dbl else 'UINT32_MAX'; T = 'double' if dbl else 'float'
            if scalar:
                return '{%s a=%s.%s[0],b=%s;%s.%s[0]=(%s)?%s:0;}' % (T, D, k, xs(ops[1], k), D, lane, PRED[pred], mask)
            return '{XMM s=%s,d=%s;for(int j=0;j<%d;j++){%s a=d.%s[j],b=s.%s[j];%s.%s[j]=(%s)?%s:0;}}' % (xv(ops[1]), D, 2 if dbl else 4, T, k, k, D, lane, PRED[pred], mask)
        if mn == 'unpckhps': return '{XMM s=%s,d=%s;%s.f[0]=d.f[2];%s.f[1]=s.f[2];%s.f[2]=d.f[3];%s.f[3]=s.f[3];}' % (xv(ops[1]), D, D, D, D, D)
        if mn == 'unpckhpd': return '{XMM s=%s,d=%s;%s.q[0]=d.q[1];%s.q[1]=s.q[1];}' % (xv(ops[1]), D, D, D)
        if mn == 'shufpd': return '{XMM s=%s,d=%s;unsigned m=%d;%s.q[0]=d.q[m&1];%s.q[1]=s.q[(m>>1)&1];}' % (xv(ops[1]), D, ops[2].imm & 255, D, D)
        if mn in ('rsqrtss', 'rcpss'):
            fn = '1.0f/sqrtf(s)' if mn == 'rsqrtss' else '1.0f/s'
            return '{float s=%s;%s.f[0]=%s;}' % (xs(ops[1], 'f'), D, fn)
        if mn in ('rsqrtps', 'rcpps'):
            fn = '1.0f/sqrtf(s.f[j])' if mn == 'rsqrtps' else '1.0f/s.f[j]'
            return '{XMM s=%s;for(int j=0;j<4;j++)%s.f[j]=%s;}' % (xv(ops[1]), D, fn)
        if not xmm0 and mn in ('movlps', 'movlpd', 'movhps', 'movhpd', 'movntdq', 'movntps', 'movntpd', 'movnti') and n == 2 and ops[1].type == X86_OP_REG:
            if mn.startswith('movnt'): return 'STX(%s,%s);' % (self.addr(ops[0]), self.xr(ops[1])) if mn != 'movnti' else self.wrop(ops[0], self.rdop(ops[1]), ops[1].size)
            return 'ST(uint64_t,%s,%s.q[%d]);' % (self.addr(ops[0]), self.xr(ops[1]), 1 if 'h' in mn else 0)
        if not xmm0 and not xmm1 and mn not in ('cvttsd2si', 'cvttss2si', 'cvtsd2si', 'cvtss2si'):
            raise Unsup(mn)
        if mn in ('movlps', 'movlpd', 'movhps', 'movhpd') and ops[0].type == X86_OP_MEM:
            return 'ST(uint64_t,%s,%s.q[%d]);' % (self.addr(ops[0]), self.xr(ops[1]), 1 if 'h' in mn else 0)
        if D is None and mn not in ('cvttsd2si', 'cvttss2si', 'cvtsd2si', 'cvtss2si', 'movmskpd', 'movmskps', 'pmovmskb'): raise Unsup(mn)
        logic = {'pxor': '^', 'xorps': '^', 'xorpd': '^', 'pand': '&', 'andps': '&', 'andpd': '&', 'por': '|', 'orps': '|', 'orpd': '|'}
        if mn in logic: return '{XMM s=%s;%s.q[0]%s=s.q[0];%s.q[1]%s=s.q[1];}' % (xv(ops[1]), D, logic[mn] + '', D, logic[mn])
        if mn in ('pandn', 'andnps', 'andnpd'): return '{XMM s=%s;%s.q[0]=~%s.q[0]&s.q[0];%s.q[1]=~%s.q[1]&s.q[1];}' % (xv(ops[1]), D, D, D, D)
        if mn in ('movlps', 'movlpd'): return '%s.q[0]=LD(uint64_t,%s);' % (D, self.addr(ops[1])) if ops[1].type == X86_OP_MEM else '%s.q[0]=%s.q[1];' % (D, self.xr(ops[1]))
        if mn in ('movhps', 'movhpd'): return '%s.q[1]=LD(uint64_t,%s);' % (D, self.addr(ops[1]))
        if mn == 'movlhps': return '%s.q[1]=%s.q[0];' % (D, self.xr(ops[1]))
        if mn == 'movhlps': return '%s.q[0]=%s.q[1];' % (D, self.xr(ops[1]))
        if mn in ('unpcklps', 'unpcklpd', 'punpcklqdq', 'punpckldq'):
            if mn == 'unpcklps' or mn == 'punpckldq': return '{XMM s=%s;XMM d=%s;%s.d[0]=d.d[0];%s.d[1]=s.d[0];%s.d[2]=d.d[1];%s.d[3]=s.d[1];}' % (xv(ops[1]), D, D, D, D, D)
            return '{XMM s=%s;%s.q[1]=s.q[0];}' % (xv(ops[1]), D)
        if mn == 'pshufd': return '{XMM s=%s;unsigned m=%d;for(int k=0;k<4;k++)%s.d[k]=s.d[(m>>(2*k))&3];}' % (xv(ops[1]), ops[2].imm & 255, D)
        if mn == 'shufps': return '{XMM s=%s,d=%s;unsigned m=%d;%s.f[0]=d.f[m&3];%s.f[1]=d.f[(m>>2)&3];%s.f[2]=s.f[(m>>4)&3];%s.f[3]=s.f[(m>>6)&3];}' % (xv(ops[1]), D, ops[2].imm & 255, D, D, D, D)
        if mn == 'movmskpd': return self.wrop(ops[0], '((%s.q[0]>>63)|((%s.q[1]>>63)<<1))' % (self.xr(ops[1]), self.xr(ops[1])), 4)
        if mn == 'movmskps': return self.wrop(ops[0], '((%s.d[0]>>31)|((%s.d[1]>>31)<<1)|((%s.d[2]>>31)<<2)|((%s.d[3]>>31)<<3))' % ((self.xr(ops[1]),) * 4), 4)
        m = {'add': '+', 'sub': '-', 'mul': '*', 'div': '/'}
        for sfx, k, T, cnt in (('ss', 'f', 'float', 1), ('sd', 'g', 'double', 1), ('ps', 'f', 'float', 4), ('pd', 'g', 'double', 2)):
            if mn[-2:] != sfx or mn[:-2] not in ('add', 'sub', 'mul', 'div', 'min', 'max', 'sqrt'): continue
            op = mn[:-2]
            if cnt == 1:
                s = xs(ops[1], k)
                e = {'min': '(%s.%s[0]<s?%s.%s[0]:s)' % (D, k, D, k), 'max': '(%s.%s[0]>s?%s.%s[0]:s)' % (D, k, D, k),
                     'sqrt': ('sqrtf(s)' if k == 'f' else 'sqrt(s)')}.get(op) or '%s.%s[0]%ss' % (D, k, m.get(op, ''))
                return '{%s s=%s;%s.%s[0]=%s;}' % (T, s, D, k, e)
            e = {'min': '(d.%s[j]<s.%s[j]?d.%s[j]:s.%s[j])' % ((k,) * 4), 'max': '(d.%s[j]>s.%s[j]?d.%s[j]:s.%s[j])' % ((k,) * 4),
                 'sqrt': ('sqrtf(s.f[j])' if k == 'f' else 'sqrt(s.g[j])')}.get(op) or 'd.%s[j]%ss.%s[j]' % (k, m.get(op, ''), k)
            return '{XMM s=%s,d=%s;for(int j=0;j<%d;j++)%s.%s[j]=%s;}' % (xv(ops[1]), D, cnt, D, k, e)
        if mn in ('cvtsi2sd', 'cvtsi2ss'):
            T = 'double' if mn.endswith('sd') else 'float'; k = 'g' if T == 'double' else 'f'
            return '%s.%s[0]=(%s)(int%d_t)%s;' % (D, k, T, 8 * ops[1].size, self.rdop(ops[1]))
        if mn in ('cvttsd2si', 'cvttss2si', 'cvtsd2si', 'cvtss2si'):
            k = 'g' if 'sd2' in mn else 'f'; B = 64 if ops[0].size == 8 else 32
            s = xs(ops[1], k)
            conv = 'aot_d2i((double)(%s),%d)' if mn.startswith('cvtt') else 'aot_d2i(nearbyint((double)(%s)),%d)'
            return self.wrop(ops[0], conv % (s, B), ops[0].size)
        if mn == 'cvtss2sd': return '{float s=%s;%s.g[0]=s;}' % (xs(ops[1], 'f'), D)
        if mn == 'cvtsd2ss': return '{double s=%s;%s.f[0]=(float)s;}' % (xs(ops[1], 'g'), D)
        if mn == 'cvtdq2ps': return '{XMM s=%s;for(int j=0;j<4;j++)%s.f[j]=(float)(int32_t)s.d[j];}' % (xv(ops[1]), D)
        if mn == 'cvttps2dq': return '{XMM s=%s;for(int j=0;j<4;j++)%s.d[j]=(uint32_t)aot_d2i(s.f[j],32);}' % (xv(ops[1]), D)
        if mn == 'cvtps2pd': return '{XMM s=%s;double a=s.f[0],b=s.f[1];%s.g[0]=a;%s.g[1]=b;}' % (xv(ops[1]), D, D)
        if mn in ('ucomiss', 'comiss', 'ucomisd', 'comisd'):
            k = 'f' if mn.endswith('ss') else 'g'; T = 'float' if k == 'f' else 'double'
            return '{%s a=%s.%s[0],b=%s;int un=a!=a||b!=b;zf=un||a==b;pf=un;cf=un||a<b;of=0;sf=0;}' % (T, D, k, xs(ops[1], k))
        LN = {'b': (1, 16), 'w': (2, 8), 'd': (4, 4), 'q': (8, 2)}
        if mn[:2] == 'ps' and mn[2:5] in ('lld', 'rld', 'rad', 'llq', 'rlq', 'llw', 'rlw', 'raw') and mn != 'pshufd':
            L = mn[-1]; k = {'d': 'd', 'q': 'q', 'w': 'w'}[L]; B = 8 * LN[L][0]; N = LN[L][1]
            o1 = ops[1]; cnt = '%d' % (o1.imm & 0xff) if o1.type == X86_OP_IMM else ('%s.q[0]' % self.xr(o1) if o1.type == X86_OP_REG else 'LD(uint64_t,%s)' % self.addr(o1))
            T = 'uint%d_t' % B; SG = 'int%d_t' % B
            if mn[2] == 'l': e = 'cnt>%d?0:(%s)(%s.%s[j]<<cnt)' % (B - 1, T, D, k)
            elif mn[3] == 'r' and mn[2] == 'r' and mn[3:5] == 'ra' or mn[2:4] == 'ra': e = '(%s)((%s)%s.%s[j]>>(cnt>%d?%d:cnt))' % (T, SG, D, k, B - 1, B - 1)
            else: e = 'cnt>%d?0:(%s)(%s.%s[j]>>cnt)' % (B - 1, T, D, k)
            return '{uint64_t cnt=%s;for(int j=0;j<%d;j++)%s.%s[j]=%s;}' % (cnt, N, D, k, e)
        if mn.startswith('punpck') and mn[6] in 'lh':
            sub = mn[7:]; L = {'bw': 'b', 'wd': 'w', 'dq': 'd', 'qdq': 'q'}[sub]; N = LN[L][1]; off = 0 if mn[6] == 'l' else N // 2
            return '{XMM s=%s,d=%s;for(int k=0;k<%d;k++){%s.%s[2*k]=d.%s[k+%d];%s.%s[2*k+1]=s.%s[k+%d];}}' % (xv(ops[1]), D, N // 2, D, L, L, off, D, L, L, off)
        if mn in ('packssdw', 'packsswb', 'packuswb'):
            src, dst, lo, hi = ('d', 'w', -32768, 32767) if mn == 'packssdw' else ('w', 'b', -128, 127) if mn == 'packsswb' else ('w', 'b', 0, 255)
            n = 4 if src == 'd' else 8; T = 'int16_t' if dst == 'w' else ('int8_t' if mn == 'packsswb' else 'uint8_t'); ST_ = 'int32_t' if src == 'd' else 'int16_t'
            sat = 'v<%d?%d:v>%d?%d:v' % (lo, lo, hi, hi)
            return '{XMM s=%s,d=%s;for(int k=0;k<%d;k++){int v=(%s)d.%s[k];%s.%s[k]=(%s)(%s);v=(%s)s.%s[k];%s.%s[k+%d]=(%s)(%s);}}' % (xv(ops[1]), D, n, ST_, src, D, dst, T, sat, ST_, src, D, dst, n, T, sat)
        if mn in ('pmullw', 'pmulhw', 'pmulhuw', 'pmuludq', 'pmaddwd', 'pminub', 'pmaxub', 'pminsw', 'pmaxsw', 'paddw', 'psubw', 'pcmpeqw', 'pcmpgtb', 'pcmpgtw', 'pcmpgtd', 'paddusb', 'psubusb', 'paddusw', 'psubusw', 'paddsw', 'psubsw', 'paddsb', 'psubsb', 'pavgb', 'pavgw'):
            g = 'XMM s=%s,d=%s;' % (xv(ops[1]), D); lp = 'for(int j=0;j<%d;j++)%s.%s[j]=%s;'
            if mn == 'pmuludq': return '{%s%s.q[0]=(uint64_t)d.d[0]*s.d[0];%s.q[1]=(uint64_t)d.d[2]*s.d[2];}' % (g, D, D)
            if mn == 'pmaddwd': return '{%sfor(int j=0;j<4;j++)%s.d[j]=(uint32_t)((int32_t)(int16_t)d.w[2*j]*(int16_t)s.w[2*j]+(int32_t)(int16_t)d.w[2*j+1]*(int16_t)s.w[2*j+1]);}' % (g, D)
            tab = {'pmullw': ('w', 8, '(uint16_t)((int16_t)d.w[j]*(int16_t)s.w[j])'), 'pmulhw': ('w', 8, '(uint16_t)(((int32_t)(int16_t)d.w[j]*(int16_t)s.w[j])>>16)'),
                   'pmulhuw': ('w', 8, '(uint16_t)(((uint32_t)d.w[j]*s.w[j])>>16)'), 'pminub': ('b', 16, 'd.b[j]<s.b[j]?d.b[j]:s.b[j]'), 'pmaxub': ('b', 16, 'd.b[j]>s.b[j]?d.b[j]:s.b[j]'),
                   'pminsw': ('w', 8, '(int16_t)d.w[j]<(int16_t)s.w[j]?d.w[j]:s.w[j]'), 'pmaxsw': ('w', 8, '(int16_t)d.w[j]>(int16_t)s.w[j]?d.w[j]:s.w[j]'),
                   'paddw': ('w', 8, '(uint16_t)(d.w[j]+s.w[j])'), 'psubw': ('w', 8, '(uint16_t)(d.w[j]-s.w[j])'), 'pcmpeqw': ('w', 8, '-(d.w[j]==s.w[j])'),
                   'pcmpgtb': ('b', 16, '-((int8_t)d.b[j]>(int8_t)s.b[j])'), 'pcmpgtw': ('w', 8, '-((int16_t)d.w[j]>(int16_t)s.w[j])'), 'pcmpgtd': ('d', 4, '-((int32_t)d.d[j]>(int32_t)s.d[j])'),
                   'paddusb': ('b', 16, '(int)d.b[j]+s.b[j]>255?255:d.b[j]+s.b[j]'), 'psubusb': ('b', 16, 'd.b[j]>s.b[j]?d.b[j]-s.b[j]:0'),
                   'paddusw': ('w', 8, '(int)d.w[j]+s.w[j]>65535?65535:d.w[j]+s.w[j]'), 'psubusw': ('w', 8, 'd.w[j]>s.w[j]?d.w[j]-s.w[j]:0'),
                   'paddsw': ('w', 8, '({int v=(int16_t)d.w[j]+(int16_t)s.w[j];(uint16_t)(v>32767?32767:v<-32768?-32768:v);})'), 'psubsw': ('w', 8, '({int v=(int16_t)d.w[j]-(int16_t)s.w[j];(uint16_t)(v>32767?32767:v<-32768?-32768:v);})'),
                   'paddsb': ('b', 16, '({int v=(int8_t)d.b[j]+(int8_t)s.b[j];(uint8_t)(v>127?127:v<-128?-128:v);})'), 'psubsb': ('b', 16, '({int v=(int8_t)d.b[j]-(int8_t)s.b[j];(uint8_t)(v>127?127:v<-128?-128:v);})'),
                   'pavgb': ('b', 16, '(d.b[j]+s.b[j]+1)>>1'), 'pavgw': ('w', 8, '(d.w[j]+s.w[j]+1)>>1')}[mn]
            return '{%s%s}' % (g, lp % (tab[1], D, tab[0], tab[2]))
        if mn == 'cvtpd2ps': return '{XMM s=%s;float a=(float)s.g[0],b=(float)s.g[1];%s.f[0]=a;%s.f[1]=b;%s.q[1]=0;}' % (xv(ops[1]), D, D, D)
        if mn == 'cvtdq2pd': return '{XMM s=%s;%s.g[0]=(int32_t)s.d[0];%s.g[1]=(int32_t)s.d[1];}' % (xv(ops[1]), D, D)
        if mn in ('cvtps2dq', 'cvttpd2dq', 'cvtpd2dq'):
            if mn == 'cvtps2dq': return '{XMM s=%s;for(int j=0;j<4;j++)%s.d[j]=(uint32_t)aot_d2i(nearbyintf(s.f[j]),32);}' % (xv(ops[1]), D)
            return '{XMM s=%s;int32_t a=(int32_t)aot_d2i(%ss.g[0]%s,32),b=(int32_t)aot_d2i(%ss.g[1]%s,32);%s.d[0]=a;%s.d[1]=b;%s.q[1]=0;}' % ((xv(ops[1]),) + (('', '') if mn == 'cvttpd2dq' else ('nearbyint(', ')')) * 2 + (D, D, D))
        if mn in ('pshuflw', 'pshufhw'):
            o = 0 if mn == 'pshuflw' else 4
            return '{XMM s=%s;unsigned m=%d;%s=s;for(int k=0;k<4;k++)%s.w[%d+k]=s.w[%d+((m>>(2*k))&3)];}' % (xv(ops[1]), ops[2].imm & 255, D, D, o, o)
        if mn in ('pcmpeqd', 'pcmpeqb', 'paddd', 'psubd', 'paddq', 'psubq', 'paddb', 'psubb', 'pmulld'):
            e = {'d': ('d', 4), 'q': ('q', 2), 'b': ('b', 16)}[mn[-1]]
            op = {'pcmpeq': '==', 'padd': '+', 'psub': '-', 'pmull': '*'}[mn[:-1]]
            ex = '-(d.%s[j]==s.%s[j])' % (e[0], e[0]) if op == '==' else 'd.%s[j]%ss.%s[j]' % (e[0], op, e[0])
            return '{XMM s=%s,d=%s;for(int j=0;j<%d;j++)%s.%s[j]=%s;}' % (xv(ops[1]), D, e[1], D, e[0], ex)
        if mn == 'pmovmskb': return self.wrop(ops[0], '({XMM s=%s;unsigned m=0;for(int j=0;j<16;j++)m|=(s.b[j]>>7)<<j;m;})' % self.xr(ops[1]), 4)
        if mn in ('pslldq', 'psrldq'): raise Unsup(mn)
        raise Unsup(mn)

    # x87 FPU ----------------------------------------------------------------
    def x87(self, i, mn, ops, n, need):
        A = self.addr
        def sti(op): return int(self.i.reg_name(op.reg)[3:-1])
        def memf(op):
            if mn.startswith('fi'): return '(aot_f80)(int%d_t)LD(int%d_t,%s)' % ((8 * op.size,) * 2 + (A(op),))
            return {4: '(aot_f80)LD(float,%s)', 8: '(aot_f80)LD(double,%s)'}[op.size] % A(op) if op.size in (4, 8) else 'aot_ld80(%s)' % A(op)
        def stv(op): return 'FST(%d)' % sti(op)
        base = mn[:-1] if mn.endswith('p') and mn not in ('fscale',) and not mn.startswith('fcom') and mn[1:-1] in ('add', 'sub', 'subr', 'mul', 'div', 'divr') else mn
        pop = base != mn
        arith = {'add': '+', 'mul': '*'}
        for nm in ('fadd', 'fsub', 'fsubr', 'fmul', 'fdiv', 'fdivr', 'fiadd', 'fisub', 'fisubr', 'fimul', 'fidiv', 'fidivr'):
            if base == nm:
                k = nm.replace('fi', 'f', 1) if nm.startswith('fi') else nm
                k = k[1:]
                if n == 1 and ops[0].type == X86_OP_MEM: dst, src = 'FST(0)', memf(ops[0])
                elif n == 1: dst, src = ('FST(%d)' % sti(ops[0]), 'FST(0)') if pop else ('FST(0)', stv(ops[0]))
                else: dst, src = stv(ops[0]), stv(ops[1])
                return '{aot_f80 _s=%s;%s=%s;}%s' % (src, dst, self._arith(k, dst, '_s'), 'FPOP();' if pop else '')
        if mn in ('fld',):
            return 'FPUSH(%s);' % (memf(ops[0]) if ops[0].type == X86_OP_MEM else stv(ops[0]))
        if mn == 'fild': return 'FPUSH(%s);' % memf(ops[0])
        if mn in ('fst', 'fstp', 'fstpnce'):
            o = ops[0]; pp = 'FPOP();' if mn != 'fst' else ''
            if o.type == X86_OP_MEM:
                if o.size == 4: return 'ST(float,%s,(float)FST(0));%s' % (A(o), pp)
                if o.size == 8: return 'ST(double,%s,(double)FST(0));%s' % (A(o), pp)
                return 'aot_st80(%s,FST(0));%s' % (A(o), pp)
            return '%s=FST(0);%s' % (stv(o), pp)
        if mn in ('fist', 'fistp', 'fisttp'):
            o = ops[0]; b = 8 * o.size
            return 'ST(int%d_t,%s,(int%d_t)aot_fist(FST(0),%d,c->fcw,%d));%s' % (b, A(o), b, b, 1 if mn == 'fisttp' else 0, 'FPOP();' if mn != 'fist' else '')
        if mn in ('fxch',): return '{aot_f80 _t=FST(0);FST(0)=%s;%s=_t;}' % (stv(ops[-1]), stv(ops[-1]))
        un = {'fchs': 'FST(0)=-FST(0);', 'fabs': 'FST(0)=F80(fabs)(FST(0));', 'fsqrt': 'FST(0)=F80(sqrt)(FST(0));', 'fsin': 'FST(0)=F80(sin)(FST(0));', 'fcos': 'FST(0)=F80(cos)(FST(0));',
              'frndint': 'FST(0)=(aot_f80)aot_fist_r(FST(0),c->fcw);', 'f2xm1': 'FST(0)=F80(exp2)(FST(0))-1;',
              'fld1': 'FPUSH(1);', 'fldz': 'FPUSH(0);', 'fldpi': 'FPUSH(3.14159265358979323846264338327950288L);', 'fldl2e': 'FPUSH(1.44269504088896340735992468100189214L);',
              'fldl2t': 'FPUSH(3.32192809488736234787031942948939018L);', 'fldlg2': 'FPUSH(0.301029995663981195213738894724493027L);', 'fldln2': 'FPUSH(0.693147180559945309417232121458176568L);',
              'fptan': 'FST(0)=F80(tan)(FST(0));FPUSH(1);', 'fpatan': 'FST(1)=F80(atan2)(FST(1),FST(0));FPOP();', 'fyl2x': 'FST(1)=FST(1)*F80(log2)(FST(0));FPOP();',
              'fyl2xp1': 'FST(1)=FST(1)*F80(log2)(FST(0)+1);FPOP();', 'fscale': 'FST(0)=F80(ldexp)(FST(0),(int)F80(trunc)(FST(1)));',
              'fsincos': '{aot_f80 _v=FST(0);FST(0)=F80(sin)(_v);FPUSH(F80(cos)(_v));}', 'fincstp': 'c->ftop=(c->ftop+1)&7;', 'fdecstp': 'c->ftop=(c->ftop+7)&7;',
              'fprem': '{aot_f80 _a=FST(0),_b=FST(1),_r=F80(fmod)(_a,_b);long long _q=(long long)F80(trunc)(_a/_b);FST(0)=_r;c->fsw=(c->fsw&~0x4700)|((_q&4)?0x100:0)|((_q&2)?0x4000:0)|((_q&1)?0x200:0);}',
              'fprem1': '{aot_f80 _a=FST(0),_b=FST(1),_r=F80(remainder)(_a,_b);long long _q=(long long)F80(nearbyint)(_a/_b);FST(0)=_r;c->fsw=(c->fsw&~0x4700)|((_q&4)?0x100:0)|((_q&2)?0x4000:0)|((_q&1)?0x200:0);}',
              'fninit': 'c->fcw=0x37F;c->fsw=0;c->ftop=0;', 'finit': 'c->fcw=0x37F;c->fsw=0;c->ftop=0;', 'fnclex': 'c->fsw&=0x7f00;', 'fclex': 'c->fsw&=0x7f00;', 'fwait': '', 'wait': '',
              'ffree': '', 'ffreep': 'FPOP();', 'fnop': '',
              'fxam': '{aot_f80 _v=FST(0);int _c=_v!=_v?0x100:(_v==INFINITY||_v==-INFINITY)?0x500:_v==0?0x4000:0x400;c->fsw=(c->fsw&~0x4700)|_c|(signbit(_v)?0x200:0);}',
              'ftst': '{aot_f80 _v=FST(0);c->fsw=(c->fsw&~0x4500)|(_v!=_v?0x4500:_v<0?0x100:_v==0?0x4000:0);}'}
        if mn in un: return un[mn]
        if mn in ('fnstenv', 'fnsave'):
            a = A(ops[0])
            s = '{uint64_t p=%s;ST(uint16_t,p,c->fcw);ST(uint16_t,p+4,(c->fsw&~0x3800)|(c->ftop<<11));ST(uint16_t,p+8,0xFFFF);' % a
            if mn == 'fnsave':
                s += 'for(int j=0;j<8;j++)aot_st80(p+28+10*j,FST(j));c->fcw=0x37F;c->fsw=0;c->ftop=0;'
            return s + '}'
        if mn in ('fldenv', 'frstor'):
            a = A(ops[0])
            s = '{uint64_t p=%s;c->fcw=LD(uint16_t,p);c->fsw=LD(uint16_t,p+4);c->ftop=(c->fsw>>11)&7;' % a
            if mn == 'frstor': s += 'unsigned ft=c->ftop;for(int j=0;j<8;j++)c->fst[(ft+j)&7]=aot_ld80(p+28+10*j);'
            return s + '}'
        if mn == 'fbstp':
            return '{uint64_t p=%s;int64_t v=aot_fist(FST(0),64,c->fcw,0);uint64_t u=v<0?-(uint64_t)v:(uint64_t)v;for(int j=0;j<9;j++){ST(uint8_t,p+j,(u%%10)|(((u/10)%%10)<<4));u/=100;}ST(uint8_t,p+9,v<0?0x80:0);FPOP();}' % A(ops[0])
        if mn in ('fnstsw', 'fstsw'):
            v = '(uint16_t)((c->fsw&~0x3800)|(c->ftop<<11))'
            return ('r0=(r0&~0xFFFFULL)|%s;' % v) if ops[0].type == X86_OP_REG else 'ST(uint16_t,%s,%s);' % (A(ops[0]), v)
        if mn in ('fnstcw', 'fstcw'): return 'ST(uint16_t,%s,c->fcw);' % A(ops[0])
        if mn == 'fldcw': return 'c->fcw=LD(uint16_t,%s);' % A(ops[0])
        if mn in ('fcom', 'fcomp', 'fucom', 'fucomp', 'fcompp', 'fucompp', 'ficom', 'ficomp'):
            src = memf(ops[0]) if n and ops[0].type == X86_OP_MEM else ('FST(%d)' % sti(ops[0]) if n else 'FST(1)')
            pp = 'FPOP();FPOP();' if mn.endswith('pp') else 'FPOP();' if mn.endswith('p') else ''
            return '{aot_f80 _a=FST(0),_b=%s;c->fsw=(c->fsw&~0x4500)|((_a!=_a||_b!=_b)?0x4500:_a<_b?0x100:_a==_b?0x4000:0);}%s' % (src, pp)
        if mn in ('fcomi', 'fucomi', 'fcompi', 'fucompi', 'fcomip', 'fucomip'):
            pp = 'FPOP();' if mn.endswith('p') or mn.endswith('pi') else ''
            return '{aot_f80 _a=FST(0),_b=%s;int _u=(_a!=_a||_b!=_b);cf=_u||_a<_b;zf=_u||_a==_b;pf=_u;of=0;sf=0;}%s' % (stv(ops[-1]), pp)
        if mn.startswith('fcmov'):
            cc = {'b': 'cf', 'e': 'zf', 'be': '(cf|zf)', 'u': 'pf', 'nb': '!cf', 'ne': '!zf', 'nbe': '!(cf|zf)', 'nu': '!pf'}[mn[5:]]
            return 'if(%s)FST(0)=%s;' % (cc, stv(ops[1]))
        raise Unsup(mn)
    def _arith(self, k, dst, src):
        a, b = (src, dst) if k in ('subr', 'divr') else (dst, src)
        return '%s%s%s' % (a, {'add': '+', 'mul': '*', 'sub': '-', 'div': '/', 'subr': '-', 'divr': '/'}[k], b)

    # AVX / AVX2 / FMA (lowered onto the SSE generator; ymm upper halves live in c->xh[]) -------------------
    def avx(self, i, mn, ops, n, need):
        m = mn[1:]; A = self.addr
        def rn(op): return self.i.reg_name(op.reg)
        def isv(op): return op.type == X86_OP_REG and (rn(op).startswith('xmm') or rn(op).startswith('ymm'))
        def idx(op): return int(rn(op)[3:])
        y = any(o.size == 32 for o in ops if o.type in (X86_OP_REG, X86_OP_MEM) and (o.type == X86_OP_MEM or isv(o)))
        def lo(op, h):   # value of 128-bit half h of a vector operand
            if isv(op): return 'c->%s[%d]' % ('xh' if h else 'x', idx(op))
            return 'LDX(%s+%d)' % (A(op), 16 * h)
        def setd(h, v): return 'c->%s[%d]=%s;' % ('xh' if h else 'x', idx(ops[0]), v)
        zhi = lambda: 'c->xh[%d].q[0]=c->xh[%d].q[1]=0;' % (idx(ops[0]), idx(ops[0])) if isv(ops[0]) and not y else ''
        if m == 'zeroupper': return 'for(int _j=0;_j<16;_j++)c->xh[_j].q[0]=c->xh[_j].q[1]=0;'
        if m == 'zeroall': return 'for(int _j=0;_j<16;_j++){c->xh[_j].q[0]=c->xh[_j].q[1]=0;c->x[_j].q[0]=c->x[_j].q[1]=0;}'
        if m in ('movaps', 'movups', 'movdqa', 'movdqu', 'movapd', 'movupd', 'lddqu', 'movntdq', 'movntps', 'movntpd') and y:
            if ops[0].type == X86_OP_MEM: return 'STX(%s,%s);STX(%s+16,%s);' % (A(ops[0]), lo(ops[1], 0), A(ops[0]), lo(ops[1], 1))
            return '{XMM a=%s,b=%s;%s%s}' % (lo(ops[1], 0), lo(ops[1], 1), setd(0, 'a'), setd(1, 'b'))
        if m.startswith('broadcast') or m.startswith('pbroadcast'):
            sz = {'ss': 4, 'sd': 8, 'f128': 16, 'i128': 16, 'b': 1, 'w': 2, 'd': 4, 'q': 8}[m.split('broadcast')[1]]
            if sz == 16: return '{XMM a=LDX(%s);%s%s}' % (A(ops[1]), setd(0, 'a'), setd(1, 'a'))
            T = 'uint%d_t' % (8 * sz); src = 'LD(%s,%s)' % (T, A(ops[1])) if ops[1].type == X86_OP_MEM else '(%s)c->x[%d].q[0]' % (T, idx(ops[1]))
            return '{%s v=%s;XMM a;for(int _j=0;_j<%d;_j++)a.%s[_j]=v;%s%s}' % (T, src, 16 // sz, {1: 'b', 2: 'w', 4: 'd', 8: 'q'}[sz], setd(0, 'a'), setd(1, 'a') if y else zhi())
        if m in ('extractf128', 'extracti128'):
            h = ops[2].imm & 1; v = 'c->%s[%d]' % ('xh' if h else 'x', idx(ops[1]))
            return 'STX(%s,%s);' % (A(ops[0]), v) if ops[0].type == X86_OP_MEM else '{XMM a=%s;c->x[%d]=a;c->xh[%d].q[0]=c->xh[%d].q[1]=0;}' % (v, idx(ops[0]), idx(ops[0]), idx(ops[0]))
        if m in ('insertf128', 'inserti128'):
            h = ops[3].imm & 1; s2 = lo(ops[2], 0)
            return '{XMM a=c->x[%d],b=c->xh[%d],ins=%s;%s%s}' % (idx(ops[1]), idx(ops[1]), s2, setd(0, 'ins' if h == 0 else 'a'), setd(1, 'b' if h == 0 else 'ins'))
        if m in ('perm2f128', 'perm2i128'):
            im = ops[3].imm
            def sel(h):
                f = (im >> (4 * h)) & 15
                return '0' if f & 8 else ('(%s)' % ('b' if f & 2 else 'a')) + '%s' % ('h' if f & 1 else 'l')
            def lane(f): return 'zero' if f & 8 else '%s%s' % ('S2' if f & 2 else 'S1', 'h' if f & 1 else 'l')
            return '{XMM zero={{0}},S1l=%s,S1h=%s,S2l=%s,S2h=%s;XMM r0_=%s,r1_=%s;%s%s}' % (lo(ops[1], 0), lo(ops[1], 1), lo(ops[2], 0), lo(ops[2], 1), lane(im & 15), lane((im >> 4) & 15), setd(0, 'r0_'), setd(1, 'r1_'))
        if m in ('movmskps', 'movmskpd', 'pmovmskb'):
            per = {'movmskps': 4, 'movmskpd': 2, 'pmovmskb': 16}[m]; sh = {'movmskps': 'd', 'movmskpd': 'q', 'pmovmskb': 'b'}[m]; bits = {'d': 31, 'q': 63, 'b': 7}[sh]
            return self.wrop(ops[0], '({unsigned _m=0;for(int _j=0;_j<%d;_j++){_m|=(unsigned)((c->%s[%d].%s[_j]>>%d)&1)<<_j;%s}_m;})' % (per, 'x', idx(ops[1]), sh, bits,
                    ('_m|=(unsigned)((c->xh[%d].%s[_j]>>%d)&1)<<(_j+%d);' % (idx(ops[1]), sh, bits, per)) if y else ''), 4)
        if m in ('blendvps', 'blendvpd', 'pblendvb'):
            sh = {'blendvps': ('d', 4, 31), 'blendvpd': ('q', 2, 63), 'pblendvb': ('b', 16, 7)}[m]
            body = ''
            for h in range(2 if y else 1):
                body += '{XMM a=%s,b=%s,k=%s;for(int _j=0;_j<%d;_j++)if((k.%s[_j]>>%d)&1)a.%s[_j]=b.%s[_j];%s}' % (lo(ops[1], h), lo(ops[2], h), lo(ops[3], h), sh[1], sh[0], sh[2], sh[0], sh[0], setd(h, 'a'))
            return body + zhi()
        if m in ('permilps',) and n == 3:
            body = ''
            for h in range(2 if y else 1):
                if ops[2].type == X86_OP_IMM: ix = '((%d>>(2*_j))&3)' % (ops[2].imm & 255)
                else: ix = '(k.d[_j]&3)'
                kk = 'XMM k=%s;' % lo(ops[2], h) if ops[2].type != X86_OP_IMM else ''
                body += '{XMM s=%s,a;%sfor(int _j=0;_j<4;_j++)a.d[_j]=s.d[%s];%s}' % (lo(ops[1], h), kk, ix, setd(h, 'a'))
            return body + zhi()
        if m in ('permq', 'permpd') and n == 3:
            im = ops[2].imm & 255
            return '{uint64_t q[4]={%s.q[0],%s.q[1],%s.q[0],%s.q[1]};XMM a,b;a.q[0]=q[%d];a.q[1]=q[%d];b.q[0]=q[%d];b.q[1]=q[%d];%s%s}' % (
                lo(ops[1], 0), lo(ops[1], 0), lo(ops[1], 1), lo(ops[1], 1), im & 3, (im >> 2) & 3, (im >> 4) & 3, (im >> 6) & 3, setd(0, 'a'), setd(1, 'b')) if False else \
                '{uint64_t q[4]={%s.q[0],%s.q[1],%s.q[0],%s.q[1]};XMM a,b;a.q[0]=q[%d];a.q[1]=q[%d];b.q[0]=q[%d];b.q[1]=q[%d];%s%s}' % (
                lo(ops[1], 0), lo(ops[1], 0), lo(ops[1], 1), lo(ops[1], 1), im & 3, (im >> 2) & 3, (im >> 4) & 3, (im >> 6) & 3, setd(0, 'a'), setd(1, 'b'))
        if m in ('permd', 'permps'):
            return '{uint32_t s[8];XMM k0=%s,k1=%s,v0=%s,v1=%s,a,b;memcpy(s,&v0,16);memcpy(s+4,&v1,16);for(int _j=0;_j<4;_j++){a.d[_j]=s[k0.d[_j]&7];b.d[_j]=s[k1.d[_j]&7];}%s%s}' % (
                lo(ops[1], 0), lo(ops[1], 1), lo(ops[2], 0), lo(ops[2], 1), setd(0, 'a'), setd(1, 'b'))
        import re
        fm = re.match(r'f(n?)m(add|sub)(132|213|231)(ps|pd|ss|sd)$', m)
        if fm:
            neg, sub, order, t = fm.groups(); sc = t[0] == 's'; dbl = t[1] == 'd'; el = 'g' if dbl else 'f'; cnt = 1 if sc else (2 if dbl else 4); fn = 'fma' if dbl else 'fmaf'
            body = ''
            for h in range(1 if sc else (2 if y else 1)):
                d = 'D'; s1 = 'S1'; s2 = 'S2'
                a, b, c_ = {'132': (d, s2, s1), '213': (s1, d, s2), '231': (s1, s2, d)}[order]
                body += '{XMM D=%s,S1=%s,S2=%s;for(int _j=0;_j<%d;_j++)D.%s[_j]=%s(%s%s.%s[_j],%s.%s[_j],%s%s.%s[_j]);%s}' % (
                    lo(ops[0], h), lo(ops[1], h), lo(ops[2], h) if not sc else ('(XMM){.%s={%s}}' % (el, ('LD(double,%s)' % A(ops[2])) if (dbl and ops[2].type == X86_OP_MEM) else (('LD(float,%s)' % A(ops[2])) if ops[2].type == X86_OP_MEM else 'c->x[%d].%s[0]' % (idx(ops[2]), el)))),
                    cnt, el, fn, '-' if neg else '', a, el, b, el, '-' if sub == 'sub' else '', c_, el, setd(h, 'D'))
            return body + (zhi() if not sc else zhi())
        # ---- generic lane-wise lowering onto SSE ----
        def fr(k): return type('FR', (), {'type': X86_OP_REG, 'reg': getattr(sys.modules['capstone.x86'], 'X86_REG_XMM%d' % k), 'size': 16})()
        direct = m in ('comiss', 'ucomiss', 'comisd', 'ucomisd', 'ptest') or not isv(ops[0]) or (n == 2 and not y)
        if direct:
            return self.sse(i, m, ops, n, isv(ops[0]), n > 1 and isv(ops[1]), need) + zhi()
        imm = [ops[-1]] if ops[-1].type == X86_OP_IMM else []
        regs = ops[:-1] if imm else ops
        shift_imm = bool(imm) and m[1:3] in ('sl', 'sr') or m in ('pslldq', 'psrldq') or (bool(imm) and m.startswith('psra'))
        body = ''
        for h in range(2 if y else 1):
            pre = ''
            if len(regs) == 2 and not shift_imm:         # d = op(s [, imm])
                srcop = regs[1]; first = None
            else:                                        # d = op(s1 [,s2] [,imm]) with s1 copied into the destination first
                first = regs[1]; srcop = regs[2] if len(regs) > 2 else None
            if first is not None: pre += 'c->x[17]=%s;' % lo(first, h)
            call_ops = [fr(17)]
            if srcop is not None:
                if isv(srcop) or (srcop.type == X86_OP_MEM and srcop.size in (16, 32)):
                    pre += 'c->x[16]=%s;' % lo(srcop, h); call_ops.append(fr(16))
                else: call_ops.append(srcop)
            call_ops += imm
            body += '{' + pre + self.sse(i, m, call_ops, len(call_ops), True, len(call_ops) > 1 and call_ops[1].type == X86_OP_REG and isv(call_ops[1]) if False else (len(call_ops) > 1 and call_ops[1].type == X86_OP_REG and rn(call_ops[1]).startswith('xmm')), need) + setd(h, 'c->x[17]') + '}'
        return body + zhi()

class FOp:
    type = X86_OP_REG
    def __init__(self, reg, size): self.reg = reg; self.size = size
REGOP_RAX = {1: FOp(X86_REG_AL, 1), 2: FOp(X86_REG_AX, 2), 4: FOp(X86_REG_EAX, 4), 8: FOp(X86_REG_RAX, 8)}

def emit(rec, outdir, per_region=300, regions_per_file=8):
    g = Gen(rec); blocks = rec.blocks(); os.makedirs(outdir, exist_ok=True)
    regions = []; cur = []; cnt = 0
    for b in blocks:
        cur.append(b); cnt += len(b)
        if cnt >= per_region: regions.append(cur); cur = []; cnt = 0
    if cur: regions.append(cur)
    owner = {b[0].address: k for k, r in enumerate(regions) for b in r}
    unsup = rec.unsup; nok = 0
    files = []; buf = ['#include "aot_runtime.h"\n']
    for k, reg in enumerate(regions):
#!/usr/bin/env python3
"""Static recompiler: x86_64 Windows PE -> portable C (guest addr == host addr, mapped at ImageBase).
usage: aot_transpiler.py game.exe out_dir"""
import sys, os, struct, bisect
from collections import Counter
import pefile
from capstone import *
from capstone.x86 import *

F5 = ('cf', 'zf', 'sf', 'of', 'pf')
N64 = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi'] + ['r%d' % i for i in range(8, 16)]
REG = {}
for i, n in enumerate(N64): REG[n] = (i, 8, 0)
for i, n in enumerate(['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'] + ['r%dd' % i for i in range(8, 16)]): REG[n] = (i, 4, 0)
for i, n in enumerate(['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di'] + ['r%dw' % i for i in range(8, 16)]): REG[n] = (i, 2, 0)
for i, n in enumerate(['al', 'cl', 'dl', 'bl', 'spl', 'bpl', 'sil', 'dil'] + ['r%db' % i for i in range(8, 16)]): REG[n] = (i, 1, 0)
for i, n in enumerate(['ah', 'ch', 'dh', 'bh']): REG[n] = (i, 1, 1)

COND = {'o': 'of', 'no': '!of', 'b': 'cf', 'c': 'cf', 'nae': 'cf', 'ae': '!cf', 'nb': '!cf', 'nc': '!cf', 'e': 'zf', 'z': 'zf',
        'ne': '!zf', 'nz': '!zf', 'be': '(cf|zf)', 'na': '(cf|zf)', 'a': '!(cf|zf)', 'nbe': '!(cf|zf)', 's': 'sf', 'ns': '!sf',
        'p': 'pf', 'pe': 'pf', 'np': '!pf', 'po': '!pf', 'l': '(sf!=of)', 'nge': '(sf!=of)', 'ge': '(sf==of)', 'nl': '(sf==of)',
        'rcxz': '(r1==0)', 'ecxz': '((uint32_t)r1==0)', 'le': '(zf|(sf!=of))', 'ng': '(zf|(sf!=of))', 'g': '!(zf|(sf!=of))', 'nle': '!(zf|(sf!=of))'}

def _fm(kinds, f): return sum(getattr(sys.modules['capstone.x86'], 'X86_EFLAGS_%s_%s' % (k, f.upper())) for k in kinds)
WMASK = {f: _fm(('MODIFY', 'SET', 'RESET', 'UNDEFINED'), f) for f in F5}  # any of these bits is non-overlapping per flag
TMASK = {f: _fm(('TEST',), f) for f in F5}
MASK64 = (1 << 64) - 1

def image_fingerprint(path):
    value = 14695981039346656037
    with open(path, 'rb') as image:
        while True:
            chunk = image.read(1024 * 1024)
            if not chunk: break
            for byte in chunk:
                value = ((value ^ byte) * 1099511628211) & MASK64
    return value

class Unsup(Exception): pass

def is_term(mn): return mn in ('jmp', 'ljmp', 'ret', 'retf', 'iret', 'iretd', 'iretq', 'ud2', 'int3', 'hlt') or (mn[0] == 'j' and mn[1:] in COND)

class Rec:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        pe = self.pe
        mg, mc = pe.OPTIONAL_HEADER.Magic, pe.FILE_HEADER.Machine
        if not ((mg == 0x20b and mc == 0x8664) or (mg == 0x10b and mc == 0x14c)):
            sys.exit('ERROR: unsupported PE (machine 0x%x, magic 0x%x). Supported: x64 PE32+ and x86 PE32.' % (mc, mg))
        self.m32 = mg == 0x10b; self.psz = 4 if self.m32 else 8
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.image_hash = image_fingerprint(path)
        self.img = bytes(pe.get_memory_mapped_image())
        self.md = Cs(CS_ARCH_X86, CS_MODE_32 if self.m32 else CS_MODE_64); self.md.detail = True
        self.text = [(s.VirtualAddress + self.base, s.VirtualAddress + self.base + max(s.Misc_VirtualSize, s.SizeOfRawData))
                     for s in pe.sections if s.Characteristics & 0x20000000]
        self.insns = {}; self.leaders = set(); self.unsup = Counter(); self.funcs = []
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint

    def in_text(self, a): return any(lo <= a < hi for lo, hi in self.text)
    def rd(self, a, n):
        o = a - self.base
        return self.img[o:o + n] if 0 <= o < len(self.img) else b''
    def decode(self, a):
        b = self.rd(a, 15)
        return next(self.md.disasm(b, a, 1), None) if b else None

    # ---- discovery -------------------------------------------------------
    def seeds(self):
        s = {self.entry}
        pe = self.pe
        try:
            if self.m32: raise KeyError  # no .pdata in PE32
            d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]  # .pdata RUNTIME_FUNCTION
            for off in range(0, d.Size - 11, 12):
                b, e, _ = struct.unpack_from('<III', self.img, d.VirtualAddress + off)
                if b == 0: break
                self.funcs.append((b + self.base, e + self.base))
                s.add(b + self.base)
        except Exception: pass
        self.funcs.sort(); self.fstarts = [f[0] for f in self.funcs]
        for sec in pe.sections:  # absolute code pointers in data (vtables, callbacks)
            if sec.Characteristics & 0x20000000: continue
            o = sec.VirtualAddress; n = min(sec.Misc_VirtualSize, sec.SizeOfRawData) & ~(self.psz - 1)
            lo_, hi_ = self.base, self.base + len(self.img)
            for v in memoryview(self.img[o:o + n]).cast('I' if self.m32 else 'Q'):
                if lo_ <= v < hi_ and self.in_text(v): s.add(v)
        try:
            tls = pe.DIRECTORY_ENTRY_TLS.struct
            cb = tls.AddressOfCallBacks
            while cb:
                v = struct.unpack_from('<I' if self.m32 else '<Q', self.img, cb - self.base)[0]
                if not v: break
                s.add(v); cb += self.psz
        except Exception: pass
        return {a for a in s if self.in_text(a)}

    def func_range(self, a):
        k = bisect.bisect_right(self.fstarts, a) - 1
        if k >= 0 and self.funcs[k][0] <= a < self.funcs[k][1]: return self.funcs[k]
        return (a - 0x4000, a + 0x4000)

    def table_targets(self, jm, recent):
        """Heuristic jump-table recovery for `jmp reg` / `jmp [tbl+idx*8]`."""
        lo, hi = self.func_range(jm.address)
        lo = max(lo, self.text[0][0]); out = set()
        cands = []  # (table address, kind) kind 'rva' | 'abs' | 'rel:<base>'
        ops = jm.operands
        if ops[0].type == X86_OP_MEM and ops[0].mem.scale == self.psz and ops[0].mem.base == 0 and ops[0].mem.index:
            cands.append((ops[0].mem.disp & MASK64, 'abs'))
        leas = {}
        for i in recent:
            if i.mnemonic == 'lea' and i.operands[1].mem.base == X86_REG_RIP:
                leas[i.operands[0].reg] = i.address + i.size + i.operands[1].mem.disp
        for i in reversed(recent):
            for o in i.operands:
                if o.type == X86_OP_MEM and o.mem.scale == 4 and o.mem.index and o.mem.base != X86_REG_RIP:
                    if o.mem.base in leas: cands.append((leas[o.mem.base], 'rel:%d' % leas[o.mem.base]))
                    if o.mem.disp > 0 and not self.m32: cands.append((self.base + o.mem.disp, 'rva'))
        for tab, kind in cands:
            found = []
            for k in range(0, 2048):
                if kind == 'abs': v = struct.unpack('<Q', self.rd(tab + self.psz * k, self.psz).ljust(8, b'\0'))[0]
                else:
                    raw = self.rd(tab + 4 * k, 4)
                    if len(raw) < 4: break
                    v = struct.unpack('<I', raw)[0] + self.base if kind == 'rva' else (tab + struct.unpack('<i', raw)[0]) & MASK64
                if lo <= v < hi + 1 and self.in_text(v): found.append(v)
                else: break
            if found: out.update(found); break
        return out

    def discover(self):
        work = list(self.seeds()); self.leaders |= set(work)
        while work:
            a = work.pop(); recent = []
            while a not in self.insns:
                i = self.decode(a)
                if i is None or not self.in_text(a): break
                self.insns[a] = i; recent = (recent + [i])[-120:]
                mn = i.mnemonic.split()[-1]; nxt = a + i.size
                direct = i.operands and i.operands[0].type == X86_OP_IMM
                if mn in ('lea', 'mov', 'movabs', 'push') and len(i.operands) > 0:  # code pointers materialised by code
                    o = i.operands[-1]
                    t = (i.address + i.size + o.mem.disp if mn == 'lea' and o.mem.base == X86_REG_RIP else o.imm if o.type == X86_OP_IMM else 0)
                    if t and t not in self.leaders and self.in_text(t) and t != nxt: self.leaders.add(t); work.append(t)
                if mn in ('jmp', 'ljmp'):
                    target = i.operands[-1] if mn == 'ljmp' else i.operands[0]
                    if target.type == X86_OP_IMM: self.leaders.add(target.imm); work.append(target.imm)
                    elif mn == 'jmp':
                        for t in self.table_targets(i, recent): self.leaders.add(t); work.append(t)
                    break
                if (mn[0] == 'j' and mn[1:] in COND) or mn.startswith('loop'):
                    t = i.operands[0].imm; self.leaders.update((t, nxt)); work.append(t)
                elif mn in ('call', 'lcall'):
                    self.leaders.add(nxt)
                    target = i.operands[-1] if mn == 'lcall' else i.operands[0]
                    if target.type == X86_OP_IMM: self.leaders.add(target.imm); work.append(target.imm)
                elif is_term(mn): break
                a = nxt
            else:
                # Decoding ran into an already-decoded address: two instruction chains converge here (overlapping decodes from a
                # mid-instruction seed). That address must be a block entry, or the real chain falls through into a block that
                # swallowed it and has no case label.
                self.leaders.add(a)
        return self

    def blocks(self):
        bl = []; cur = None; prev = None
        for a in sorted(self.insns):
            i = self.insns[a]
            if cur is None or a in self.leaders or prev is None or prev.address + prev.size != a or is_term(prev.mnemonic.split()[-1]) \
               or prev.mnemonic.split()[-1] in ('call', 'lcall') or prev.mnemonic.split()[-1].startswith('loop') or (prev.mnemonic.split()[-1][0] == 'j'):
                cur = []; bl.append(cur)
            cur.append(i); prev = i
        return bl

# ---------------------------------------------------------------------------
class Gen:
    def __init__(self, rec):
        self.rec = rec; self.i = None; self.m32 = bool(getattr(rec, 'm32', False))

    # operands ---------------------------------------------------------------
    def reg(self, r):
        n = self.i.reg_name(r)
        if n not in REG: raise Unsup('reg ' + n)
        return REG[n]
    _mmx_ctx = None   # set while an MMX op is emulated through its SSE2 lane implementation (8-byte value in the low half)
    def xr(self, op):
        n = self.i.reg_name(op.reg)
        if self._mmx_ctx is not None and n.startswith('mm') and n[2:].isdigit(): return 'mx_d' if op is self._mmx_ctx else 'mx_s'
        if not n.startswith('xmm'): raise Unsup('reg ' + n)
        return 'c->x[%s]' % n[3:]
    def mmr(self, op):
        n = self.i.reg_name(op.reg)
        if not n.startswith('mm') or not n[2:].isdigit(): raise Unsup('reg ' + n)
        return 'c->mm[%s]' % n[2:]
    def sreg(self, op):
        n = self.i.reg_name(op.reg)
        if n not in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'): raise Unsup('reg ' + n)
        return 'c->sregs[%d]' % ('es cs ss ds fs gs'.split().index(n))
    def addr(self, op):
        m = op.mem; i = self.i
        if self.m32:
            p = []
            if m.base: p.append('r%d' % self.reg(m.base)[0])
            if m.index: p.append('r%d*%d' % (self.reg(m.index)[0], m.scale))
            if m.disp or not p: p.append('0x%xULL' % (m.disp & 0xFFFFFFFF))
            e = '(uint64_t)(uint32_t)(%s)' % '+'.join(p)
        elif m.base == X86_REG_RIP:
            e = '0x%xULL' % ((i.address + i.size + m.disp) & MASK64)
        else:
            p = []
            if m.base: p.append('r%d' % self.reg(m.base)[0])
            if m.index: p.append('r%d*%d' % (self.reg(m.index)[0], m.scale))
            if m.disp or not p: p.append('0x%xULL' % (m.disp & MASK64))
            e = '(uint64_t)(%s)' % '+'.join(p)
        if m.segment in (X86_REG_GS, X86_REG_FS): e = '(%s+c->gs)' % e
        return e
    def rdop(self, op, S=None):
        S = S or op.size; T = 'uint%d_t' % (8 * S)
        if op.type == X86_OP_REG:
            n, s, h = self.reg(op.reg)
            return '(uint8_t)(r%d>>8)' % n if h else '(%s)r%d' % (T if s == S else 'uint%d_t' % (8 * s), n)
        if op.type == X86_OP_IMM: return '((%s)0x%xULL)' % (T, op.imm & ((1 << (8 * S)) - 1))
        return 'LD(%s,%s)' % (T, self.addr(op))
    def wrop(self, op, v, S=None):
        S = S or op.size
        if op.type == X86_OP_MEM: return 'ST(uint%d_t,%s,%s);' % (8 * S, self.addr(op), v)
        n, s, h = self.reg(op.reg)
        if h: return 'r%d=(r%d&~0xFF00ULL)|((uint64_t)(uint8_t)(%s)<<8);' % (n, n, v)
        if s == 8: return 'r%d=(uint64_t)(%s);' % (n, v)
        if s == 4: return 'r%d=(uint32_t)(%s);' % (n, v)
        return 'r%d=(r%d&~0x%xULL)|(uint%d_t)(%s);' % (n, n, (1 << 8 * s) - 1, 8 * s, v)

    def fl(self, need, **kw):  # emit flag assignments that are live
        return ''.join('%s=%s;' % (f, e) for f, e in kw.items() if f in need)

    # ALU -----------------------------------------------------------------
    def alu(self, mn, S, a, b, dst, need, nowrite=False):
        T = 'uint%d_t' % (8 * S); sb = 8 * S - 1; ST_ = 'int%d_t' % (8 * S)
        com = dict(zf='r==0', sf='(r>>%d)&1' % sb, pf='PAR(r)')
        pre = '%s a=%s,b=%s;' % (T, a, b)
        if mn == 'add': core, f = 'T r=a+b;', dict(cf='r<a', of='(((a^r)&(b^r))>>%d)&1' % sb)
        elif mn in ('sub', 'cmp'): core, f = 'T r=a-b;', dict(cf='a<b', of='(((a^b)&(a^r))>>%d)&1' % sb)
        elif mn == 'adc': core, f = 'T cin=cf; T r=a+b+cin;', dict(cf='cin?r<=a:r<a', of='(((a^r)&(b^r))>>%d)&1' % sb)
        elif mn == 'sbb': core, f = 'T cin=cf; T r=a-b-cin;', dict(cf='cin?a<=b:a<b', of='(((a^b)&(a^r))>>%d)&1' % sb)
        else:
            op = {'and': '&', 'test': '&', 'or': '|', 'xor': '^'}[mn]
            core, f = 'T r=a%sb;' % op, dict(cf='0', of='0')
        s = '{%s%s%s' % (pre, core.replace('T', T), self.fl(need, **{**f, **com}))
        if mn not in ('cmp', 'test') and not nowrite: s += self.wrop(dst, 'r', S)
        return s + '}'

    def shift(self, mn, S, ops, need):
        T = 'uint%d_t' % (8 * S); ST_ = 'int%d_t' % (8 * S); B = 8 * S; sb = B - 1
        a = self.rdop(ops[0], S)
        cnt = self.rdop(ops[1], 1) if len(ops) > 1 else '1'
        mask = 63 if S == 8 else 31
        pre = '{%s a=%s; unsigned cnt=(unsigned)(%s)&%d; if(cnt){ uint64_t w=a;' % (T, a, cnt, mask)
        zsp = dict(zf='r==0', sf='(r>>%d)&1' % sb, pf='PAR(r)')
        if mn in ('shl', 'sal'):
            body = '%s r=(%s)(w<<cnt);' % (T, T) + self.fl(need, cf='cnt<=%d?(w>>(%d-cnt))&1:0' % (B, B), of='((r>>%d)&1)^(cnt<=%d?(w>>(%d-cnt))&1:0)' % (sb, B, B), **zsp)
        elif mn == 'shr':
            body = '%s r=(%s)(w>>cnt);' % (T, T) + self.fl(need, cf='(w>>(cnt-1))&1', of='(a>>%d)&1' % sb, **zsp)
        elif mn == 'sar':
            body = '%s s=(%s)a; unsigned k=cnt>%d?%d:cnt; %s r=(%s)(s>>k);' % (ST_, ST_, sb, sb, T, T) + \
                   self.fl(need, cf='(s>>(k-1))&1', of='0', **zsp)
        elif mn in ('rol', 'ror'):
            sh = ('(w<<k)|(w>>(%d-k))' if mn == 'rol' else '(w>>k)|(w<<(%d-k))') % B
            body = 'unsigned k=cnt%%%d; %s r=k?(%s)(%s):a;' % (B, T, T, sh)
            if mn == 'rol': body += self.fl(need, cf='r&1', of='((r>>%d)&1)^(r&1)' % sb)
            else: body += self.fl(need, cf='(r>>%d)&1' % sb, of='((r>>%d)^(r>>%d))&1' % (sb, sb - 1))
        elif mn in ('rcl', 'rcr'):
            mod = '%%%d' % (B + 1) if B < 32 else ''
            if mn == 'rcl':
                body = '%s r=a; unsigned k=cnt%s; while(k--){unsigned nc=(r>>%d)&1;r=(%s)((r<<1)|cf);cf=nc;}' % (T, mod, sb, T)
                body += self.fl(need, of='((r>>%d)&1)^cf' % sb)
            else:
                body = '%s r=a; unsigned k=cnt%s; while(k--){unsigned nc=r&1;r=(%s)((r>>1)|((%s)cf<<%d));cf=nc;}' % (T, mod, T, T, sb)
                body += self.fl(need, of='((r>>%d)^(r>>%d))&1' % (sb, sb - 1))
        else: raise Unsup(mn)
        return pre + body + self.wrop(ops[0], 'r', S) + '}}'

    def cond(self, mn, pre):
        return COND[mn[len(pre):]]

    # main translate ---------------------------------------------------------
    def tr(self, i, need, tgt):
        """returns C statements. tgt(addr)-> goto statement for a code address."""
        self.i = i; mn = i.mnemonic.split()[-1]; pre = i.mnemonic.split()[:-1]
        ops = i.operands; n = len(ops); S = ops[0].size if n else 8; nx = i.address + i.size
        rep = 0xF3 in i.prefix or 0xF2 in i.prefix; repne = 0xF2 in i.prefix
        lock = 'lock' in pre or 0xF0 in i.prefix
        xmm0 = n and ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg).startswith('xmm')
        xmm1 = n > 1 and ops[1].type == X86_OP_REG and self.i.reg_name(ops[1].reg).startswith('xmm')
        mm0 = n and ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg).startswith('mm')
        mm1 = n > 1 and ops[1].type == X86_OP_REG and self.i.reg_name(ops[1].reg).startswith('mm')
        fence = '__atomic_thread_fence(__ATOMIC_SEQ_CST);' if lock else ''
        if mn in ('nop', 'endbr64', 'pause', 'fnop', 'prefetcht0', 'prefetchnta', 'prefetcht1', 'prefetcht2', 'prefetchw', 'lfence', 'sfence', 'ud0') or mn.startswith('nop'): return ''
        if mn in ('cli', 'sti', 'invd'): return ''  # privileged/no-op in the hosted user-mode model
        if mn == 'mfence': return '__atomic_thread_fence(__ATOMIC_SEQ_CST);'
        if mn in ('mov', 'movabs') and not xmm0 and not xmm1 and not mm0 and not mm1:
            if ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg) in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'):
                return '%s=(uint16_t)%s;' % (self.sreg(ops[0]), self.rdop(ops[1], 2))
            if ops[1].type == X86_OP_REG and self.i.reg_name(ops[1].reg) in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'):
                return self.wrop(ops[0], self.sreg(ops[1]), S)
            return self.wrop(ops[0], self.rdop(ops[1], S), S)
#!/usr/bin/env python3
"""Static recompiler: x86_64 Windows PE -> portable C (guest addr == host addr, mapped at ImageBase).
usage: aot_transpiler.py game.exe out_dir"""
import sys, os, struct, bisect
from collections import Counter
import pefile
from capstone import *
from capstone.x86 import *

F5 = ('cf', 'zf', 'sf', 'of', 'pf')
N64 = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi'] + ['r%d' % i for i in range(8, 16)]
REG = {}
for i, n in enumerate(N64): REG[n] = (i, 8, 0)
for i, n in enumerate(['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'] + ['r%dd' % i for i in range(8, 16)]): REG[n] = (i, 4, 0)
for i, n in enumerate(['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di'] + ['r%dw' % i for i in range(8, 16)]): REG[n] = (i, 2, 0)
for i, n in enumerate(['al', 'cl', 'dl', 'bl', 'spl', 'bpl', 'sil', 'dil'] + ['r%db' % i for i in range(8, 16)]): REG[n] = (i, 1, 0)
for i, n in enumerate(['ah', 'ch', 'dh', 'bh']): REG[n] = (i, 1, 1)

COND = {'o': 'of', 'no': '!of', 'b': 'cf', 'c': 'cf', 'nae': 'cf', 'ae': '!cf', 'nb': '!cf', 'nc': '!cf', 'e': 'zf', 'z': 'zf',
        'ne': '!zf', 'nz': '!zf', 'be': '(cf|zf)', 'na': '(cf|zf)', 'a': '!(cf|zf)', 'nbe': '!(cf|zf)', 's': 'sf', 'ns': '!sf',
        'p': 'pf', 'pe': 'pf', 'np': '!pf', 'po': '!pf', 'l': '(sf!=of)', 'nge': '(sf!=of)', 'ge': '(sf==of)', 'nl': '(sf==of)',
        'rcxz': '(r1==0)', 'ecxz': '((uint32_t)r1==0)', 'le': '(zf|(sf!=of))', 'ng': '(zf|(sf!=of))', 'g': '!(zf|(sf!=of))', 'nle': '!(zf|(sf!=of))'}

def _fm(kinds, f): return sum(getattr(sys.modules['capstone.x86'], 'X86_EFLAGS_%s_%s' % (k, f.upper())) for k in kinds)
WMASK = {f: _fm(('MODIFY', 'SET', 'RESET', 'UNDEFINED'), f) for f in F5}  # any of these bits is non-overlapping per flag
TMASK = {f: _fm(('TEST',), f) for f in F5}
MASK64 = (1 << 64) - 1

def image_fingerprint(path):
    value = 14695981039346656037
    with open(path, 'rb') as image:
        while True:
            chunk = image.read(1024 * 1024)
            if not chunk: break
            for byte in chunk:
                value = ((value ^ byte) * 1099511628211) & MASK64
    return value

class Unsup(Exception): pass

def is_term(mn): return mn in ('jmp', 'ljmp', 'ret', 'retf', 'iret', 'iretd', 'iretq', 'ud2', 'int3', 'hlt') or (mn[0] == 'j' and mn[1:] in COND)

class Rec:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        pe = self.pe
        mg, mc = pe.OPTIONAL_HEADER.Magic, pe.FILE_HEADER.Machine
        if not ((mg == 0x20b and mc == 0x8664) or (mg == 0x10b and mc == 0x14c)):
            sys.exit('ERROR: unsupported PE (machine 0x%x, magic 0x%x). Supported: x64 PE32+ and x86 PE32.' % (mc, mg))
        self.m32 = mg == 0x10b; self.psz = 4 if self.m32 else 8
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.image_hash = image_fingerprint(path)
        self.img = bytes(pe.get_memory_mapped_image())
        self.md = Cs(CS_ARCH_X86, CS_MODE_32 if self.m32 else CS_MODE_64); self.md.detail = True
        self.text = [(s.VirtualAddress + self.base, s.VirtualAddress + self.base + max(s.Misc_VirtualSize, s.SizeOfRawData))
                     for s in pe.sections if s.Characteristics & 0x20000000]
        self.insns = {}; self.leaders = set(); self.unsup = Counter(); self.funcs = []
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint

    def in_text(self, a): return any(lo <= a < hi for lo, hi in self.text)
    def rd(self, a, n):
        o = a - self.base
        return self.img[o:o + n] if 0 <= o < len(self.img) else b''
    def decode(self, a):
        b = self.rd(a, 15)
        return next(self.md.disasm(b, a, 1), None) if b else None

    # ---- discovery -------------------------------------------------------
    def seeds(self):
        s = {self.entry}
        pe = self.pe
        try:
            if self.m32: raise KeyError  # no .pdata in PE32
            d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]  # .pdata RUNTIME_FUNCTION
            for off in range(0, d.Size - 11, 12):
                b, e, _ = struct.unpack_from('<III', self.img, d.VirtualAddress + off)
                if b == 0: break
                self.funcs.append((b + self.base, e + self.base))
                s.add(b + self.base)
        except Exception: pass
        self.funcs.sort(); self.fstarts = [f[0] for f in self.funcs]
        for sec in pe.sections:  # absolute code pointers in data (vtables, callbacks)
            if sec.Characteristics & 0x20000000: continue
            o = sec.VirtualAddress; n = min(sec.Misc_VirtualSize, sec.SizeOfRawData) & ~(self.psz - 1)
            lo_, hi_ = self.base, self.base + len(self.img)
            for v in memoryview(self.img[o:o + n]).cast('I' if self.m32 else 'Q'):
                if lo_ <= v < hi_ and self.in_text(v): s.add(v)
        try:
            tls = pe.DIRECTORY_ENTRY_TLS.struct
            cb = tls.AddressOfCallBacks
            while cb:
                v = struct.unpack_from('<I' if self.m32 else '<Q', self.img, cb - self.base)[0]
                if not v: break
                s.add(v); cb += self.psz
        except Exception: pass
        return {a for a in s if self.in_text(a)}

    def func_range(self, a):
        k = bisect.bisect_right(self.fstarts, a) - 1
        if k >= 0 and self.funcs[k][0] <= a < self.funcs[k][1]: return self.funcs[k]
        return (a - 0x4000, a + 0x4000)

    def table_targets(self, jm, recent):
        """Heuristic jump-table recovery for `jmp reg` / `jmp [tbl+idx*8]`."""
        lo, hi = self.func_range(jm.address)
        lo = max(lo, self.text[0][0]); out = set()
        cands = []  # (table address, kind) kind 'rva' | 'abs' | 'rel:<base>'
        ops = jm.operands
        if ops[0].type == X86_OP_MEM and ops[0].mem.scale == self.psz and ops[0].mem.base == 0 and ops[0].mem.index:
            cands.append((ops[0].mem.disp & MASK64, 'abs'))
        leas = {}
        for i in recent:
            if i.mnemonic == 'lea' and i.operands[1].mem.base == X86_REG_RIP:
                leas[i.operands[0].reg] = i.address + i.size + i.operands[1].mem.disp
        for i in reversed(recent):
            for o in i.operands:
                if o.type == X86_OP_MEM and o.mem.scale == 4 and o.mem.index and o.mem.base != X86_REG_RIP:
                    if o.mem.base in leas: cands.append((leas[o.mem.base], 'rel:%d' % leas[o.mem.base]))
                    if o.mem.disp > 0 and not self.m32: cands.append((self.base + o.mem.disp, 'rva'))
        for tab, kind in cands:
            found = []
            for k in range(0, 2048):
                if kind == 'abs': v = struct.unpack('<Q', self.rd(tab + self.psz * k, self.psz).ljust(8, b'\0'))[0]
                else:
                    raw = self.rd(tab + 4 * k, 4)
                    if len(raw) < 4: break
                    v = struct.unpack('<I', raw)[0] + self.base if kind == 'rva' else (tab + struct.unpack('<i', raw)[0]) & MASK64
                if lo <= v < hi + 1 and self.in_text(v): found.append(v)
                else: break
            if found: out.update(found); break
        return out

    def discover(self):
        work = list(self.seeds()); self.leaders |= set(work)
        while work:
            a = work.pop(); recent = []
            while a not in self.insns:
                i = self.decode(a)
                if i is None or not self.in_text(a): break
                self.insns[a] = i; recent = (recent + [i])[-120:]
                mn = i.mnemonic.split()[-1]; nxt = a + i.size
                direct = i.operands and i.operands[0].type == X86_OP_IMM
                if mn in ('lea', 'mov', 'movabs', 'push') and len(i.operands) > 0:  # code pointers materialised by code
                    o = i.operands[-1]
                    t = (i.address + i.size + o.mem.disp if mn == 'lea' and o.mem.base == X86_REG_RIP else o.imm if o.type == X86_OP_IMM else 0)
                    if t and t not in self.leaders and self.in_text(t) and t != nxt: self.leaders.add(t); work.append(t)
                if mn in ('jmp', 'ljmp'):
                    target = i.operands[-1] if mn == 'ljmp' else i.operands[0]
                    if target.type == X86_OP_IMM: self.leaders.add(target.imm); work.append(target.imm)
                    elif mn == 'jmp':
                        for t in self.table_targets(i, recent): self.leaders.add(t); work.append(t)
                    break
                if (mn[0] == 'j' and mn[1:] in COND) or mn.startswith('loop'):
                    t = i.operands[0].imm; self.leaders.update((t, nxt)); work.append(t)
                elif mn in ('call', 'lcall'):
                    self.leaders.add(nxt)
                    target = i.operands[-1] if mn == 'lcall' else i.operands[0]
                    if target.type == X86_OP_IMM: self.leaders.add(target.imm); work.append(target.imm)
                elif is_term(mn): break
                a = nxt
            else:
                # Decoding ran into an already-decoded address: two instruction chains converge here (overlapping decodes from a
                # mid-instruction seed). That address must be a block entry, or the real chain falls through into a block that
                # swallowed it and has no case label.
                self.leaders.add(a)
        return self

    def blocks(self):
        bl = []; cur = None; prev = None
        for a in sorted(self.insns):
            i = self.insns[a]
            if cur is None or a in self.leaders or prev is None or prev.address + prev.size != a or is_term(prev.mnemonic.split()[-1]) \
               or prev.mnemonic.split()[-1] in ('call', 'lcall') or prev.mnemonic.split()[-1].startswith('loop') or (prev.mnemonic.split()[-1][0] == 'j'):
                cur = []; bl.append(cur)
            cur.append(i); prev = i
        return bl

# ---------------------------------------------------------------------------
class Gen:
    def __init__(self, rec):
        self.rec = rec; self.i = None; self.m32 = bool(getattr(rec, 'm32', False))

    # operands ---------------------------------------------------------------
    def reg(self, r):
        n = self.i.reg_name(r)
        if n not in REG: raise Unsup('reg ' + n)
        return REG[n]
    _mmx_ctx = None   # set while an MMX op is emulated through its SSE2 lane implementation (8-byte value in the low half)
    def xr(self, op):
        n = self.i.reg_name(op.reg)
        if self._mmx_ctx is not None and n.startswith('mm') and n[2:].isdigit(): return 'mx_d' if op is self._mmx_ctx else 'mx_s'
        if not n.startswith('xmm'): raise Unsup('reg ' + n)
        return 'c->x[%s]' % n[3:]
    def mmr(self, op):
        n = self.i.reg_name(op.reg)
        if not n.startswith('mm') or not n[2:].isdigit(): raise Unsup('reg ' + n)
        return 'c->mm[%s]' % n[2:]
    def sreg(self, op):
        n = self.i.reg_name(op.reg)
        if n not in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'): raise Unsup('reg ' + n)
        return 'c->sregs[%d]' % ('es cs ss ds fs gs'.split().index(n))
    def addr(self, op):
        m = op.mem; i = self.i
        if self.m32:
            p = []
            if m.base: p.append('r%d' % self.reg(m.base)[0])
            if m.index: p.append('r%d*%d' % (self.reg(m.index)[0], m.scale))
            if m.disp or not p: p.append('0x%xULL' % (m.disp & 0xFFFFFFFF))
            e = '(uint64_t)(uint32_t)(%s)' % '+'.join(p)
        elif m.base == X86_REG_RIP:
            e = '0x%xULL' % ((i.address + i.size + m.disp) & MASK64)
        else:
            p = []
            if m.base: p.append('r%d' % self.reg(m.base)[0])
            if m.index: p.append('r%d*%d' % (self.reg(m.index)[0], m.scale))
            if m.disp or not p: p.append('0x%xULL' % (m.disp & MASK64))
            e = '(uint64_t)(%s)' % '+'.join(p)
        if m.segment in (X86_REG_GS, X86_REG_FS): e = '(%s+c->gs)' % e
        return e
    def rdop(self, op, S=None):
        S = S or op.size; T = 'uint%d_t' % (8 * S)
        if op.type == X86_OP_REG:
            n, s, h = self.reg(op.reg)
            return '(uint8_t)(r%d>>8)' % n if h else '(%s)r%d' % (T if s == S else 'uint%d_t' % (8 * s), n)
        if op.type == X86_OP_IMM: return '((%s)0x%xULL)' % (T, op.imm & ((1 << (8 * S)) - 1))
        return 'LD(%s,%s)' % (T, self.addr(op))
    def wrop(self, op, v, S=None):
        S = S or op.size
        if op.type == X86_OP_MEM: return 'ST(uint%d_t,%s,%s);' % (8 * S, self.addr(op), v)
        n, s, h = self.reg(op.reg)
        if h: return 'r%d=(r%d&~0xFF00ULL)|((uint64_t)(uint8_t)(%s)<<8);' % (n, n, v)
        if s == 8: return 'r%d=(uint64_t)(%s);' % (n, v)
        if s == 4: return 'r%d=(uint32_t)(%s);' % (n, v)
        return 'r%d=(r%d&~0x%xULL)|(uint%d_t)(%s);' % (n, n, (1 << 8 * s) - 1, 8 * s, v)

    def fl(self, need, **kw):  # emit flag assignments that are live
        return ''.join('%s=%s;' % (f, e) for f, e in kw.items() if f in need)

    # ALU -----------------------------------------------------------------
    def alu(self, mn, S, a, b, dst, need, nowrite=False):
        T = 'uint%d_t' % (8 * S); sb = 8 * S - 1; ST_ = 'int%d_t' % (8 * S)
        com = dict(zf='r==0', sf='(r>>%d)&1' % sb, pf='PAR(r)')
        pre = '%s a=%s,b=%s;' % (T, a, b)
        if mn == 'add': core, f = 'T r=a+b;', dict(cf='r<a', of='(((a^r)&(b^r))>>%d)&1' % sb)
        elif mn in ('sub', 'cmp'): core, f = 'T r=a-b;', dict(cf='a<b', of='(((a^b)&(a^r))>>%d)&1' % sb)
        elif mn == 'adc': core, f = 'T cin=cf; T r=a+b+cin;', dict(cf='cin?r<=a:r<a', of='(((a^r)&(b^r))>>%d)&1' % sb)
        elif mn == 'sbb': core, f = 'T cin=cf; T r=a-b-cin;', dict(cf='cin?a<=b:a<b', of='(((a^b)&(a^r))>>%d)&1' % sb)
        else:
            op = {'and': '&', 'test': '&', 'or': '|', 'xor': '^'}[mn]
            core, f = 'T r=a%sb;' % op, dict(cf='0', of='0')
        s = '{%s%s%s' % (pre, core.replace('T', T), self.fl(need, **{**f, **com}))
        if mn not in ('cmp', 'test') and not nowrite: s += self.wrop(dst, 'r', S)
        return s + '}'

    def shift(self, mn, S, ops, need):
        T = 'uint%d_t' % (8 * S); ST_ = 'int%d_t' % (8 * S); B = 8 * S; sb = B - 1
        a = self.rdop(ops[0], S)
        cnt = self.rdop(ops[1], 1) if len(ops) > 1 else '1'
        mask = 63 if S == 8 else 31
        pre = '{%s a=%s; unsigned cnt=(unsigned)(%s)&%d; if(cnt){ uint64_t w=a;' % (T, a, cnt, mask)
        zsp = dict(zf='r==0', sf='(r>>%d)&1' % sb, pf='PAR(r)')
        if mn in ('shl', 'sal'):
            body = '%s r=(%s)(w<<cnt);' % (T, T) + self.fl(need, cf='cnt<=%d?(w>>(%d-cnt))&1:0' % (B, B), of='((r>>%d)&1)^(cnt<=%d?(w>>(%d-cnt))&1:0)' % (sb, B, B), **zsp)
        elif mn == 'shr':
            body = '%s r=(%s)(w>>cnt);' % (T, T) + self.fl(need, cf='(w>>(cnt-1))&1', of='(a>>%d)&1' % sb, **zsp)
        elif mn == 'sar':
            body = '%s s=(%s)a; unsigned k=cnt>%d?%d:cnt; %s r=(%s)(s>>k);' % (ST_, ST_, sb, sb, T, T) + \
                   self.fl(need, cf='(s>>(k-1))&1', of='0', **zsp)
        elif mn in ('rol', 'ror'):
            sh = ('(w<<k)|(w>>(%d-k))' if mn == 'rol' else '(w>>k)|(w<<(%d-k))') % B
            body = 'unsigned k=cnt%%%d; %s r=k?(%s)(%s):a;' % (B, T, T, sh)
            if mn == 'rol': body += self.fl(need, cf='r&1', of='((r>>%d)&1)^(r&1)' % sb)
            else: body += self.fl(need, cf='(r>>%d)&1' % sb, of='((r>>%d)^(r>>%d))&1' % (sb, sb - 1))
        elif mn in ('rcl', 'rcr'):
            mod = '%%%d' % (B + 1) if B < 32 else ''
            if mn == 'rcl':
                body = '%s r=a; unsigned k=cnt%s; while(k--){unsigned nc=(r>>%d)&1;r=(%s)((r<<1)|cf);cf=nc;}' % (T, mod, sb, T)
                body += self.fl(need, of='((r>>%d)&1)^cf' % sb)
            else:
                body = '%s r=a; unsigned k=cnt%s; while(k--){unsigned nc=r&1;r=(%s)((r>>1)|((%s)cf<<%d));cf=nc;}' % (T, mod, T, T, sb)
                body += self.fl(need, of='((r>>%d)^(r>>%d))&1' % (sb, sb - 1))
        else: raise Unsup(mn)
        return pre + body + self.wrop(ops[0], 'r', S) + '}}'

    def cond(self, mn, pre):
        return COND[mn[len(pre):]]

    # main translate ---------------------------------------------------------
    def tr(self, i, need, tgt):
        """returns C statements. tgt(addr)-> goto statement for a code address."""
        self.i = i; mn = i.mnemonic.split()[-1]; pre = i.mnemonic.split()[:-1]
        ops = i.operands; n = len(ops); S = ops[0].size if n else 8; nx = i.address + i.size
        rep = 0xF3 in i.prefix or 0xF2 in i.prefix; repne = 0xF2 in i.prefix
        lock = 'lock' in pre or 0xF0 in i.prefix
        xmm0 = n and ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg).startswith('xmm')
        xmm1 = n > 1 and ops[1].type == X86_OP_REG and self.i.reg_name(ops[1].reg).startswith('xmm')
        mm0 = n and ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg).startswith('mm')
        mm1 = n > 1 and ops[1].type == X86_OP_REG and self.i.reg_name(ops[1].reg).startswith('mm')
        fence = '__atomic_thread_fence(__ATOMIC_SEQ_CST);' if lock else ''
        if mn in ('nop', 'endbr64', 'pause', 'fnop', 'prefetcht0', 'prefetchnta', 'prefetcht1', 'prefetcht2', 'prefetchw', 'lfence', 'sfence', 'ud0', 'sldt', 'lldt', 'clts', 'verw', 'xabort', 'bndstx', 'maskmovq') or mn.startswith('nop'): return ''
        if mn in ('cli', 'sti', 'invd'): return ''  # privileged/no-op in the hosted user-mode model
        if mn == 'mfence': return '__atomic_thread_fence(__ATOMIC_SEQ_CST);'
        if mn in ('mov', 'movabs') and not xmm0 and not xmm1 and not mm0 and not mm1:
            if ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg) in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'):
                return '%s=(uint16_t)%s;' % (self.sreg(ops[0]), self.rdop(ops[1], 2))
            if ops[1].type == X86_OP_REG and self.i.reg_name(ops[1].reg) in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'):
                return self.wrop(ops[0], self.sreg(ops[1]), S)
            return self.wrop(ops[0], self.rdop(ops[1], S), S)
        if mn in ('add', 'sub', 'and', 'or', 'xor', 'cmp', 'test', 'adc', 'sbb'): return fence + self.alu(mn, S, self.rdop(ops[0], S), self.rdop(ops[1], S), ops[0], need)
        if mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'): return self.shift(mn, S, ops, need)
        if mn in ('movzx', 'movsx', 'movsxd'):
            S1 = ops[1].size; T0 = 'uint%d_t' % (8 * S)
            v = '(%s)%s' % (T0, self.rdop(ops[1], S1)) if mn == 'movzx' else '(%s)(int%d_t)%s' % (T0, 8 * S1, self.rdop(ops[1], S1))
            return self.wrop(ops[0], v, S)
        if mn == 'lea': return self.wrop(ops[0], '(uint%d_t)%s' % (8 * S, self.addr(ops[1])), S)
        m32 = self.m32; PS = 4 if m32 else 8; PU, PO = ('PUSH32', 'POP32') if m32 else ('PUSH', 'POP')
        if mn == 'push':
            v = self.sreg(ops[0]) if ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg) in ('es', 'cs', 'ss', 'ds', 'fs', 'gs') else self.rdop(ops[0], PS)
            return '%s(%s);' % (PU, v)
        if mn == 'pop':
            if ops[0].type == X86_OP_REG and self.i.reg_name(ops[0].reg) in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'):
                return '%s=(uint16_t)%s();' % (self.sreg(ops[0]), PO)
            return self.wrop(ops[0], '%s()' % PO, PS)
        if mn == 'leave': return 'r4=r5; r5=%s();' % PO
        if mn == 'enter':
            alloc = ops[0].imm & 0xffff; nesting = ops[1].imm & 31
            return '{uint%d_t frame; %s(r5); frame=r4;' % (8 * PS, PU) + \
                   ''.join('frame-=%d;%s(LD(uint%d_t,frame));' % (PS, PU, 8 * PS) for _ in range(max(0, nesting - 1))) + \
                   ('%s(frame);' % PU if nesting else '') + 'r5=r4;r4=(uint%d_t)(r4-%d);}' % (8 * PS, alloc)
        if m32 and mn in ('les', 'lds'):
            dst = self.wrop(ops[0], 'LD(uint32_t,%s)' % self.addr(ops[1]), 4)
            return '{%sc->sregs[%d]=LD(uint16_t,%s+4);}' % (dst, 0 if mn == 'les' else 3, self.addr(ops[1]))
        if mn == 'xlatb':
            a = '(uint32_t)(r3+(uint8_t)r0)' if m32 else '(r3+(uint8_t)r0)'
            return 'r0=(r0&~0xFFULL)|LD(uint8_t,%s);' % a
        if mn == 'salc': re
    if mn == 'salc': return 'r0=(r0&~0xFFULL)|(cf?0xFF:0);'
        if mn in ('aaa', 'a
uf.append('\n'.join(out))
        if (k + 1) % regions_per_file == 0 or k == len(regions) - 1:
            fn = os.path.join(outdir, 'aot_part%d.c' % len(files))
            with open(fn, 'w', encoding='utf-8') as output:
                output.write('\n'.join(buf))
            files.append(fn)
            buf = ['#include "aot_runtime.h"\n']
    tab = ['#include "aot_runtime.h"']
    tab += ['uint64_t region_%d(CPU*,uint64_t);' % k for k in range(len(regions))]
    tab.append('const int aot_is_pe32=%d;' % (1 if rec.m32 else 0))
    tab.append('const uint64_t aot_image_base=0x%xULL, aot_entry=0x%xULL, aot_image_hash=0x%xULL; const int aot_nregions=%d;' % (rec.base, rec.entry, rec.image_hash, len(regions)))
    tab.append('const uint64_t aot_region_lo[]={%s};' % ','.join('0x%xULL' % r[0][0].address for r in regions))
    tab.append('const uint64_t aot_region_hi[]={%s};' % ','.join('0x%xULL' % (r[-1][-1].address + r[-1][-1].size) for r in regions))
    tab.append('uint64_t (*const aot_region_fn[])(CPU*,uint64_t)={%s};' % ','.join('region_%d' % k for k in range(len(regions))))
    with open(os.path.join(outdir, 'aot_tab.c'), 'w', encoding='utf-8') as output:
        output.write('\n'.join(tab) + '\n')
    return len(rec.insns), len(blocks), len(regions), unsup

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    rec = Rec(sys.argv[1]).discover()
    ni, nb, nr, un = emit(rec, sys.argv[2])
    print('[+] %d instructions, %d blocks, %d regions' % (ni, nb, nr))
    if un: print('[!] unsupported instructions (become runtime traps): ' + ', '.join('%s x%d' % kv for kv in un.most_common()))
