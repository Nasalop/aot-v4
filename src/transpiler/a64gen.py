"""ARM64 assembly backend (opt-in).  x86 eax..edi live in x19..x26 for the whole program, x27 = CPU*, x28 = packed EFLAGS bits
(CF 0, PF 2, ZF 6, SF 7, OF 11), x29 = shadow return ring pointer (call/ret pairs go through native bl/ret x1; the dispatch table is reached with adrp, 4KB-aligned).  Every asm region shares one host frame, so a jump between regions is a plain `b`.
Anything not translated natively is one call to a tiny C function (uf_<addr>) made by the C backend: state goes through the CPU struct."""
from capstone.x86 import *
X = {X86_REG_EAX:0, X86_REG_ECX:1, X86_REG_EDX:2, X86_REG_EBX:3, X86_REG_ESP:4, X86_REG_EBP:5, X86_REG_ESI:6, X86_REG_EDI:7}
def W(n): return 'w%d' % (19 + n)
def XR(n): return 'x%d' % (19 + n)
FB = {'cf': 0, 'pf': 2, 'zf': 6, 'sf': 7, 'of': 11}
CC = {'e':'eq','z':'eq','ne':'ne','nz':'ne','b':'lo','c':'lo','nae':'lo','ae':'hs','nb':'hs','nc':'hs','be':'ls','na':'ls','a':'hi','nbe':'hi',
      'l':'lt','nge':'lt','ge':'ge','nl':'ge','le':'le','ng':'le','g':'gt','nle':'gt','s':'mi','ns':'pl','o':'vs','no':'vc'}
def mov32(d, v):   # materialise a 32-bit constant into register name d (w form)
    v &= 0xffffffff
    if v < 0x10000: return ['mov %s, #%d' % (d, v)]
    if v >= 0xffff0000: return ['mov %s, #%d' % (d, v - (1 << 32))] if False else ['movn %s, #%d' % (d, (~v) & 0xffff)]
    return ['movz %s, #%d' % (d, v & 0xffff), 'movk %s, #%d, lsl #16' % (d, v >> 16)]
def hashinfo(n):
    bits = max(4, (2 * n - 1).bit_length()); return bits, 32 - bits

class Fn(Exception): pass   # not translated natively: use the C fallback

class Blk:
    """Translates the instructions of one basic block into text lines. `fallback(i, need)` -> uf function name."""
    def __init__(s, fallback, haslabel, sh, uid):
        s.o = []; s.fallback = fallback; s.haslabel = haslabel; s.sh = sh; s.uid = uid; s.nl = 0; s.ftv = False; s.gb = None   # gb: 64KB-aligned guest address currently in x12 (block-local cache for absolute operands)
    def e(s, *l): s.o.extend(l)
    def lab(s): s.nl += 1; return '.Lt%s_%d' % (s.uid, s.nl)
    # ---------- operands ----------
    def reg(s, op): 
        if op.type == 1 and op.reg in X and op.size == 4: return X[op.reg]
        raise Fn()
    def addr(s, op, lea=False, al=4):   # -> (base_reg_str, imm_or_None) leaving the address in a register; may emit code.  Returns operand text for [..]
        m = op.mem; seg = False
        if m.segment:
            if m.segment not in (X86_REG_FS, X86_REG_GS) or lea: raise Fn()
            seg = True   # fs:/gs: = 32-bit offset + the TEB base kept in c->gs
        if m.base and m.base not in X or m.index and m.index not in X: raise Fn()
        b = XR(X[m.base]) if m.base else None; ix = X[m.index] if m.index else None; d = m.disp & 0xffffffff
        if d >= 0x80000000: d -= 1 << 32
        if not seg and not lea and ix is None and b is not None and (-256 <= d <= 255 or (0 <= d <= 4095 * al and d % al == 0)):
            return '[%s, #%d]' % (b, d)
        if b is None and ix is None and not seg and not lea and 0 <= d < 0x80000000 and (d & 0xffff) <= 4095 * al and (d & 0xffff) % al == 0:
            base = d & 0xffff0000   # globals: one movz for the 64KB page (cached for the rest of the block) + a scaled-immediate access
            if s.gb != base: s.e('movz w12, #%d, lsl #16' % (base >> 16)); s.gb = base
            return '[x12, #%d]' % (d & 0xffff)
        if b is None and ix is None and seg and not lea and 0 <= d <= 4095 * al and d % al == 0:
            s.e('ldr x10, [x27, #736]'); return '[x10, #%d]' % d
        if b is None and ix is None:
            s.e(*mov32('w10', d))
            if seg: s.e('ldr x11, [x27, #736]', 'add x10, x10, x11')
            return '[x10]'
        if m.scale not in (1, 2, 4, 8): raise Fn()
        sh = {1: 0, 2: 1, 4: 2, 8: 3}[m.scale]
        if b is None: s.e('lsl w10, %s, #%d' % (W(ix), sh))
        elif ix is None:
            if d and -0x1000000 < d < 0x1000000:   # base + large/odd displacement straight from the base register: 1-2 instructions instead of mov+mov+add
                bw = W(X[m.base]); mag = abs(d); aop = 'add' if d > 0 else 'sub'
                if mag >> 12: s.e('%s w10, %s, #%d, lsl #12' % (aop, bw, mag >> 12)); bw = 'w10'
                if mag & 0xfff: s.e('%s w10, %s, #%d' % (aop, bw, mag & 0xfff))
                d = 0
            else: s.e('mov w10, %s' % W(X[m.base]))
        else: s.e('add w10, %s, %s, lsl #%d' % (W(X[m.base]), W(ix), sh))
        if d and not seg and not lea and (-256 <= d <= 255 or (0 <= d <= 4095 * al and d % al == 0)): return '[x10, #%d]' % d
        if d:
            if 0 < d < 4096: s.e('add w10, w10, #%d' % d)
            elif -4096 < d < 0: s.e('sub w10, w10, #%d' % -d)
            else: s.e(*mov32('w11', d), 'add w10, w10, w11')
        if seg: s.e('ldr x11, [x27, #736]', 'add x10, x10, x11')
        return '[x10]'
    def val(s, op, t):   # source value as a w register name; t = scratch
        if op.type == 1: return W(s.reg(op))
        if op.type == 2:
            v = op.imm & 0xffffffff
            if v == 0: return 'wzr'
            s.e(*mov32(t, v)); return t
        if op.type == 3:
            if op.size != 4: raise Fn()
            a = s.addr(op); s.e('ldr %s, %s' % (t, a)); return t
        raise Fn()
    # ---------- flags ----------
    def setf(s, nd, kind, res):
        """After an arithmetic/logic op that set NZCV (kind add/sub/logic) materialise the flags in `nd` into x28.  res = w register with the result."""
        for f in ('cf', 'zf', 'sf', 'of'):
            if f not in nd: continue
            p = FB[f]
            if kind == 'logic' and f in ('cf', 'of'): s.e('bfi x28, xzr, #%d, #1' % p); continue
            cond = {'cf': 'cs' if kind == 'add' else 'cc', 'zf': 'eq', 'sf': 'mi', 'of': 'vs'}[f]
            s.e('cset w1, %s' % cond, 'bfi x28, x1, #%d, #1' % p)
        if 'pf' in nd:
            s.e('eor w1, %s, %s, lsr #4' % (res, res), 'eor w1, w1, w1, lsr #2', 'eor w1, w1, w1, lsr #1', 'mvn w1, w1', 'bfi x28, x1, #2, #1')
    # ---------- instruction ----------
    def ins(s, i, nd, fuse=None, arm=None):
        mn = i.mnemonic; ops = i.operands; n = len(ops)
        if mn in ('nop', 'wait', 'fwait'): return
        lock = mn.startswith('lock ')
        if lock: mn = mn[5:]
        if lock or mn in ('xadd', 'cmpxchg', 'xchg'): return s.atomic(i, mn, nd, lock)
        if (mn.startswith('set') and mn[3:] in CCN) or (mn.startswith('cmov') and mn[4:] in CCN): return s.cc_consume(i, arm)
        if mn == 'mov' and n == 2 and ops[0].size == 4:
            d, sr = ops
            if d.type == 1:
                dr = s.reg(d)
                if sr.type == 1: s.e('mov %s, %s' % (W(dr), W(s.reg(sr))))
                elif sr.type == 2: s.e(*mov32(W(dr), sr.imm))
                elif sr.type == 3: s.e('ldr %s, %s' % (W(dr), s.addr(sr)))
                else: raise Fn()
                return
            if d.type == 3:
                v = s.val(sr, 'w0') if sr.type != 3 else None
                if v is None: raise Fn()
                s.e('str %s, %s' % (v, s.addr(d))); return
            raise Fn()
        if mn == 'lea' and n == 2 and ops[0].type == 1 and ops[0].size == 4:
            dr = s.reg(ops[0]); m = ops[1].mem
            if m.segment: raise Fn()
            if m.base and not m.index and -4096 < m.disp < 4096 and m.base in X:
                d = m.disp
                if d >= 0: s.e('add %s, %s, #%d' % (W(dr), W(X[m.base]), d))
                else: s.e('sub %s, %s, #%d' % (W(dr), W(X[m.base]), -d))
                return
            s.addr(ops[1], lea=True); L = s.o[-1] if s.o else ''
            if L.split(' ', 2)[0] in ('add', 'sub', 'lsl', 'mov') and L.split(' ', 2)[1] == 'w10,': s.o[-1] = L.replace('w10,', W(dr) + ',', 1)   # the last instruction of the address computation writes w10: aim it at the destination
            else: s.e('mov %s, w10' % W(dr))
            return
        if mn in ('add', 'sub', 'and', 'or', 'xor', 'cmp', 'test') and n == 2 and ops[0].size == 4:
            return s.alu(mn, ops, nd, fuse)
        if mn in ('inc', 'dec') and n == 1 and ops[0].type == 1 and ops[0].size == 4:
            r = s.reg(ops[0]); need = nd & {'zf', 'sf', 'of', 'pf'}
            if need: s.e('%s w0, %s, #1' % ('adds' if mn == 'inc' else 'subs', W(r)))
            else: s.e('%s %s, %s, #1' % ('add' if mn == 'inc' else 'sub', W(r), W(r))); return
            s.setf(need, 'add' if mn == 'inc' else 'sub', 'w0'); s.e('mov %s, w0' % W(r)); return
        if mn == 'push' and n == 1 and ops[0].size == 4:
            v = s.val(ops[0], 'w0')
            if v == W(4): s.e('mov w0, w23'); v = 'w0'   # push esp stores the value from BEFORE the decrement
            s.e('str %s, [x23, #-4]!' % v); return
        if mn == 'pop' and n == 1 and ops[0].type == 1 and ops[0].size == 4:
            r = s.reg(ops[0])
            if r == 4: raise Fn()
            s.e('ldr %s, [x23], #4' % W(r)); return
        return s.ins2(i, nd, fuse)

    R8 = {X86_REG_AL: (0, 0), X86_REG_CL: (1, 0), X86_REG_DL: (2, 0), X86_REG_BL: (3, 0), X86_REG_AH: (0, 8), X86_REG_CH: (1, 8), X86_REG_DH: (2, 8), X86_REG_BH: (3, 8)}
    R16 = {X86_REG_AX: 0, X86_REG_CX: 1, X86_REG_DX: 2, X86_REG_BX: 3, X86_REG_SP: 4, X86_REG_BP: 5, X86_REG_SI: 6, X86_REG_DI: 7}
    def ldv(s, op, t):   # zero-extended 1/2/4-byte source value into w register t
        sz = op.size
        if op.type == 2: s.e(*mov32(t, op.imm & ((1 << (8 * sz)) - 1))); return t
        if op.type == 1:
            if sz == 4: return W(s.reg(op))
            if sz == 1:
                if op.reg not in s.R8: raise Fn()
                r, sh = s.R8[op.reg]; s.e('ubfx %s, %s, #%d, #8' % (t, W(r), sh)); return t
            if sz == 2:
                if op.reg not in s.R16: raise Fn()
                s.e('ubfx %s, %s, #0, #16' % (t, W(s.R16[op.reg]))); return t
            raise Fn()
        if op.type == 3:
            a = s.addr(op, al=sz); s.e('%s %s, %s' % ({1: 'ldrb', 2: 'ldrh', 4: 'ldr'}[sz], t, a)); return t
        raise Fn()
    def stv(s, op, v):   # store w register v (low bits) into a 1/2/4-byte destination
        sz = op.size
        if op.type == 1:
            if sz == 4: s.e('mov %s, %s' % (W(s.reg(op)), v)); return
            if sz == 1:
                if op.reg not in s.R8: raise Fn()
                r, sh = s.R8[op.reg]; s.e('bfi %s, %s, #%d, #8' % (XR(r), v.replace('w', 'x', 1) if v != 'wzr' else 'xzr', sh)); return
            if sz == 2:
                if op.reg not in s.R16: raise Fn()
                s.e('bfi %s, %s, #0, #16' % (XR(s.R16[op.reg]), v.replace('w', 'x', 1) if v != 'wzr' else 'xzr')); return
            raise Fn()
        if op.type == 3:
            a = s.addr(op, al=sz); s.e('%s %s, %s' % ({1: 'strb', 2: 'strh', 4: 'str'}[sz], v, a)); return
        raise Fn()
    XOFF = 128   # offsetof(CPU, x)
    def xmm(s, op):
        if op.type != 1 or not (X86_REG_XMM0 <= op.reg <= X86_REG_XMM7): raise Fn()
        return s.XOFF + 16 * (op.reg - X86_REG_XMM0)
    def sse_move(s, i, ops):
        mn = i.mnemonic; d, sr = ops
        if mn in ('movaps', 'movups', 'movdqa', 'movdqu', 'movapd', 'movupd'):
            if d.type == 1 and sr.type == 1: s.e('ldr q0, [x27, #%d]' % s.xmm(sr), 'str q0, [x27, #%d]' % s.xmm(d)); return
            if d.type == 1 and sr.type == 3: a = s.addr(sr, al=16); s.e('ldr q0, %s' % a, 'str q0, [x27, #%d]' % s.xmm(d)); return
            if d.type == 3 and sr.type == 1: o = s.xmm(sr); a = s.addr(d, al=16); s.e('ldr q0, [x27, #%d]' % o, 'str q0, %s' % a); return
        if mn == 'movss':
            if d.type == 1 and sr.type == 3: a = s.addr(sr); s.e('ldr s0, %s' % a, 'str q0, [x27, #%d]' % s.xmm(d)); return
            if d.type == 3 and sr.type == 1: o = s.xmm(sr); a = s.addr(d); s.e('ldr w0, [x27, #%d]' % o, 'str w0, %s' % a); return
            if d.type == 1 and sr.type == 1: s.e('ldr w0, [x27, #%d]' % s.xmm(sr), 'str w0, [x27, #%d]' % s.xmm(d)); return
        raise Fn()
    # ---------- x87 (st(i) slots live in c->fst[(ftop+i)&7] as doubles; ftop is cached in w15 for the rest of the block once loaded) ----------
    FST = 768; FCW = 832; FSW = 834; FTOP = 836
    def ftl(s):
        if not s.ftv: s.e('ldrb w15, [x27, #%d]' % s.FTOP, 'add x14, x27, #%d' % s.FST); s.ftv = True   # w15 = TOP, x14 = &st[0] for the rest of the block
    def fslot(s, k, r):   # x<r> <- address of ST(k); returns the operand text
        s.ftl()
        if k == 0: return '[x14, x15, lsl #3]'   # register-offset addressing: no address arithmetic for ST(0)
        s.e('add w%d, w15, #%d' % (r, k), 'and w%d, w%d, #7' % (r, r)); return '[x14, x%d, lsl #3]' % r
    def fpush(s): s.ftl(); s.e('add w15, w15, #7', 'and w15, w15, #7', 'strb w15, [x27, #%d]' % s.FTOP)
    def fpop(s): s.ftl(); s.e('add w15, w15, #1', 'and w15, w15, #7', 'strb w15, [x27, #%d]' % s.FTOP)
    def sti(s, op):
        if op.type != 1 or not (X86_REG_ST0 <= op.reg <= X86_REG_ST7): raise Fn()
        return op.reg - X86_REG_ST0
    def fmem(s, op, mn, dreg):   # memory operand converted to a double in dreg (d0/d1)
        isint = mn.startswith('fi')
        if op.type != 3: raise Fn()
        if isint:
            if op.size == 2: a = s.addr(op, al=2); s.e('ldrsh w3, %s' % a, 'scvtf %s, w3' % dreg)
            elif op.size == 4: a = s.addr(op); s.e('ldr w3, %s' % a, 'scvtf %s, w3' % dreg)
            elif op.size == 8: a = s.addr(op, al=8); s.e('ldr x3, %s' % a, 'scvtf %s, x3' % dreg)
            else: raise Fn()
        elif op.size == 4: a = s.addr(op); s.e('ldr s3, %s' % a, 'fcvt %s, s3' % dreg)
        elif op.size == 8: a = s.addr(op, al=8); s.e('ldr %s, %s' % (dreg, a))
        else: raise Fn()
    def x87(s, i, nd):
        mn = i.mnemonic; ops = i.operands; n = len(ops)
        base = mn[:-1] if mn.endswith('p') and mn[1:-1] in ('add', 'sub', 'subr', 'mul', 'div', 'divr') else mn
        pop = base != mn
        for nm in ('fadd', 'fsub', 'fsubr', 'fmul', 'fdiv', 'fdivr', 'fiadd', 'fisub', 'fisubr', 'fimul', 'fidiv', 'fidivr'):
            if base != nm: continue
            k = (nm.replace('fi', 'f', 1) if nm.startswith('fi') else nm)[1:]
            if n == 1 and ops[0].type == 3:
                s.fmem(ops[0], mn, 'd1'); a0 = s.fslot(0, 0); s.e('ldr d0, %s' % a0); dst = a0; pp = False
            else:
                if n == 1: di, si = (s.sti(ops[0]), 0) if pop else (0, s.sti(ops[0]))
                elif n == 2: di, si = s.sti(ops[0]), s.sti(ops[1])
                else: raise Fn()
                ad = s.fslot(di, 0); asr = s.fslot(si, 1); s.e('ldr d0, %s' % ad, 'ldr d1, %s' % asr); dst = ad; pp = pop
            if k in ('subr', 'divr'): s.e('%s d0, d1, d0' % {'subr': 'fsub', 'divr': 'fdiv'}[k])
            else: s.e('%s d0, d0, d1' % {'add': 'fadd', 'sub': 'fsub', 'mul': 'fmul', 'div': 'fdiv'}[k])
            s.e('str d0, %s' % dst)
            if pp: s.fpop()
            return
        if mn in ('fld', 'fild'):
            if ops[0].type == 3: s.fmem(ops[0], mn, 'd0')
            else: a = s.fslot(s.sti(ops[0]), 0); s.e('ldr d0, %s' % a)
            s.fpush(); a = s.fslot(0, 0); s.e('str d0, %s' % a); return
        if mn in ('fst', 'fstp'):
            o = ops[0]; a = s.fslot(0, 0); pp = mn == 'fstp'
            if o.type == 3:
                if o.size == 4: s.e('ldr d0, %s' % a, 'fcvt s0, d0'); s.e('str s0, %s' % s.addr(o))
                elif o.size == 8: s.e('ldr d0, %s' % a); s.e('str d0, %s' % s.addr(o, al=8))
                else: raise Fn()
            else: b = s.fslot(s.sti(o), 1); s.e('ldr d0, %s' % a, 'str d0, %s' % b)
            if pp: s.fpop()
            return
        if mn == 'fxch':
            k = s.sti(ops[-1]) if n else 1; a = s.fslot(0, 0); b = s.fslot(k, 1); s.e('ldr d0, %s' % a, 'ldr d1, %s' % b, 'str d1, %s' % a, 'str d0, %s' % b); return
        if mn in ('fchs', 'fabs', 'fsqrt'):
            a = s.fslot(0, 0); s.e('ldr d0, %s' % a, '%s d0, d0' % {'fchs': 'fneg', 'fabs': 'fabs', 'fsqrt': 'fsqrt'}[mn], 'str d0, %s' % a); return
        if mn in ('fldz', 'fld1'):
            s.fpush(); a = s.fslot(0, 0)
            if mn == 'fldz': s.e('str xzr, %s' % a)
            else: s.e('fmov d0, #1.0', 'str d0, %s' % a)
            return
        if mn in ('fcom', 'fcomp', 'fucom', 'fucomp', 'fcompp', 'fucompp'):
            a = s.fslot(0, 0); s.e('ldr d0, %s' % a)
            if n and ops[0].type == 3: s.fmem(ops[0], mn, 'd1')
            else: b = s.fslot(s.sti(ops[0]) if n else 1, 1); s.e('ldr d1, %s' % b)
            s.e('fcmp d0, d1', 'mov w2, #0', 'mov w3, #256', 'csel w2, w3, w2, mi', 'mov w3, #16384', 'csel w2, w3, w2, eq', 'mov w3, #17664', 'csel w2, w3, w2, vs',
                'ldrh w4, [x27, #%d]' % s.FSW, 'bic w4, w4, w3' if False else 'mov w3, #17664', 'bic w4, w4, w3', 'orr w4, w4, w2', 'strh w4, [x27, #%d]' % s.FSW)
            if mn.endswith('pp'): s.fpop(); s.fpop()
            elif mn.endswith('p'): s.fpop()
            return
        if mn in ('fcomi', 'fucomi', 'fcompi', 'fucompi', 'fcomip', 'fucomip') and n in (1, 2):
            k = s.sti(ops[-1]); a = s.fslot(0, 0); b = s.fslot(k, 1); s.e('ldr d0, %s' % a, 'ldr d1, %s' % b, 'fcmp d0, d1')
            need = nd & set(FB)
            if 'cf' in need: s.e('cset w1, lo', 'cset w2, vs', 'orr w1, w1, w2', 'bfi x28, x1, #0, #1')
            if 'zf' in need: s.e('cset w1, eq', 'cset w2, vs', 'orr w1, w1, w2', 'bfi x28, x1, #6, #1')
            if 'pf' in need: s.e('cset w1, vs', 'bfi x28, x1, #2, #1')
            if 'sf' in need: s.e('bfi x28, xzr, #7, #1')
            if 'of' in need: s.e('bfi x28, xzr, #11, #1')
            if mn in ('fcompi', 'fucompi', 'fcomip', 'fucomip'): s.fpop()
            return
        if mn == 'fnstsw' and n == 1:
            o = ops[0]; s.ftl()
            s.e('ldrh w0, [x27, #%d]' % s.FSW, 'mov w1, #14336', 'bic w0, w0, w1', 'orr w0, w0, w15, lsl #11')
            if o.type == 1 and o.reg == X86_REG_AX: s.e('bfi x19, x0, #0, #16'); return
            if o.type == 3 and o.size == 2: s.e('strh w0, %s' % s.addr(o, al=2)); return
            raise Fn()
        if mn == 'fnstcw' and n == 1 and ops[0].type == 3 and ops[0].size == 2: s.e('ldrh w0, [x27, #%d]' % s.FCW); s.e('strh w0, %s' % s.addr(ops[0], al=2)); return
        if mn == 'fldcw' and n == 1 and ops[0].type == 3 and ops[0].size == 2: s.e('ldrh w0, %s' % s.addr(ops[0], al=2)); s.e('strh w0, [x27, #%d]' % s.FCW); return
        raise Fn()
    # ---------- SSE arithmetic / logic / conversion ----------
    def vsrc(s, op, reg, al):
        if op.type == 1: s.e('ldr %s, [x27, #%d]' % (reg, s.xmm(op)))
        elif op.type == 3: s.e('ldr %s, %s' % (reg, s.addr(op, al=al)))
        else: raise Fn()
    def sse(s, i, nd):
        mn = i.mnemonic; ops = i.operands; n = len(ops)
        if n == 3 and mn == 'shufps': pass
        elif n != 2: raise Fn()
        d, sr = ops[0], ops[1]
        if mn in ('movd',):
            if d.type == 1 and d.size == 16 or d.type == 1 and X86_REG_XMM0 <= d.reg <= X86_REG_XMM7:
                od = s.xmm(d)
                if sr.type == 1 and sr.reg in X: s.e('fmov s0, %s' % W(X[sr.reg]))
                elif sr.type == 3: s.e('ldr s0, %s' % s.addr(sr))
                else: raise Fn()
                s.e('str q0, [x27, #%d]' % od); return
            if sr.type == 1 and X86_REG_XMM0 <= sr.reg <= X86_REG_XMM7:
                o = s.xmm(sr)
                if d.type == 1 and d.reg in X: s.e('ldr %s, [x27, #%d]' % (W(X[d.reg]), o)); return
                if d.type == 3: s.e('ldr w0, [x27, #%d]' % o); s.e('str w0, %s' % s.addr(d)); return
            raise Fn()
        if mn in ('movq', 'movsd', 'movlps', 'movhps', 'movlpd', 'movhpd', 'movhlps', 'movlhps'):
            if d.type == 1 and X86_REG_XMM0 <= d.reg <= X86_REG_XMM7:
                od = s.xmm(d)
                if mn in ('movq', 'movsd') and sr.type == 3: s.e('ldr d0, %s' % s.addr(sr, al=8), 'str q0, [x27, #%d]' % od); return
                if mn in ('movq', 'movsd') and sr.type == 1: s.e('ldr d0, [x27, #%d]' % s.xmm(sr)); s.e('str q0, [x27, #%d]' % od if mn == 'movq' else 'str d0, [x27, #%d]' % od); return
                if mn in ('movlps', 'movlpd') and sr.type == 3: s.e('ldr d0, %s' % s.addr(sr, al=8), 'str d0, [x27, #%d]' % od); return
                if mn in ('movhps', 'movhpd') and sr.type == 3: s.e('ldr d0, %s' % s.addr(sr, al=8), 'str d0, [x27, #%d]' % (od + 8)); return
                if mn == 'movhlps' and sr.type == 1: s.e('ldr d0, [x27, #%d]' % (s.xmm(sr) + 8), 'str d0, [x27, #%d]' % od); return
                if mn == 'movlhps' and sr.type == 1: s.e('ldr d0, [x27, #%d]' % s.xmm(sr), 'str d0, [x27, #%d]' % (od + 8)); return
                raise Fn()
            if d.type == 3 and sr.type == 1 and mn in ('movq', 'movsd', 'movlps', 'movlpd', 'movhps', 'movhpd'):
                o = s.xmm(sr) + (8 if mn in ('movhps', 'movhpd') else 0); s.e('ldr d0, [x27, #%d]' % o); s.e('str d0, %s' % s.addr(d, al=8)); return
            raise Fn()
        if d.type != 1 or not (X86_REG_XMM0 <= d.reg <= X86_REG_XMM7): raise Fn()
        od = s.xmm(d)
        import re as _re
        if mn in ('pslldq', 'psrldq') and sr.type == 2:
            c = sr.imm & 255; s.e('ldr q0, [x27, #%d]' % od)
            if c > 15: s.e('movi v0.2d, #0')
            elif c:
                s.e('movi v1.2d, #0')
                s.e('ext v0.16b, v1.16b, v0.16b, #%d' % (16 - c) if mn == 'pslldq' else 'ext v0.16b, v0.16b, v1.16b, #%d' % c)
            s.e('str q0, [x27, #%d]' % od); return
        mc = _re.match(r'cmp(eq|lt|le|neq|nlt|nle)ps$', mn)
        if mc:
            k = mc.group(1); s.vsrc(sr, 'q1', 16); s.e('ldr q0, [x27, #%d]' % od)
            s.e({'eq': 'fcmeq v0.4s, v0.4s, v1.4s', 'neq': 'fcmeq v0.4s, v0.4s, v1.4s', 'lt': 'fcmgt v0.4s, v1.4s, v0.4s', 'nlt': 'fcmgt v0.4s, v1.4s, v0.4s', 'le': 'fcmge v0.4s, v1.4s, v0.4s', 'nle': 'fcmge v0.4s, v1.4s, v0.4s'}[k])
            if k[0] == 'n': s.e('mvn v0.16b, v0.16b')
            s.e('str q0, [x27, #%d]' % od); return
        m = _re.match(r'(add|sub|mul|div|sqrt|min|max)(ss|sd|ps|pd)$', mn)
        if m:
            op, ty = m.groups()
            if ty in ('ss', 'sd'):
                r = 's' if ty == 'ss' else 'd'; al = 4 if ty == 'ss' else 8
                s.vsrc(sr, r + '1', al)
                if op == 'sqrt': s.e('fsqrt %s0, %s1' % (r, r))
                else:
                    s.e('ldr %s0, [x27, #%d]' % (r, od))
                    if op in ('min', 'max'): s.e('fcmp %s0, %s1' % (r, r), 'fcsel %s0, %s0, %s1, %s' % (r, r, r, 'mi' if op == 'min' else 'gt'))
                    else: s.e('%s %s0, %s0, %s1' % ({'add': 'fadd', 'sub': 'fsub', 'mul': 'fmul', 'div': 'fdiv'}[op], r, r, r))
                s.e('str %s0, [x27, #%d]' % (r, od)); return
            if op in ('min', 'max'): raise Fn()
            t = '4s' if ty == 'ps' else '2d'; s.vsrc(sr, 'q1', 16)
            if op == 'sqrt': s.e('fsqrt v0.%s, v1.%s' % (t, t))
            else: s.e('ldr q0, [x27, #%d]' % od, '%s v0.%s, v0.%s, v1.%s' % ({'add': 'fadd', 'sub': 'fsub', 'mul': 'fmul', 'div': 'fdiv'}[op], t, t, t))
            s.e('str q0, [x27, #%d]' % od); return
        LG = {'xorps': 'eor', 'xorpd': 'eor', 'pxor': 'eor', 'andps': 'and', 'andpd': 'and', 'pand': 'and', 'orps': 'orr', 'orpd': 'orr', 'por': 'orr', 'andnps': 'andn', 'andnpd': 'andn', 'pandn': 'andn'}
        if mn in LG:
            if LG[mn] == 'eor' and sr.type == 1 and sr.reg == d.reg: s.e('stp xzr, xzr, [x27, #%d]' % od); return
            s.vsrc(sr, 'q1', 16); s.e('ldr q0, [x27, #%d]' % od)
            if LG[mn] == 'andn': s.e('bic v0.16b, v1.16b, v0.16b')
            else: s.e('%s v0.16b, v0.16b, v1.16b' % LG[mn])
            s.e('str q0, [x27, #%d]' % od); return
        if mn == 'cvtss2sd': s.vsrc(sr, 's1', 4); s.e('fcvt d1, s1', 'str d1, [x27, #%d]' % od); return
        if mn == 'cvtsd2ss': s.vsrc(sr, 'd1', 8); s.e('fcvt s1, d1', 'str s1, [x27, #%d]' % od); return
        if mn in ('cvtsi2sd', 'cvtsi2ss'):
            if sr.type == 1 and sr.reg in X: w = W(X[sr.reg])
            elif sr.type == 3 and sr.size == 4: s.e('ldr w0, %s' % s.addr(sr)); w = 'w0'
            else: raise Fn()
            r = 'd' if mn.endswith('sd') else 's'; s.e('scvtf %s0, %s' % (r, w), 'str %s0, [x27, #%d]' % (r, od)); return
        if mn == 'cvtps2pd': s.vsrc(sr, 'd1', 8); s.e('fcvtl v0.2d, v1.2s', 'str q0, [x27, #%d]' % od); return
        if mn == 'cvtpd2ps': s.vsrc(sr, 'q1', 16); s.e('fcvtn v0.2s, v1.2d', 'str q0, [x27, #%d]' % od); return
        if mn == 'cvtdq2ps': s.vsrc(sr, 'q1', 16); s.e('scvtf v0.4s, v1.4s', 'str q0, [x27, #%d]' % od); return
        if mn in ('unpcklps', 'unpckhps'):
            s.vsrc(sr, 'q1', 16); s.e('ldr q0, [x27, #%d]' % od, '%s v0.4s, v0.4s, v1.4s' % ('zip1' if mn == 'unpcklps' else 'zip2'), 'str q0, [x27, #%d]' % od); return
        if mn == 'shufps' and n == 3 and ops[2].type == 2:
            im = ops[2].imm & 255; s.vsrc(sr, 'q1', 16); s.e('ldr q0, [x27, #%d]' % od)
            s.e('mov v2.s[0], v0.s[%d]' % (im & 3), 'mov v2.s[1], v0.s[%d]' % ((im >> 2) & 3), 'mov v2.s[2], v1.s[%d]' % ((im >> 4) & 3), 'mov v2.s[3], v1.s[%d]' % ((im >> 6) & 3), 'str q2, [x27, #%d]' % od); return
        if mn in ('comiss', 'ucomiss', 'comisd', 'ucomisd'):
            r = 's' if mn.endswith('ss') else 'd'; s.vsrc(sr, r + '1', 4 if r == 's' else 8); s.e('ldr %s0, [x27, #%d]' % (r, od), 'fcmp %s0, %s1' % (r, r))
            need = nd & set(FB)
            if 'cf' in need: s.e('cset w1, lo', 'cset w2, vs', 'orr w1, w1, w2', 'bfi x28, x1, #0, #1')
            if 'zf' in need: s.e('cset w1, eq', 'cset w2, vs', 'orr w1, w1, w2', 'bfi x28, x1, #6, #1')
            if 'pf' in need: s.e('cset w1, vs', 'bfi x28, x1, #2, #1')
            if 'sf' in need: s.e('bfi x28, xzr, #7, #1')
            if 'of' in need: s.e('bfi x28, xzr, #11, #1')
            return
        raise Fn()
    SSE_SET = frozenset('addss subss mulss divss sqrtss minss maxss addsd subsd mulsd divsd sqrtsd minsd maxsd addps subps mulps divps sqrtps addpd subpd mulpd divpd sqrtpd xorps xorpd pxor andps andpd pand orps orpd por andnps andnpd pandn cvtss2sd cvtsd2ss cvtsi2sd cvtsi2ss cvtps2pd cvtpd2ps cvtdq2ps unpcklps unpckhps shufps comiss ucomiss comisd ucomisd movd movq movsd movlps movhps movlpd movhpd movhlps movlhps pslldq psrldq cmpeqps cmpltps cmpleps cmpneqps cmpnltps cmpnleps'.split())
    # ---------- condition consumers (setcc / cmovcc) ----------
    def arm_cc(s, cc):
        """test the packed flags in x28 for x86 condition cc; returns the ARM condition that is true iff cc holds"""
        cc = CCN[cc]; neg = cc in ('ne', 'ae', 'ns', 'no', 'np', 'ge', 'g', 'a')
        base = {'ne': 'e', 'ae': 'b', 'ns': 's', 'no': 'o', 'np': 'p', 'ge': 'l', 'g': 'le', 'a': 'be'}.get(cc, cc)
        if base in ('e', 'b', 's', 'o', 'p'): s.e('tst w28, #%d' % {'e': 0x40, 'b': 1, 's': 0x80, 'o': 0x800, 'p': 4}[base])
        elif base == 'be': s.e('mov w1, #65', 'tst w28, w1')
        elif base == 'l': s.e('eor w1, w28, w28, lsr #4', 'tst w1, #128')
        else: s.e('eor w1, w28, w28, lsr #4', 'orr w1, w1, w28, lsl #1', 'tst w1, #128')   # le: ZF | (SF != OF)
        return 'eq' if neg else 'ne'
    def cc_consume(s, i, arm=None):
        """setcc / cmovcc.  arm = ARM condition when the producer's NZCV is still live (compare+consumer fusion), else read the packed flags."""
        mn = i.mnemonic; ops = i.operands
        if mn.startswith('set') and len(ops) == 1 and ops[0].size == 1:
            cc = mn[3:]
            if cc not in CCN: raise Fn()
            if ops[0].type == 1 and ops[0].reg not in s.R8: raise Fn()
            c = arm or s.arm_cc(cc); s.e('cset w0, %s' % c); s.stv(ops[0], 'w0'); return
        if mn.startswith('cmov') and len(ops) == 2 and ops[0].type == 1 and ops[0].size == 4 and ops[1].size == 4 and ops[1].type in (1, 3):
            cc = mn[4:]
            if cc not in CCN: raise Fn()
            dr = W(s.reg(ops[0])); v = s.val(ops[1], 'w2')   # the load happens regardless of the condition, as on x86
            c = arm or s.arm_cc(cc); s.e('csel %s, %s, %s, %s' % (dr, v, dr, c)); return
        raise Fn()
    # ---------- locked / atomic read-modify-write (LL/SC loops; no LSE needed so it runs on every ARMv8 core) ----------
    def atomic(s, i, mn, nd, lock):
        ops = i.operands; n = len(ops); need = nd & set(FB)
        if mn == 'xchg' and n == 2 and ops[0].type == 1 and ops[1].type == 1 and ops[0].size == 4 and ops[1].size == 4:
            a = W(s.reg(ops[0])); b = W(s.reg(ops[1]))
            if a != b: s.e('mov w0, %s' % a, 'mov %s, %s' % (a, b), 'mov %s, w0' % b)
            return
        if mn in ('xadd', 'cmpxchg', 'xchg') and n == 2:
            m, r = ops
            if mn == 'xchg' and m.type != 3: m, r = r, m
            if m.type != 3 or r.type != 1 or m.size != 4 or r.size != 4: raise Fn()
            rr = W(s.reg(r)); s.addr(m, lea=True)   # guest address -> w10
            L = s.lab(); s.e('mov w2, %s' % rr)
            if mn == 'xchg':
                s.e('%s:' % L, 'ldaxr w0, [x10]', 'stlxr w4, w2, [x10]', 'cbnz w4, %s' % L, 'mov %s, w0' % rr); return
            if mn == 'xadd':
                s.e('%s:' % L, 'ldaxr w0, [x10]', 'add w3, w0, w2', 'stlxr w4, w3, [x10]', 'cbnz w4, %s' % L)
                if need: s.e('adds w5, w0, w2'); s.setf(need, 'add', 'w5')
                s.e('mov %s, w0' % rr); return
            Ld = s.lab(); De = s.lab()   # cmpxchg: eax == [m] ? [m] = r : eax = [m]; flags of cmp eax,[m]
            s.e('%s:' % L, 'ldaxr w0, [x10]', 'subs w5, w19, w0', 'b.ne %s' % Ld, 'stlxr w4, w2, [x10]', 'cbnz w4, %s' % L, 'b %s' % De, '%s:' % Ld, 'clrex', 'mov w19, w0', '%s:' % De)
            if need: s.setf(need, 'sub', 'w5')
            return
        if lock and mn in ('add', 'sub', 'and', 'or', 'xor', 'inc', 'dec') and ops and ops[0].type == 3 and ops[0].size == 4:
            if mn in ('inc', 'dec') and n == 1: s.e('mov w2, #1'); need = need - {'cf'}
            elif n == 2 and ops[1].type == 2: s.e(*mov32('w2', ops[1].imm & 0xffffffff))
            elif n == 2 and ops[1].type == 1 and ops[1].size == 4: s.e('mov w2, %s' % W(s.reg(ops[1])))
            else: raise Fn()
            s.addr(ops[0], lea=True); L = s.lab()
            op = {'add': 'add', 'inc': 'add', 'sub': 'sub', 'dec': 'sub', 'and': 'and', 'or': 'orr', 'xor': 'eor'}[mn]
            s.e('%s:' % L, 'ldaxr w0, [x10]', '%s w3, w0, w2' % op, 'stlxr w4, w3, [x10]', 'cbnz w4, %s' % L)
            if need:
                if mn in ('add', 'inc'): s.e('adds w5, w0, w2'); s.setf(need, 'add', 'w5')
                elif mn in ('sub', 'dec'): s.e('subs w5, w0, w2'); s.setf(need, 'sub', 'w5')
                else: s.e('%s w5, w0, w2' % op, 'tst w5, w5'); s.setf(need, 'logic', 'w5')
            return
        raise Fn()
    def ins2(s, i, nd, fuse=None):
        """sized moves, extensions, shifts, 8-bit compares and the rest of the common integer forms"""
        mn = i.mnemonic; ops = i.operands; n = len(ops); need = nd & set(FB)
        if mn[0] == 'f' and mn not in ('fist', 'fistp') : return s.x87(i, nd)
        if mn in s.SSE_SET: return s.sse(i, nd)
        if mn in ('movaps', 'movups', 'movdqa', 'movdqu', 'movapd', 'movupd', 'movss') and n == 2: return s.sse_move(i, ops)
        if mn == 'mov' and n == 2 and ops[0].size in (1, 2) and ops[0].size == ops[1].size:
            d, sr = ops
            if d.type == 3 and sr.type == 3: raise Fn()
            v = s.ldv(sr, 'w0'); s.stv(d, v); return
        if mn in ('movzx', 'movsx') and n == 2 and ops[0].type == 1 and ops[0].size == 4 and ops[1].size in (1, 2):
            dr = s.reg(ops[0]); v = s.ldv(ops[1], 'w0')
            if mn == 'movzx': s.e('mov %s, %s' % (W(dr), v))
            else: s.e('%s %s, %s' % ('sxtb' if ops[1].size == 1 else 'sxth', W(dr), v))
            return
        if mn in ('cmp', 'test') and n == 2 and ops[0].size in (1, 2) and ops[0].size == ops[1].size:
            d, sr = ops
            if d.type == 3 and sr.type == 3: raise Fn()
            sh = 24 if d.size == 1 else 16
            if 'pf' not in need:
                def top(op, t):   # value << sh without the separate zero-extend
                    if op.type == 2: s.e(*mov32(t, ((op.imm & ((1 << (8 * op.size)) - 1)) << sh) & 0xffffffff)); return t
                    if op.type == 1 and op.size == 1 and op.reg in s.R8 and s.R8[op.reg][1] == 0: s.e('lsl %s, %s, #24' % (t, W(s.R8[op.reg][0]))); return t
                    if op.type == 1 and op.size == 2 and op.reg in s.R16: s.e('lsl %s, %s, #16' % (t, W(s.R16[op.reg]))); return t
                    v = s.ldv(op, t); s.e('lsl %s, %s, #%d' % (t, v, sh)); return t
                same = d.type == 1 and sr.type == 1 and d.reg == sr.reg
                a = top(d, 'w4'); b = a if same else top(sr, 'w5')
                s.e('%s wzr, %s, %s' % ('subs' if mn == 'cmp' else 'ands', a, b))
                if need: s.setf(need, 'sub' if mn == 'cmp' else 'logic', 'w6')
                return
            a = s.ldv(d, 'w2'); b = s.ldv(sr, 'w3')
            s.e('lsl w4, %s, #%d' % (a, sh), 'lsl w5, %s, #%d' % (b, sh))
            s.e('%s wzr, w4, w5' % ('subs' if mn == 'cmp' else 'ands'))
            if need:
                if 'pf' in need: s.e('%s w6, %s, %s' % ('sub' if mn == 'cmp' else 'and', a, b))
                s.setf(need, 'sub' if mn == 'cmp' else 'logic', 'w6')
            return
        if mn in ('shl', 'sal', 'shr', 'sar') and n == 2 and ops[0].type == 1 and ops[0].size == 4 and not need:
            dr = s.reg(ops[0]); op = {'shl': 'lsl', 'sal': 'lsl', 'shr': 'lsr', 'sar': 'asr'}[mn]
            if ops[1].type == 2:
                c = ops[1].imm & 31
                if c: s.e('%s %s, %s, #%d' % (op, W(dr), W(dr), c))
                return
            if ops[1].type == 1 and ops[1].reg == X86_REG_CL: s.e('%s %s, %s, w20' % (op, W(dr), W(dr))); return
            raise Fn()
        if mn in ('shl', 'sal', 'shr', 'sar') and n == 2 and ops[0].type in (1, 3) and ops[0].size == 4 and (ops[1].type == 2 or (ops[1].type == 1 and ops[1].reg == X86_REG_CL and need)):
            var = ops[1].type == 1
            c = 0 if var else ops[1].imm & 31
            if not var and c == 0: return
            d = ops[0]
            if d.type == 1: dr = s.reg(d); a = W(dr)
            else: ad = s.addr(d); s.e('ldr w0, %s' % ad); a = 'w0'
            k = 'shl' if mn in ('shl', 'sal') else mn
            sop = {'shl': 'lsl', 'shr': 'lsr', 'sar': 'asr'}[k]
            if var:
                done = s.lab(); s.e('and w6, w20, #31', 'cbz w6, %s' % done)   # count 0 leaves everything (flags too) unchanged
                s.e('%s w3, %s, w6' % (sop, a))
            else: s.e('%s w3, %s, #%d' % (sop, a, c))
            if 'cf' in need or (k == 'shl' and 'of' in need):
                if var:
                    if k == 'shl': s.e('mov w7, #32', 'sub w7, w7, w6')
                    else: s.e('sub w7, w6, #1')
                    s.e('%s w4, %s, w7' % ('lsr' if k != 'sar' else 'asr', a), 'and w4, w4, #1')
                else: s.e('ubfx w4, %s, #%d, #1' % (a, 32 - c if k == 'shl' else c - 1))   # CF = last bit shifted out
                if 'cf' in need: s.e('bfi x28, x4, #0, #1')
            if 'of' in need:
                if k == 'shl': s.e('lsr w5, w3, #31', 'eor w5, w5, w4', 'bfi x28, x5, #11, #1')
                elif k == 'shr': s.e('lsr w5, %s, #31' % a, 'bfi x28, x5, #11, #1')
                else: s.e('bfi x28, xzr, #11, #1')
            if 'zf' in need: s.e('cmp w3, #0', 'cset w5, eq', 'bfi x28, x5, #6, #1')
            if 'sf' in need: s.e('lsr w5, w3, #31', 'bfi x28, x5, #7, #1')
            if 'pf' in need: s.e('eor w1, w3, w3, lsr #4', 'eor w1, w1, w1, lsr #2', 'eor w1, w1, w1, lsr #1', 'mvn w1, w1', 'bfi x28, x1, #2, #1')
            if d.type == 1: s.e('mov %s, w3' % a)
            else: s.e('str w3, %s' % ad)
            if var: s.e('%s:' % done)
            return
        # 8/16-bit add/sub/and/or/xor (flags via the operands shifted to the top of a 32-bit register)
        if mn in ('add', 'sub', 'and', 'or', 'xor') and n == 2 and ops[0].size in (1, 2) and (ops[1].size == ops[0].size or ops[1].type == 2) and not (ops[0].type == 3 and ops[1].type == 3):
            d, sr = ops; sz = d.size; sh = 32 - 8 * sz
            a = s.ldv(d, 'w2')
            if sr.type == 2: s.e(*mov32('w3', sr.imm & ((1 << (8 * sz)) - 1))); b = 'w3'
            else: b = s.ldv(sr, 'w3')
            if mn in ('and', 'or', 'xor'):
                s.e('%s w7, %s, %s' % ({'and': 'and', 'or': 'orr', 'xor': 'eor'}[mn], a, b))
                if need & {'zf', 'sf', 'pf'}: s.e('lsl w4, w7, #%d' % sh, 'tst w4, w4'); s.setf(need, 'logic', 'w7')
                elif need: s.setf(need, 'logic', 'w7')
            elif not need: s.e('%s w7, %s, %s' % (mn, a, b))
            else:
                s.e('lsl w4, %s, #%d' % (a, sh), 'lsl w5, %s, #%d' % (b, sh), '%s w6, w4, w5' % ('adds' if mn == 'add' else 'subs'), 'lsr w7, w6, #%d' % sh)
                s.setf(need, mn, 'w7')
            s.stv(d, 'w7'); return
        # adc/sbb with the result flags dead: the incoming carry is just a bit of the packed flags
        if mn in ('adc', 'sbb') and n == 2 and not need and ops[0].size in (1, 4) and (ops[1].size == ops[0].size or ops[1].type == 2) and not (ops[0].type == 3 and ops[1].type == 3):
            d, sr = ops; sz = d.size
            if sz == 4 and d.type == 1: a = W(s.reg(d))
            else: a = s.ldv(d, 'w2')
            if sr.type == 2: s.e(*mov32('w3', sr.imm & ((1 << (8 * sz)) - 1))); b = 'w3'
            else: b = s.ldv(sr, 'w3')
            s.e('and w1, w28, #1', '%s w7, %s, %s' % ('add' if mn == 'adc' else 'sub', a, b), '%s w7, w7, w1' % ('add' if mn == 'adc' else 'sub'))
            s.stv(d, 'w7'); return
        if mn == 'not' and n == 1 and ops[0].type == 1 and ops[0].size == 4: r = W(s.reg(ops[0])); s.e('mvn %s, %s' % (r, r)); return
        if mn == 'neg' and n == 1 and ops[0].type == 1 and ops[0].size == 4:
            r = W(s.reg(ops[0]))
            if not need: s.e('neg %s, %s' % (r, r)); return
            s.e('subs w0, wzr, %s' % r); s.setf(need, 'sub', 'w0'); s.e('mov %s, w0' % r); return
        if mn == 'leave' and n == 0: s.e('mov w23, w24', 'ldr w24, [x23]', 'add w23, w23, #4'); return
        if mn == 'cdq' and n == 0: s.e('asr w21, w19, #31'); return
        if mn in ('inc', 'dec') and n == 1 and ops[0].type == 3 and ops[0].size == 4:
            need = need & {'zf', 'sf', 'of', 'pf'}; a = s.addr(ops[0]); s.e('ldr w0, %s' % a)
            s.e('%s w3, w0, #1' % (('adds' if mn == 'inc' else 'subs') if need else ('add' if mn == 'inc' else 'sub')))
            if need: s.setf(need, 'add' if mn == 'inc' else 'sub', 'w3')
            s.e('str w3, %s' % a); return
        if mn == 'imul' and not need and n in (2, 3) and ops[0].type == 1 and ops[0].size == 4:
            dr = s.reg(ops[0])
            if n == 2: a = W(dr); b = s.ldv(ops[1], 'w2')
            elif ops[2].type == 2: a = s.ldv(ops[1], 'w2'); s.e(*mov32('w3', ops[2].imm)); b = 'w3'
            else: raise Fn()
            if ops[ops.__len__() - 1].type == 2 and n == 2: raise Fn()
            s.e('mul %s, %s, %s' % (W(dr), a, b)); return
        if mn in ('shl', 'sal', 'shr', 'sar') and n == 2 and ops[0].type == 1 and ops[0].size == 4 and need and ops[1].type == 2:
            c = ops[1].imm & 31
            if c == 0: return
            dr = W(s.reg(ops[0])); op = {'shl': 'lsl', 'sal': 'lsl', 'shr': 'lsr', 'sar': 'asr'}[mn]
            s.e('mov w2, %s' % dr, '%s %s, %s, #%d' % (op, dr, dr, c))
            sub = need & {'zf', 'sf', 'pf'}
            if sub: s.e('tst %s, %s' % (dr, dr)); s.setf(sub, 'logic', dr)
            if need & {'cf', 'of'}:
                s.e('ubfx w1, w2, #%d, #1' % ((32 - c) if mn in ('shl', 'sal') else (c - 1)))
                if 'cf' in need: s.e('bfi x28, x1, #0, #1')
                if 'of' in need:
                    if mn in ('shl', 'sal'): s.e('eor w1, w1, %s, lsr #31' % dr)
                    elif mn == 'shr': s.e('lsr w1, w2, #31')
                    else: s.e('mov w1, #0')
                    s.e('bfi x28, x1, #11, #1')
            return
        if mn == 'imul' and need and n in (2, 3) and ops[0].type == 1 and ops[0].size == 4:
            dr = s.reg(ops[0])
            if n == 2: a = W(dr); b = s.ldv(ops[1], 'w2')
            elif ops[2].type == 2: a = s.ldv(ops[1], 'w2'); s.e(*mov32('w3', ops[2].imm & 0xffffffff)); b = 'w3'
            else: raise Fn()
            s.e('smull x4, %s, %s' % (a, b), 'mov w0, w4', 'cmp x4, w4, sxtw', 'cset w6, ne')   # CF = OF = the 64-bit product does not fit in 32 bits signed
            sub = need & {'zf', 'sf', 'pf'}
            if sub: s.e('tst w0, w0'); s.setf(sub, 'logic', 'w0')
            if 'cf' in need: s.e('bfi x28, x6, #0, #1')
            if 'of' in need: s.e('bfi x28, x6, #11, #1')
            s.e('mov %s, w0' % W(dr)); return
        if mn == 'lahf' and n == 0: s.e('mov w0, #213', 'and w0, w28, w0', 'orr w0, w0, #2', 'bfi x19, x0, #8, #8'); return
        if mn == 'sahf' and n == 0:
            s.e('ubfx w0, w19, #8, #8', 'bfi x28, x0, #0, #1', 'ubfx w1, w0, #2, #1', 'bfi x28, x1, #2, #1', 'ubfx w1, w0, #6, #1', 'bfi x28, x1, #6, #1', 'ubfx w1, w0, #7, #1', 'bfi x28, x1, #7, #1'); return
        raise Fn()
    def alu(s, mn, ops, nd, fuse):
        d, sr = ops; need = nd & set(FB)
        if d.type == 3 and sr.type == 3: raise Fn()
        if d.type == 1: dr = s.reg(d); dn = W(dr)
        elif d.type == 3: dr = None; dn = None
        else: raise Fn()
        def aimm(v):   # immediate operand text usable directly by the ARM instruction, else None
            if mn in ('add', 'sub', 'cmp'):
                if 0 <= v < 4096: return '#%d' % v
                if v & 0xfff == 0 and v < (1 << 24): return '#%d, lsl #12' % (v >> 12)
                return None
            return '#0x%x' % v if logic_imm(v) else None
        # source first (it may use w0/w1 as scratch), then the destination memory operand's address in x10
        if d.type == 3:
            if d.size != 4: raise Fn()
            sv = (aimm(sr.imm & 0xffffffff) if sr.type == 2 else None) or (s.val(sr, 'w2') if sr.type != 3 else None)
            if sv is None: raise Fn()
            a = s.addr(d); s.e('ldr w0, %s' % a); dv = 'w0'
        else:
            sv = (aimm(sr.imm & 0xffffffff) if sr.type == 2 else None) or s.val(sr, 'w2'); dv = dn
        op = {'add': 'adds', 'sub': 'subs', 'cmp': 'subs', 'and': 'ands', 'or': 'orr', 'xor': 'eor', 'test': 'ands'}[mn]
        kind = {'add': 'add', 'sub': 'sub', 'cmp': 'sub', 'and': 'logic', 'or': 'logic', 'xor': 'logic', 'test': 'logic'}[mn]
        res = 'wzr' if mn in ('cmp', 'test') else ('w3' if d.type == 3 else dn)
        if mn in ('or', 'xor'):
            if need & {'zf', 'sf', 'pf'} or fuse is not None:   # orr/eor do not set NZCV: redo the result as ands-with-self (tst)
                s.e('%s %s, %s, %s' % (op, 'w3' if d.type == 3 else dn, dv, sv)); r = 'w3' if d.type == 3 else dn
                s.e('tst %s, %s' % (r, r)); res = r
            else:
                s.e('%s %s, %s, %s' % (op, 'w3' if d.type == 3 else dn, dv, sv)); res = 'w3' if d.type == 3 else dn; need = set()
                if nd & {'cf', 'of'}: need = nd & {'cf', 'of'}
        else:
            if need or fuse is not None or mn in ('cmp', 'test'):
                s.e('%s %s, %s, %s' % (op, res if res != 'wzr' else 'wzr', dv, sv))
            else: s.e('%s %s, %s, %s' % ({'adds': 'add', 'subs': 'sub', 'ands': 'and'}[op], res, dv, sv))
        if need:
            if mn in ('cmp', 'test') or res == 'wzr': rr = 'w3'; 
            else: rr = res
            if 'pf' in need and rr == 'w3' and mn in ('cmp', 'test'):
                # parity of the discarded result
                s.e('%s w3, %s, %s' % ({'cmp': 'sub', 'test': 'and'}[mn], dv, sv))
            s.setf(need, kind, rr)
        if d.type == 3 and mn not in ('cmp', 'test'):
            s.e('str w3, %s' % a)

def logic_imm(v):
    """True when the 32-bit value is encodable as an AArch64 logical (bitmask) immediate"""
    if v in (0, 0xffffffff): return False
    for e in (2, 4, 8, 16, 32):
        m = (1 << e) - 1; el = v & m
        if all(((v >> k) & m) == el for k in range(0, 32, e)):
            bits = [(el >> k) & 1 for k in range(e)]
            return 0 < el < m and sum(1 for k in range(e) if bits[k] == 0 and bits[(k + 1) % e] == 1) == 1
    return False

# ======================= block terminators + program builder =======================
def disp_site(o, sh, mulc, tag, call=None):
    """Per-site dispatch of the guest pc in w9: first probe inline (its own indirect branch), collisions/misses go to the shared slow path.
    call = None: plain jump.  call = [lines]: indirect call whose shadow-stack entry was already pushed (its host return address is .Lc_<tag>)."""
    o.extend(mov32('w10', mulc) + ['mul w10, w9, w10', 'lsr w10, w10, #%d' % sh, 'adrp x11, aot_a64_tab', 'add x10, x11, x10, lsl #4', 'ldr w11, [x10]', 'cmp w11, w9', 'b.ne .Ls_%s' % tag, 'ldr x12, [x10, #8]'])
    if call is None: o.extend(['br x12', '.Ls_%s:' % tag, 'b aot_a64_slow'])
    else: o.extend(['blr x12', '.Lc_%s:' % tag] + call + ['.Ls_%s:' % tag, 'bl aot_a64_slowc', 'b .Lc_%s' % tag])   # call = the lines that continue at the return address

def push_shadow(o, nx, tag):
    """guest call: push the return address on the guest stack and a (guest ret, host ret) pair on the shadow ring (x29, 8KB, 16-byte entries, wraps by clearing bit 13)"""
    o.extend(mov32('w0', nx) + ['adr x1, .Lc_%s' % tag, 'stp x0, x1, [x29], #16', 'bic x29, x29, #0x2000', 'str w0, [x23, #-4]!'])

def pop_shadow(o, tag):
    """w9 = guest return address (already popped from the guest stack): return through the shadow ring so the CPU's return-address predictor sees a matched bl/ret pair"""
    o.extend(['sub x10, x29, #16', 'bfi x29, x10, #0, #13', 'ldp x0, x1, [x29]', 'cmp w0, w9', 'b.ne .Lrm_%s' % tag, 'ret x1', '.Lrm_%s:' % tag, 'b aot_a64_retmiss'])

CCN = {'e':'e','z':'e','ne':'ne','nz':'ne','b':'b','c':'b','nae':'b','ae':'ae','nb':'ae','nc':'ae','be':'be','na':'be','a':'a','nbe':'a','s':'s','ns':'ns','o':'o','no':'no','p':'p','pe':'p','np':'np','po':'np','l':'l','nge':'l','ge':'ge','nl':'ge','le':'le','ng':'le','g':'g','nle':'g'}
HIST = {}
HIST2 = {}
class Prog:
    """Builds the whole-program assembly text for a list of regions (each a list of basic blocks of capstone insns)."""
    def __init__(s, g, regions, live_out, flagrw, F5, cfun, special=None):
        s.g = g; s.regions = regions; s.live_out = live_out; s.flagrw = flagrw; s.F5 = F5; s.cfun = cfun; s.special = special or {}
        s.leaders = sorted(b[0].address for r in regions for b in r)
        s.bits, s.sh = hashinfo(len(s.leaders)); s.mulc = 0x9E3779B1
        s.native = 0; s.fb = 0; s.lset = set(s.leaders); s.natblk = {}; s.elided = lambda a: False
    def lbl(s, a): return 'L_%x' % a
    def goto(s, o, a):   # direct transfer to guest address a
        if a in s.lset: o.append('b %s' % s.lbl(a))
        else: o.extend(mov32('w9', a) + ['b aot_a64_exit'])
    GPR = {}
    for _k, _ns in enumerate((('eax', 'ax', 'al', 'ah'), ('ecx', 'cx', 'cl', 'ch'), ('edx', 'dx', 'dl', 'dh'), ('ebx', 'bx', 'bl', 'bh'), ('esp', 'sp'), ('ebp', 'bp'), ('esi', 'si', 'sil'), ('edi', 'di', 'dil'))):
        for _n in _ns: GPR[_n] = _k
    def gpr_sets(s, i):
        """(read, written) sets of guest GPR numbers for x87/SSE/MMX instructions (their C function touches no other integer state); None = use the full exchange"""
        mn = i.mnemonic.split()[-1]
        if not (mn[0] == 'f' or mn in ('emms',) or any(i.reg_name(o.reg).startswith(('xmm', 'mm', 'st')) for o in i.operands if o.type == 1)): return None
        try: rd, wr = i.regs_access()
        except Exception: return None
        R = set(); W_ = set()
        for lst, dst in ((rd, R), (wr, W_)):
            for r in lst:
                nm = i.reg_name(r)
                if nm in s.GPR: dst.add(s.GPR[nm])
                elif nm in ('eflags', 'fpsw', 'fpcw', 'mxcsr', 'rip', 'eip', 'cs', 'ds', 'es', 'ss', 'fs', 'gs') or nm.startswith(('xmm', 'mm', 'st', 'fp')): pass
                else: return None
        return R | W_, W_
    def fallback(s, o, i, nd, ctl=False):
        w, r, var = s.flagrw(i); s.fb += 1
        gs = None if ctl else s.gpr_sets(i)
        if gs is not None:
            sp, ld = sorted(gs[0]), sorted(gs[1])
            o.extend(['str x%d, [x27, #%d]' % (19 + k, 8 * k) for k in sp])
            if r or w or var: o.append('bl aot_a64_fl_store')
            o.extend(['mov x0, x27'] + mov32('w1', i.address) + ['bl %s' % s.cfun(i, nd), 'mov w9, w0'])
            o.extend(['ldr w%d, [x27, #%d]' % (19 + k, 8 * k) for k in ld])
            if w or var: o.append('bl aot_a64_fl_load')
            return
        HIST[i.mnemonic.split()[-1]] = HIST.get(i.mnemonic.split()[-1], 0) + 1
        if i.mnemonic in ('mov', 'shl', 'add', 'sbb', 'shr', 'and', 'movsd'): k2 = i.mnemonic + ' ' + ','.join('%d/%d%s' % (o.type, o.size, ('s' if o.type == 3 and o.mem.segment else '')) for o in i.operands) + ' fl=' + ','.join(sorted(nd)); HIST2[k2] = HIST2.get(k2, 0) + 1
        o.extend(['stp x19, x20, [x27]', 'stp x21, x22, [x27, #16]', 'stp x23, x24, [x27, #32]', 'stp x25, x26, [x27, #48]'])
        if r or w or var or ctl: o.append('bl aot_a64_fl_store')
        o.extend(['mov x0, x27'] + mov32('w1', i.address) + ['bl %s' % s.cfun(i, nd), 'mov w9, w0'])
        o.extend(['ldr w19, [x27]', 'ldr w20, [x27, #8]', 'ldr w21, [x27, #16]', 'ldr w22, [x27, #24]', 'ldr w23, [x27, #32]', 'ldr w24, [x27, #40]', 'ldr w25, [x27, #48]', 'ldr w26, [x27, #56]'])
        if w or var or ctl: o.append('bl aot_a64_fl_load')
        if ctl: o.extend(mov32('w10', i.address + i.size) + ['cmp w9, w10', 'b.ne .Lfs%x' % i.address])
    def fb_ctl(s, o, i, nd):
        """control-flow instruction executed by its C function: it returns the next pc; the expected fall-through continues inline, anything else is dispatched"""
        s.fallback(o, i, nd, True); s.goto(o, i.address + i.size); o.extend(['.Lfs%x:' % i.address, 'b aot_a64_slow'])
    def cond_branch(s, o, i, tgt, fall, fused=None):
        cc = CC[i.mnemonic[1:]]
        o.append('b.%s %s' % (cc, s.lbl(tgt)) if tgt in s.lset else 'b.%s %s_x%x' % (cc, 'aot_a64', tgt))
    def term_cond(s, o, cc_kind):   # evaluate x86 condition `cc` from the packed flags into the Z flag convention: returns (arm cond to branch on)
        pass
    CTL = ('call', 'jmp', 'ret', 'ljmp', 'retf', 'iret', 'iretd', 'loop', 'loope', 'loopne', 'jecxz', 'jcxz', 'int', 'int3', 'ud2', 'hlt', 'lcall')
    def fuse_kind(s, prev, cc):
        """ARM condition for x86 condition cc evaluated straight after `prev` (an ALU op executed in flag-setting form), or None"""
        mn = prev.mnemonic
        if mn not in ('cmp', 'sub', 'test', 'and', 'add', 'or', 'xor') or len(prev.operands) != 2 or (prev.operands[0].size != 4 and not (mn in ('cmp', 'test') and prev.operands[0].size in (1, 2) and prev.operands[0].size == prev.operands[1].size)): return None
        if prev.operands[0].type == 3 and mn not in ('cmp', 'test'): return None
        if mn in ('cmp', 'sub'): return CC.get(cc)
        if mn == 'add': return {'e': 'eq', 'ne': 'ne', 's': 'mi', 'ns': 'pl', 'o': 'vs', 'no': 'vc', 'l': 'lt', 'ge': 'ge', 'le': 'le', 'g': 'gt', 'b': 'hs', 'ae': 'lo'}.get(cc)
        return {'e': 'eq', 'ne': 'ne', 's': 'mi', 'ns': 'pl', 'l': 'lt', 'ge': 'ge', 'le': 'le', 'g': 'gt'}.get(cc)   # logic: CF=OF=0, ARM C/V are cleared too
    def block(s, b, k, idx_next):
        o = []; i0 = b[0]; live = set(s.live_out[i0.address]); needs = []; lives = []
        for i in reversed(b):
            w, r, var = s.flagrw(i); lives.append(set(live))   # flags live after i
            if var: needs.append(set(s.F5)); live |= r; continue
            needs.append(live & w); live = (live - w) | r
        needs.reverse(); lives.reverse()
        o.extend(['.globl %s' % s.lbl(i0.address), '.hidden %s' % s.lbl(i0.address), '%s:' % s.lbl(i0.address)])
        nat = s.natblk.get(i0.address)
        if nat: s.fb_ctl(o, i0, set(s.F5)); return o
        B = Blk(None, None, s.sh, '%x' % i0.address)
        last = b[-1]; lm = last.mnemonic.split()[-1]
        elided = lm == 'call' and last.operands and last.operands[0].type == 2 and s.elided(last.operands[0].imm)
        ends = (lm in s.CTL or lm.startswith('j')) and not elided
        body = b[:-1] if ends else b
        jcc = lm.startswith('j') and (lm[1:] in CC or lm[1:] in ('p', 'np', 'pe', 'po')) and last.operands[0].type == 2
        # producer/consumer fusion: an ALU op directly followed by setcc / cmovcc / the block's own jcc keeps its result in NZCV (no packed-flag materialisation)
        # when nothing after the last consumer reads what the producer writes.  fprod: producer idx -> consumer idxs, fcons: consumer idx -> ARM condition
        fprod = {}; fcons = {}; T = Blk(None, None, s.sh, 'plan')
        for p in range(len(b) - 1):
            prev = b[p]
            if prev.mnemonic.split()[-1] not in ('cmp', 'sub', 'test', 'and', 'add', 'or', 'xor') or prev.address in s.special: continue
            w, r, var = s.flagrw(prev)
            if var: continue
            arms = {}; q = p + 1
            while q < len(b):
                c = b[q]; cm = c.mnemonic; isj = jcc and c is last
                if isj: cc = lm[1:]
                elif c.address in s.special: break
                elif cm.startswith('set'): cc = cm[3:]
                elif cm.startswith('cmov'): cc = cm[4:]
                else: break
                cc = CCN.get(cc); arm = s.fuse_kind(prev, cc) if cc else None
                if not arm: break
                arms[q] = arm; q += 1
                if isj: break
            if not arms or (set(lives[max(arms)]) & w): continue
            try:
                for q2, a2 in arms.items():
                    if not (jcc and b[q2] is last): T.o = []; T.ins(b[q2], set(), arm=a2)   # every consumer must emit natively: a C fallback would read x28
            except Fn: continue
            fprod[p] = list(arms); fcons.update(arms)
        skip = set()
        def stack_pair(idx):
            if idx + 1 >= len(b): return None
            i, j = b[idx], b[idx + 1]
            if i.mnemonic != j.mnemonic or i.mnemonic not in ('push', 'pop') or i.address in s.special or j.address in s.special: return None
            if (ends or elided) and j is last: return None
            try:
                if i.mnemonic == 'push' and all(len(x.operands) == 1 and x.operands[0].type in (1, 2) and x.operands[0].size in (4,) for x in (i, j)):
                    pre = []; names = []
                    for x, t in ((i, 'w0'), (j, 'w1')):
                        op = x.operands[0]
                        if op.type == 2:
                            v = op.imm & 0xffffffff
                            if v == 0: names.append('wzr')
                            else: pre.extend(mov32(t, v)); names.append(t)
                        else:
                            r = B.reg(op)
                            if r == 4: return None
                            names.append(W(r))
                    return pre + ['stp %s, %s, [x23, #-8]!' % (names[1], names[0])]   # first push lands at the higher address
                if any(len(x.operands) != 1 or x.operands[0].type != 1 or x.operands[0].size != 4 for x in (i, j)): return None
                ra, rb = B.reg(i.operands[0]), B.reg(j.operands[0])
            except Fn: return None
            if 4 in (ra, rb) or (ra == rb and i.mnemonic == 'pop'): return None
            if i.mnemonic == 'push': return ['stp %s, %s, [x23, #-8]!' % (W(rb), W(ra))]
            return ['ldp %s, %s, [x23], #8' % (W(ra), W(rb))]
        for idx, (i, nd) in enumerate(zip(b, needs)):
            if ends and i is last: break
            if elided and i is last: break
            if idx in skip: continue
            if idx not in fprod and idx not in fcons:
                pr = stack_pair(idx)
                if pr: o.extend(pr); skip.add(idx + 1); s.native += 2; continue
            if idx in fprod:
                try: B.o = []; B.ins(i, set(), fuse=True); o.extend(B.o); s.native += 1; continue
                except Fn:
                    B.ftv = False; B.gb = None
                    for q in fprod[idx]: fcons.pop(q, None)
            elif idx in fcons:
                B.o = []; B.ins(i, set(), arm=fcons[idx]); o.extend(B.o); s.native += 1; continue
            try:
                if i.address in s.special: raise Fn()
                B.o = []; B.ins(i, nd); o.extend(B.o); s.native += 1
            except Fn:
                B.ftv = False; B.gb = None
                ctl = i.mnemonic.split()[-1] in s.CTL or i.mnemonic.startswith('j')
                if ctl: s.fb_ctl(o, i, nd); return o
                s.fallback(o, i, nd, False); B.ftv = False; B.gb = None
        if not ends:
            s.goto(o, last.address + last.size); return o
        nd = needs[-1]; nx = last.address + last.size; op0 = last.operands[0] if last.operands else None
        if lm == 'jmp' and op0 is not None and op0.type == 2:
            s.goto(o, op0.imm & 0xffffffff); return o
        if jcc:
            tgt = op0.imm & 0xffffffff; cc = lm[1:]; lt = '.Lj%x' % last.address
            if (len(b) - 1) in fcons: o.append('b.%s %s' % (fcons[len(b) - 1], lt))
            else: s.cond_from_flags(o, cc, lt)
            s.goto(o, nx); o.append('%s:' % lt); s.goto(o, tgt); return o
        def target_into_w9(op):   # value of an indirect branch operand -> w9 (may use w10/w11); None when not expressible natively
            try:
                if op.type == 1: o.append('mov w9, %s' % W(B.reg(op))); return True
                if op.type == 3 and op.size == 4: o.append('ldr w9, %s' % B.addr_into(o, op)); return True
            except Fn: return False
            return False
        if lm == 'call' and op0 is not None and op0.type == 2:
            tgt = op0.imm & 0xffffffff
            if tgt in s.lset:
                tg = 'd%x' % last.address; push_shadow(o, nx, tg); o.extend(['bl %s' % s.lbl(tgt), '.Lc_%s:' % tg]); s.goto(o, nx); return o
            o.extend(mov32('w0', nx) + ['str w0, [x23, #-4]!']); s.goto(o, tgt); return o
        if lm in ('call', 'jmp') and op0 is not None and op0.type in (1, 3) and op0.size == 4 and op0.type == 3 and not op0.mem.segment or lm in ('call', 'jmp') and op0 is not None and op0.type == 1 and op0.size == 4:
            B.o = []
            try:
                if op0.type == 1: B.o.append('mov w9, %s' % W(B.reg(op0)))
                else: a = B.addr(op0); B.o.append('ldr w9, %s' % a)
                o.extend(B.o)
                if lm == 'call':
                    tg = 'i%x' % last.address; push_shadow(o, nx, tg); c = []; s.goto(c, nx); disp_site(o, s.sh, s.mulc, tg, c); return o
                disp_site(o, s.sh, s.mulc, 'i%x' % last.address); return o
            except Fn: pass
        if lm == 'ret' and not last.operands:
            o.append('ldr w9, [x23], #4'); pop_shadow(o, 'r%x' % last.address); return o
        if lm == 'ret' and op0 is not None and op0.type == 2:
            o.extend(['ldr w9, [x23]'] + (['add w23, w23, #%d' % (4 + op0.imm)] if 4 + op0.imm < 4096 else mov32('w0', 4 + op0.imm) + ['add w23, w23, w0'])); disp_site(o, s.sh, s.mulc, 'r%x' % last.address); return o
        s.fb_ctl(o, last, nd); return o
    def cond_from_flags(s, o, cc, lbl):
        """branch to lbl when x86 condition cc holds, reading the packed flags in x28"""
        t = {'e': ('z', 6), 'z': ('z', 6), 'ne': ('nz', 6), 'nz': ('nz', 6), 'b': ('z', 0), 'c': ('z', 0), 'nae': ('z', 0), 'ae': ('nz', 0), 'nb': ('nz', 0), 'nc': ('nz', 0),
             's': ('z', 7), 'ns': ('nz', 7), 'o': ('z', 11), 'no': ('nz', 11), 'p': ('z', 2), 'pe': ('z', 2), 'np': ('nz', 2), 'po': ('nz', 2)}
        if cc in t:
            kind, bit = t[cc]; o.append('%s w28, #%d, %s' % ('tbnz' if kind == 'z' else 'tbz', bit, lbl)); return
        if cc in ('be', 'na'): o.extend(['mov w0, #0x41', 'tst w28, w0', 'b.ne %s' % lbl]); return
        if cc in ('a', 'nbe'): o.extend(['mov w0, #0x41', 'tst w28, w0', 'b.eq %s' % lbl]); return
        o.extend(['ubfx w0, w28, #7, #1', 'ubfx w1, w28, #11, #1', 'cmp w0, w1'])
        if cc in ('l', 'nge'): o.append('b.ne %s' % lbl)
        elif cc in ('ge', 'nl'): o.append('b.eq %s' % lbl)
        elif cc in ('le', 'ng'): o.extend(['b.ne %s' % lbl, 'tbnz w28, #6, %s' % lbl])
        elif cc in ('g', 'nle'): o.extend(['b.ne .Lg_%s_skip' % lbl[2:], 'tbz w28, #6, %s' % lbl, '.Lg_%s_skip:' % lbl[2:]])
        else: raise Fn()

def core_asm(prog, nregions):
    """Entry shim, shared exit, flag marshalling, slow dispatch and the dispatch table (everything every region shares)."""
    OFF = 760   # offsetof(CPU, cf); cf zf sf of pf are consecutive bytes (checked by a _Static_assert in aot_runtime.h)
    n = 1 << prog.bits; mask = n - 1; sh = prog.sh
    place = [None] * n
    for a in prog.leaders:
        h = ((a * prog.mulc) & 0xffffffff) >> sh
        while place[h] is not None: h = (h + 1) & mask
        place[h] = a
    o = ['.text', '.balign 16', '.globl aot_a64_enter', '.hidden aot_a64_enter', 'aot_a64_enter:',
         'stp x29, x30, [sp, #-96]!', 'stp x19, x20, [sp, #16]', 'stp x21, x22, [sp, #32]', 'stp x23, x24, [sp, #48]', 'stp x25, x26, [sp, #64]', 'stp x27, x28, [sp, #80]',
         'mov x27, x0', 'mov w9, w1',
         # shadow return ring: 8KB at a 16KB-aligned base below the frame, old sp kept just above it; entry 0 is a sentinel (guest 0 -> aot_a64_slow)
         'mov x10, sp', 'sub x10, x10, #0x2000', 'sub x10, x10, #64', 'and x10, x10, #0xffffffffffffc000', 'mov x11, sp', 'str x11, [x10, #0x2000]', 'adr x12, aot_a64_slow', 'stp xzr, x12, [x10]',
         'add x29, x10, #16', 'sub sp, x10, #64',
         'ldr w19, [x27]', 'ldr w20, [x27, #8]', 'ldr w21, [x27, #16]', 'ldr w22, [x27, #24]', 'ldr w23, [x27, #32]', 'ldr w24, [x27, #40]', 'ldr w25, [x27, #48]', 'ldr w26, [x27, #56]', 'bl aot_a64_fl_load', 'b aot_a64_slow',
         '.globl aot_a64_exit', '.hidden aot_a64_exit', 'aot_a64_exit:',
         'stp x19, x20, [x27]', 'stp x21, x22, [x27, #16]', 'stp x23, x24, [x27, #32]', 'stp x25, x26, [x27, #48]', 'bl aot_a64_fl_store', 'mov w0, w9',
         'and x10, x29, #0xffffffffffffc000', 'ldr x10, [x10, #0x2000]', 'mov sp, x10',
         'ldp x27, x28, [sp, #80]', 'ldp x25, x26, [sp, #64]', 'ldp x23, x24, [sp, #48]', 'ldp x21, x22, [sp, #32]', 'ldp x19, x20, [sp, #16]', 'ldp x29, x30, [sp], #96', 'ret',
         '.globl aot_a64_fl_store', '.hidden aot_a64_fl_store', 'aot_a64_fl_store:',
         'and w0, w28, #1', 'strb w0, [x27, #%d]' % OFF, 'ubfx w0, w28, #6, #1', 'strb w0, [x27, #%d]' % (OFF + 1), 'ubfx w0, w28, #7, #1', 'strb w0, [x27, #%d]' % (OFF + 2),
         'ubfx w0, w28, #11, #1', 'strb w0, [x27, #%d]' % (OFF + 3), 'ubfx w0, w28, #2, #1', 'strb w0, [x27, #%d]' % (OFF + 4), 'ret',
         '.globl aot_a64_fl_load', '.hidden aot_a64_fl_load', 'aot_a64_fl_load:',
         'ldrb w28, [x27, #%d]' % OFF, 'ldrb w0, [x27, #%d]' % (OFF + 1), 'ldrb w1, [x27, #%d]' % (OFF + 2), 'ldrb w2, [x27, #%d]' % (OFF + 3), 'ldrb w3, [x27, #%d]' % (OFF + 4),
         'bfi w28, w0, #6, #1', 'bfi w28, w1, #7, #1', 'bfi w28, w2, #11, #1', 'bfi w28, w3, #2, #1', 'ret',
         '.globl aot_a64_slow', '.hidden aot_a64_slow', 'aot_a64_slow:']
    o += mov32('w10', prog.mulc) + ['mul w10, w9, w10', 'lsr w10, w10, #%d' % sh, 'adrp x13, aot_a64_tab',
         '.Lsl_loop:', 'add x11, x13, x10, lsl #4', 'ldr w12, [x11]', 'cbz w12, .Lsl_miss', 'cmp w12, w9', 'b.eq .Lsl_hit', 'add w10, w10, #1', 'and w10, w10, #%d' % mask, 'b .Lsl_loop',
         '.Lsl_hit:', 'ldr x12, [x11, #8]', 'br x12',
         '.Lsl_miss:', 'str x9, [sp, #-16]!', 'stp x19, x20, [x27]', 'stp x21, x22, [x27, #16]', 'stp x23, x24, [x27, #32]', 'stp x25, x26, [x27, #48]',
         'mov x0, x27', 'mov w1, w9', 'bl aot_a64_imp_c',
         'ldr w19, [x27]', 'ldr w20, [x27, #8]', 'ldr w21, [x27, #16]', 'ldr w22, [x27, #24]', 'ldr w23, [x27, #32]', 'ldr w24, [x27, #40]', 'ldr w25, [x27, #48]', 'ldr w26, [x27, #56]',
         'ldr x9, [sp], #16', 'cbz w0, aot_a64_exit', 'mov w9, w0',
         # an imported/untranslated callee was run by the host and "returned" to w9: drop its call's shadow entry (the top one, if it is for exactly this return address)
         'sub x10, x29, #16', 'mov x11, x29', 'bfi x11, x10, #0, #13', 'ldr w12, [x11]', 'cmp w12, w9', 'csel x29, x11, x29, eq', 'b aot_a64_slow',
         # indirect call that missed the inline probe: x30 (after the bl at the call site) becomes the host return address of the shadow entry just pushed
         '.globl aot_a64_slowc', '.hidden aot_a64_slowc', 'aot_a64_slowc:', 'sub x11, x29, #8', 'mov x10, x29', 'bfi x10, x11, #0, #13', 'str x30, [x10]', 'b aot_a64_slow',
         # ret whose shadow entry did not match (non-local return, longjmp, stale entries): look a few entries down, never past the ring base; else plain dispatch
         '.globl aot_a64_retmiss', '.hidden aot_a64_retmiss', 'aot_a64_retmiss:', 'tst x29, #0x1fff', 'b.ne .Lrm_go', 'add x29, x29, #16', 'b aot_a64_slow',
         '.Lrm_go:', 'mov x12, x29', 'mov w10, #32',
         '.Lrm_l:', 'sub x11, x29, #16', 'bfi x29, x11, #0, #13', 'ldp x0, x1, [x29]', 'cbz w0, .Lrm_f', 'cmp w0, w9', 'b.eq .Lrm_h', 'tst x29, #0x1fff', 'b.eq .Lrm_f', 'subs w10, w10, #1', 'b.ne .Lrm_l',
         '.Lrm_f:', 'mov x29, x12', 'b aot_a64_slow', '.Lrm_h:', 'ret x1']
    for k in range(nregions): o += ['.globl region_%d' % k, '.hidden region_%d' % k, '.type region_%d, %%function' % k, 'region_%d:' % k, 'b aot_a64_enter']
    o += ['.section .data.rel.ro,"aw",@progbits', '.balign 4096', '.globl aot_a64_tab', '.hidden aot_a64_tab', 'aot_a64_tab:']
    for a in place:
        if a is None: o += ['.word 0, 0', '.quad 0']
        else: o += ['.word 0x%x, 0' % a, '.quad L_%x' % a]
    o += ['.globl aot_a64_tab_n', '.hidden aot_a64_tab_n', 'aot_a64_tab_n:', '.quad %d' % n, '.text']
    return o

def to_c(lines):
    """wrap assembly lines as a C translation unit made of one top-level asm statement"""
    out = ['__asm__(']
    for l in lines: out.append('"%s\\n"' % l.replace('\\', '\\\\').replace('"', '\\"'))
    out.append(');\n')
    return '\n'.join(out)


_INV = {'eq': 'ne', 'ne': 'eq', 'cs': 'cc', 'cc': 'cs', 'hs': 'lo', 'lo': 'hs', 'mi': 'pl', 'pl': 'mi', 'vs': 'vc', 'vc': 'vs', 'hi': 'ls', 'ls': 'hi', 'ge': 'lt', 'lt': 'ge', 'gt': 'le', 'le': 'gt'}
def peep(lines):
    """`bcc .Lj; b L_next; .Lj: b L_tgt; [labels] L_next:` -> `b!cc L_next; b L_tgt; [labels] L_next:` (one instruction less, and the fall-through path takes one branch instead of two)"""
    out = []; i = 0; n = len(lines)
    while i < n:
        l = lines[i]
        if i + 4 < n and l.startswith(('b.', 'tbz ', 'tbnz ', 'cbz ', 'cbnz ')) and l.endswith(' .Lj' + l.rsplit('.Lj', 1)[-1]) and '.Lj' in l:
            tgt = l.rsplit(' ', 1)[1]; nxt = lines[i + 1]
            if nxt.startswith('b L_') and not any(tgt in x for x in lines[max(0, i - 6):i]) and lines[i + 2] == tgt + ':' and lines[i + 3].startswith('b ') and (nxt[2:] + ':') in lines[i + 4:i + 7]:
                if l.startswith('b.'): c, t = l[2:].split(' ', 1); inv = 'b.%s %s' % (_INV[c], nxt[2:]) if c in _INV else None
                else: inv = {'tbz': 'tbnz', 'tbnz': 'tbz', 'cbz': 'cbnz', 'cbnz': 'cbz'}[l.split(' ', 1)[0]] + ' ' + l.split(' ', 1)[1].rsplit(' ', 1)[0] + ' ' + nxt[2:]
                if inv: out.extend([inv, lines[i + 3]]); i += 4; continue
        out.append(l); i += 1
    return out
