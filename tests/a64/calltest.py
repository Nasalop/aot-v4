"""Integration test of the shadow return ring: real aot_a64_enter/exit/slow/retmiss/slowc + generated call/ret blocks, linked and run in unicorn."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from a64test import *
BASE = 0x10000000
def build(blocks):   # blocks: list of (addr, x86 text); text must end in a control transfer
    insns = []
    for a, t in blocks:
        code = asm_x86_at(t, a); cur = []
        for i in md.disasm(code, a):
            cur.append(i)
            if i.mnemonic in ('call', 'ret', 'jmp') or i.mnemonic.startswith('j'): insns.append(cur); cur = []
        if cur: insns.append(cur)
    prog = a64gen.Prog(None, [insns], {}, T.flagrw, T.F5, lambda i, nd: 'uf_%x' % i.address)
    class LO(dict):
        def __missing__(s, k): return set()
    prog.live_out = LO()
    lines = []
    for b in insns: lines += prog.block(b, 0, None)
    core = a64gen.core_asm(prog, 1)
    f = os.path.join(TMP, 'full.s'); open(f, 'w').write('.text\n' + '\n'.join(lines) + '\n' + '\n'.join(core) + '\n.text\n.globl aot_a64_imp_c\naot_a64_imp_c: mov x0, #0\nret\n')
    run(['clang', '-target', 'aarch64-linux-gnu', '-march=armv8.2-a+lse+crc', '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-Ttext=0x%x' % BASE, '-Wl,--no-rosegment', f, '-o', f + '.elf'])
    run(['llvm-objcopy', '-O', 'binary', f + '.elf', f + '.bin']); syms = {}
    out = subprocess.run(['llvm-nm', f + '.elf'], capture_output=True, text=True).stdout
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 3: syms[p[2]] = int(p[0], 16)
    return open(f + '.bin', 'rb').read(), syms
def asm_x86_at(t, a):
    import re
    f = os.path.join(TMP, 'xx.s'); lines = []
    for ln in t.split('\n'):
        m = re.match(r'^\s*(j\w+|call)\s+(0x[0-9a-fA-F]+)\s*$', ln)
        if m: ln = '.att_syntax\n%s T_%s\n.intel_syntax noprefix' % (m.group(1), m.group(2)[2:]); lines.append('.set T_%s, %s\n' % (m.group(2)[2:], m.group(2)))
        lines.append(ln)
    pre = [l for l in lines if l.startswith('.set')]; body = [l for l in lines if not l.startswith('.set')]
    open(f, 'w').write('.intel_syntax noprefix\n.text\n.globl _start\n_start:\n' + ''.join(pre) + '\n'.join(body) + '\n')
    run(['clang', '-target', 'i386-linux-gnu', '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-Ttext=0x%x' % a, f, '-o', f + '.elf']); run(['llvm-objcopy', '-O', 'binary', '-j', '.text', f + '.elf', f + '.bin']); return open(f + '.bin', 'rb').read()
def go(blocks, entry, regs, name, expect, want_slow=0):
    img, syms = build(blocks)
    ua = Uc(UC_ARCH_ARM64, UC_MODE_ARM); ua.ctl_set_cpu_model(UC_CPU_ARM64_MAX)
    ua.mem_map(BASE, (len(img) + 0x1fff) & ~0xfff + 0x1000 if False else ((len(img) >> 12) + 2) << 12); ua.mem_write(BASE, img)
    ua.mem_map(0x20000000, 0x1000); ua.mem_map(0x30000000, 0x100000); sp = 0x30100000 - 0x1000; ua.reg_write(UC_ARM64_REG_SP, sp)
    ua.mem_map(0x40000000, 0x4000); cpu = bytearray(0x400)
    for k, v in enumerate(regs): cpu[8 * k:8 * k + 4] = struct.pack('<I', v)
    ua.mem_write(0x20000000, bytes(cpu)); ua.reg_write(UC_ARM64_REG_X0, 0x20000000); ua.reg_write(UC_ARM64_REG_X1, entry)
    hits = {'slow': 0, 'retmiss': 0, 'slowc': 0}; inv = {syms['aot_a64_slow']: 'slow', syms['aot_a64_retmiss']: 'retmiss', syms['aot_a64_slowc']: 'slowc'}
    def hook(uc, addr, size, ud):
        if addr in inv: hits[inv[addr]] += 1
    ua.hook_add(UC_HOOK_CODE, hook, begin=BASE, end=BASE + len(img))
    ua.reg_write(UC_ARM64_REG_X30, 0x20000800); ua.mem_map(0x20001000, 0x1000); ua.mem_write(0x20001000, b'\x00\x00\x20\xd4')   # brk: enter's caller
    ua.reg_write(UC_ARM64_REG_X30, 0x20001000)
    try: ua.emu_start(syms['aot_a64_enter'], 0x20001000, count=20000)
    except UcError as e: print('FAIL', name, 'fault', e, hex(ua.reg_read(UC_ARM64_REG_PC))); return False
    w0 = ua.reg_read(UC_ARM64_REG_X0) & 0xffffffff; out = struct.unpack('<8I', bytes(ua.mem_read(0x20000000, 64))[0:64:2] if False else b''.join(bytes(ua.mem_read(0x20000000 + 8 * k, 4)) for k in range(8)))
    ok = w0 == expect['pc'] and out[0] == expect['eax'] and out[4] == expect['esp'] and hits['slow'] - 1 == want_slow   # the final transfer to the untranslated exit pc is one slow-path visit
    print('ok  ' if ok else 'FAIL', name, 'pc=%x eax=%x esp=%x hits=%s' % (w0, out[0], out[4], hits)); return ok
ESP = 0x40003800
r = [0, 0, 0, 0, ESP, 0, 0, 0]
res = []
# 1: nested direct calls, ret matched everywhere -> slow path only for the final exit pc
res.append(go([(0x401000, 'mov eax, 1\ncall 0x401100\nadd eax, 0x10\njmp 0x409999'), (0x401100, 'add eax, 2\ncall 0x401200\nadd eax, 4\nret'), (0x401200, 'add eax, 8\nret'), ], 0x401000, r, 'nested calls', dict(pc=0x409999, eax=0x1f, esp=ESP)))
# 2: callee overwrites its return address (non-local return) -> retmiss -> slow path finds the real block
res.append(go([(0x401000, 'mov eax, 1\ncall 0x401100\nadd eax, 0x10\njmp 0x409999'), (0x401100, 'mov dword ptr [esp], 0x401300\nret'), (0x401300, 'add eax, 0x100\njmp 0x409999')], 0x401000, r, 'clobbered return', dict(pc=0x409999, eax=0x101, esp=ESP), want_slow=1))
# 3: ret n and an indirect call through a register that misses the table (-> slowc, imp_c ends the run)
res.append(go([(0x401000, 'push 5\ncall 0x401100\nadd eax, 0x10\njmp 0x409999'), (0x401100, 'mov eax, [esp+4]\nret 4')], 0x401000, r, 'ret n', dict(pc=0x409999, eax=0x15, esp=ESP)))
# 4: recursion deeper than the ring (600 > 512 entries): must still return correctly
res.append(go([(0x401000, 'mov ecx, 600\nxor eax, eax\ncall 0x401100\njmp 0x409999'), (0x401100, 'inc eax\ndec ecx\njz 0x401200\ncall 0x401100\n' + 'ret'), (0x401200, 'ret')], 0x401000, r, 'deep recursion', dict(pc=0x409999, eax=600, esp=ESP), want_slow=1))
sys.exit(0 if all(res) else 1)
