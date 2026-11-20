"""ARM64 assembly backend (opt-in).  x86 eax..edi live in x19..x26 for the whole program, x27 = CPU*, x28 = packed EFLAGS bits
(CF 0, PF 2, ZF 6, SF 7, OF 11), x29 = dispatch table.  Every asm region shares one host frame, so a jump between regions is a plain `b`.
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
        s.o = []; s.fallback = fallback; s.haslabel = haslabel; s.sh = sh; s.uid = uid; s.nl = 0
    def e(s, *l): s.o.extend(l)
    def lab(s): s.nl += 1; return '.Lt%s_%d' % (s.uid, s.nl)
    # ---------- operands ----------
    def reg(s, op): 
        if op.type == 1 and op.reg in X and op.size == 4: return X[op.reg]
        raise Fn()
    def addr(s, op, lea=False, al=4):   # -> (base_reg_str, imm_or_None) leaving the address in a register; may emit code.  Returns operand text for [..]
        m = op.mem
        if m.segment: raise Fn()
        if m.base and m.base not in X or m.index and m.index not in X: raise Fn()
        b = XR(X[m.base]) if m.base else None; ix = X[m.index] if m.index else None; d = m.disp & 0xffffffff
        if d >= 0x80000000: d -= 1 << 32
        if not lea and ix is None and b is not None and (-256 <= d <= 255 or (0 <= d <= 4095 * al and d % al == 0)):
            return '[%s, #%d]' % (b, d)
        if b is None and ix is None: s.e(*mov32('w10', d)); return '[x10]'
        if m.scale not in (1, 2, 4, 8): raise Fn()
        sh = {1: 0, 2: 1, 4: 2, 8: 3}[m.scale]
        if b is None: s.e('lsl w10, %s, #%d' % (W(ix), sh))
        elif ix is None: s.e('mov w10, %s' % W(X[m.base]))
        else: s.e('add w10, %s, %s, lsl #%d' % (W(X[m.base]), W(ix), sh))
        if d:
            if 0 < d < 4096: s.e('add w10, w10, #%d' % d)
            elif -4096 < d < 0: s.e('sub w10, w10, #%d' % -d)
            else: s.e(*mov32('w11', d), 'add w10, w10, w11')
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
    def ins(s, i, nd, fuse=None):
        mn = i.mnemonic; ops = i.operands; n = len(ops)
        if mn == 'nop': return
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
            s.addr(ops[1], lea=True); s.e('mov %s, w10' % W(dr)); return
        if mn in ('add', 'sub', 'and', 'or', 'xor', 'cmp', 'test') and n == 2 and ops[0].size == 4:
            return s.alu(mn, ops, nd, fuse)
        if mn in ('inc', 'dec') and n == 1 and ops[0].type == 1 and ops[0].size == 4:
            r = s.reg(ops[0]); need = nd & {'zf', 'sf', 'of', 'pf'}
            if need: s.e('%s w0, %s, #1' % ('adds' if mn == 'inc' else 'subs', W(r)))
            else: s.e('%s %s, %s, #1' % ('add' if mn == 'inc' else 'sub', W(r), W(r))); return
            s.setf(need, 'add' if mn == 'inc' else 'sub', 'w0'); s.e('mov %s, w0' % W(r)); return
        if mn == 'push' and n == 1 and ops[0].size == 4:
            v = s.val(ops[0], 'w0') if ops[0].type != 3 else s.val(ops[0], 'w0')
            s.e('sub w23, w23, #4', 'str %s, [x23]' % v); return
        if mn == 'pop' and n == 1 and ops[0].type == 1 and ops[0].size == 4:
            r = s.reg(ops[0])
            if r == 4: raise Fn()
            s.e('ldr %s, [x23]' % W(r), 'add w23, w23, #4'); return
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
    def ins2(s, i, nd, fuse=None):
        """sized moves, extensions, shifts, 8-bit compares and the rest of the common integer forms"""
        mn = i.mnemonic; ops = i.operands; n = len(ops); need = nd & set(FB)
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
        raise Fn()
    def alu(s, mn, ops, nd, fuse):
        d, sr = ops; need = nd & set(FB)
        if d.type == 3 and sr.type == 3: raise Fn()
        if d.type == 1: dr = s.reg(d); dn = W(dr)
        elif d.type == 3: dr = None; dn = None
        else: raise Fn()
        # source first (it may use w0/w1 as scratch), then the destination memory operand's address in x10
        if d.type == 3:
            if d.size != 4: raise Fn()
            sv = s.val(sr, 'w2') if sr.type != 3 else None
            if sv is None: raise Fn()
            a = s.addr(d); s.e('ldr w0, %s' % a); dv = 'w0'
        else:
            sv = s.val(sr, 'w2'); dv = dn
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

# ======================= block terminators + program builder =======================
def disp_site(o, sh, mulc, tag):
    """Per-site dispatch of the guest pc in w9: first probe inline (its own indirect branch), collisions/misses go to the shared slow path."""
    o.extend(mov32('w10', mulc) + ['mul w10, w9, w10', 'lsr w10, w10, #%d' % sh, 'add x10, x29, x10, lsl #4', 'ldr w11, [x10]', 'cmp w11, w9', 'b.ne aot_a64_slow', 'ldr x12, [x10, #8]', 'br x12'])

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
    def fallback(s, o, i, nd, ctl=False):
        w, r, var = s.flagrw(i); s.fb += 1
        o.extend(['stp x19, x20, [x27]', 'stp x21, x22, [x27, #16]', 'stp x23, x24, [x27, #32]', 'stp x25, x26, [x27, #48]'])
        if r or w or var or ctl: o.append('bl aot_a64_fl_store')
        o.extend(['mov x0, x27'] + mov32('w1', i.address) + ['bl %s' % s.cfun(i, nd), 'mov w9, w0'])
        o.extend(['ldr w19, [x27]', 'ldr w20, [x27, #8]', 'ldr w21, [x27, #16]', 'ldr w22, [x27, #24]', 'ldr w23, [x27, #32]', 'ldr w24, [x27, #40]', 'ldr w25, [x27, #48]', 'ldr w26, [x27, #56]'])
        if w or var or ctl: o.append('bl aot_a64_fl_load')
        if ctl: o.extend(mov32('w10', i.address + i.size) + ['cmp w9, w10', 'b.ne aot_a64_slow'])
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
        o = []; i0 = b[0]; live = set(s.live_out[i0.address]); needs = []
        for i in reversed(b):
            w, r, var = s.flagrw(i)
            if var: needs.append(set(s.F5)); live |= r; continue
            needs.append(live & w); live = (live - w) | r
        needs.reverse()
        o.extend(['.globl %s' % s.lbl(i0.address), '.hidden %s' % s.lbl(i0.address), '%s:' % s.lbl(i0.address)])
        nat = s.natblk.get(i0.address)
        if nat: s.fallback(o, i0, set(s.F5), True); return o
        B = Blk(None, None, s.sh, '%x' % i0.address)
        last = b[-1]; lm = last.mnemonic.split()[-1]
        elided = lm == 'call' and last.operands and last.operands[0].type == 2 and s.elided(last.operands[0].imm)
        ends = (lm in s.CTL or lm.startswith('j')) and not elided
        body = b[:-1] if ends else b
        jcc = lm.startswith('j') and (lm[1:] in CC or lm[1:] in ('p', 'np', 'pe', 'po')) and last.operands[0].type == 2
        fz = None   # compare/branch fusion: the flag consumer is the block's own conditional branch and nothing after it reads what the compare writes
        if jcc and len(b) >= 2:
            prev = b[-2]; arm = s.fuse_kind(prev, lm[1:]); w, r, var = s.flagrw(prev)
            if arm and not var and not (set(s.live_out[i0.address]) & w) and prev.address not in s.special: fz = (prev, arm)
        for idx, (i, nd) in enumerate(zip(b, needs)):
            if ends and i is last: break
            if elided and i is last: break
            if fz and i is fz[0]:
                try: B.o = []; B.ins(i, set(), fuse=True); o.extend(B.o); s.native += 1; continue
                except Fn: fz = None
            try:
                if i.address in s.special: raise Fn()
                B.o = []; B.ins(i, nd); o.extend(B.o); s.native += 1
            except Fn:
                ctl = i.mnemonic.split()[-1] in s.CTL or i.mnemonic.startswith('j')
                s.fallback(o, i, nd, ctl)
                if ctl and i is last: return o
        if not ends:
            s.goto(o, last.address + last.size); return o
        nd = needs[-1]; nx = last.address + last.size; op0 = last.operands[0] if last.operands else None
        if lm == 'jmp' and op0 is not None and op0.type == 2:
            s.goto(o, op0.imm & 0xffffffff); return o
        if jcc:
            tgt = op0.imm & 0xffffffff; cc = lm[1:]; lt = '.Lj%x' % last.address
            if fz: o.append('b.%s %s' % (fz[1], lt))
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
            o.extend(mov32('w0', nx) + ['sub w23, w23, #4', 'str w0, [x23]']); s.goto(o, tgt); return o
        if lm in ('call', 'jmp') and op0 is not None and op0.type in (1, 3) and op0.size == 4 and op0.type == 3 and not op0.mem.segment or lm in ('call', 'jmp') and op0 is not None and op0.type == 1 and op0.size == 4:
            B.o = []
            try:
                if op0.type == 1: B.o.append('mov w9, %s' % W(B.reg(op0)))
                else: a = B.addr(op0); B.o.append('ldr w9, %s' % a)
                o.extend(B.o)
                if lm == 'call': o.extend(mov32('w0', nx) + ['sub w23, w23, #4', 'str w0, [x23]'])
                disp_site(o, s.sh, s.mulc, 'i%x' % last.address); return o
            except Fn: pass
        if lm == 'ret' and not last.operands:
            o.extend(['ldr w9, [x23]', 'add w23, w23, #4']); disp_site(o, s.sh, s.mulc, 'r%x' % last.address); return o
        if lm == 'ret' and op0 is not None and op0.type == 2:
            o.extend(['ldr w9, [x23]', 'add w23, w23, #%d' % (4 + op0.imm)]); disp_site(o, s.sh, s.mulc, 'r%x' % last.address); return o
        s.fallback(o, last, nd, True); return o
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
         'mov x27, x0', 'adrp x29, aot_a64_tab', 'add x29, x29, :lo12:aot_a64_tab', 'mov w9, w1',
         'ldr w19, [x27]', 'ldr w20, [x27, #8]', 'ldr w21, [x27, #16]', 'ldr w22, [x27, #24]', 'ldr w23, [x27, #32]', 'ldr w24, [x27, #40]', 'ldr w25, [x27, #48]', 'ldr w26, [x27, #56]', 'bl aot_a64_fl_load', 'b aot_a64_slow',
         '.globl aot_a64_exit', '.hidden aot_a64_exit', 'aot_a64_exit:',
         'stp x19, x20, [x27]', 'stp x21, x22, [x27, #16]', 'stp x23, x24, [x27, #32]', 'stp x25, x26, [x27, #48]', 'bl aot_a64_fl_store', 'mov w0, w9',
         'ldp x27, x28, [sp, #80]', 'ldp x25, x26, [sp, #64]', 'ldp x23, x24, [sp, #48]', 'ldp x21, x22, [sp, #32]', 'ldp x19, x20, [sp, #16]', 'ldp x29, x30, [sp], #96', 'ret',
         '.globl aot_a64_fl_store', '.hidden aot_a64_fl_store', 'aot_a64_fl_store:',
         'and w0, w28, #1', 'strb w0, [x27, #%d]' % OFF, 'ubfx w0, w28, #6, #1', 'strb w0, [x27, #%d]' % (OFF + 1), 'ubfx w0, w28, #7, #1', 'strb w0, [x27, #%d]' % (OFF + 2),
         'ubfx w0, w28, #11, #1', 'strb w0, [x27, #%d]' % (OFF + 3), 'ubfx w0, w28, #2, #1', 'strb w0, [x27, #%d]' % (OFF + 4), 'ret',
         '.globl aot_a64_fl_load', '.hidden aot_a64_fl_load', 'aot_a64_fl_load:',
         'ldrb w28, [x27, #%d]' % OFF, 'ldrb w0, [x27, #%d]' % (OFF + 1), 'ldrb w1, [x27, #%d]' % (OFF + 2), 'ldrb w2, [x27, #%d]' % (OFF + 3), 'ldrb w3, [x27, #%d]' % (OFF + 4),
         'bfi w28, w0, #6, #1', 'bfi w28, w1, #7, #1', 'bfi w28, w2, #11, #1', 'bfi w28, w3, #2, #1', 'ret',
         '.globl aot_a64_slow', '.hidden aot_a64_slow', 'aot_a64_slow:']
    o += mov32('w10', prog.mulc) + ['mul w10, w9, w10', 'lsr w10, w10, #%d' % sh,
         '.Lsl_loop:', 'add x11, x29, x10, lsl #4', 'ldr w12, [x11]', 'cbz w12, aot_a64_exit', 'cmp w12, w9', 'b.eq .Lsl_hit', 'add w10, w10, #1', 'and w10, w10, #%d' % mask, 'b .Lsl_loop',
         '.Lsl_hit:', 'ldr x12, [x11, #8]', 'br x12']
    for k in range(nregions): o += ['.globl region_%d' % k, '.hidden region_%d' % k, '.type region_%d, %%function' % k, 'region_%d:' % k, 'b aot_a64_enter']
    o += ['.section .data.rel.ro,"aw",@progbits', '.balign 16', '.globl aot_a64_tab', '.hidden aot_a64_tab', 'aot_a64_tab:']
    for a in place:
        if a is None: o += ['.word 0, 0', '.quad 0']
        else: o += ['.word 0x%x, 0' % a, '.quad L_%x' % a]
    o += ['.text']
    return o

def to_c(lines):
    """wrap assembly lines as a C translation unit made of one top-level asm statement"""
    out = ['__asm__(']
    for l in lines: out.append('"%s\\n"' % l.replace('\\', '\\\\').replace('"', '\\"'))
    out.append(');\n')
    return '\n'.join(out)
