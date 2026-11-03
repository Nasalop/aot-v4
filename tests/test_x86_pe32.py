import sys
"""PE32 (32-bit x86) pipeline tests: translate+build+run, import resolution, compat report. Needs gcc, capstone, pefile."""
import json, os, subprocess, sys, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src" / "tools"))
ENV = dict(os.environ)

def build(exe, name):
    p = subprocess.run(["bash", str(ROOT / "aot.sh"), str(ROOT / "tests" / exe), name], capture_output=True, text=True, cwd=ROOT, env=ENV)
    assert p.returncode == 0, p.stdout + p.stderr
    return ROOT / "output" / name

def run(binary, exe):
    return subprocess.run([str(binary), str(ROOT / "tests" / exe)], capture_output=True, text=True, timeout=60)

class X86(unittest.TestCase):
    def test_system_dlls_winsock_crypt_bstr_window_wndproc(self):
        # ws2_32 / advapi32 / oleaut32 / user32: 32-bit layouts, WNDCLASSEX, CREATESTRUCT, WndProc callback through the shim adapter
        r = run(build("x86_t7.exe", "ut_x86t7"), "x86_t7.exe")
        self.assertEqual(r.stdout, (ROOT / "tests" / "x86_t7.expected").read_text(), r.stderr)
    def test_remaining_dlls_are_in_the_runtime_manifest(self):
        sys.path.insert(0, str(ROOT / "src" / "tools")); import pe_dependency_inspect as i
        m = i.x86_manifest()
        for dll in ("advapi32.dll", "binkw32.dll", "d3dx10_36.dll", "dinput8.dll", "dsound.dll", "eax.dll", "iphlpapi.dll", "ole32.dll", "oleaut32.dll", "shell32.dll", "user32.dll", "wininet.dll", "winmm.dll", "ws2_32.dll", "xinput1_3.dll"):
            self.assertTrue(m.get(dll), dll)
        self.assertIn("#11", m["dsound.dll"]); self.assertIn("_BinkOpen", m["binkw32.dll"])

    def test_core_translation_and_run(self):
        r = run(build("x86_t1.exe", "ut_x86t1"), "x86_t1.exe")
        self.assertEqual(r.returncode, 5)
        for want in ("fib(20)=6765", "hash=827720373", "fc=123 cd=5", "poly*1000=17607", "struct:-27000000000,15", "sorted:0,50,99 heap<4G=1", "mul64:-1234567000 sh:-154320875"):
            self.assertIn(want, r.stdout)
    def test_cdecl_fp_imports_and_command_line(self):
        r = run(build("x86_t2.exe", "ut_x86t2"), "x86_t2.exe")
        self.assertIn("sqrt2e6=1414213 pow=81 atan2e6=785398", r.stdout)
        self.assertIn("len=5 low=1", r.stdout)
        self.assertEqual(r.returncode, 0)
        self.assertIn("GetCommandLineA", r.stdout)
        self.assertIn("x86_t2.exe", r.stdout)
    def test_local_interlocked_handles_and_x87_reverse_ops(self):
        r = run(build("x86_t3.exe", "ut_x86t3"), "x86_t3.exe")
        self.assertEqual(r.returncode, 3)
        self.assertIn("inter=6,6,16,99", r.stdout)            # Interlocked* implemented in win_x86.c
        self.assertIn("wait=0 proc=1", r.stdout)               # 64-bit tagged handle aliased to 32 bits and back
        self.assertIn("x87rev=-6000,400,6000,2500", r.stdout)  # fsubr/fdivr (matches native gcc)
    def test_d3d10_com_stdcall_vtables(self):
        r = run(build("x86_d3d10.exe", "ut_x86d3d10"), "x86_d3d10.exe")   # 32-bit D3D10+DXGI: low objects, 4-byte vtables, stdcall method pops, 60-byte swapchain desc
        self.assertEqual(r.returncode, 4)
        for want in ("create hr=0 objs_low=1", "getbuffer hr=0", "creatertv hr=0", "present hr=0", "getdesc w=640 h=360"): self.assertIn(want, r.stdout)
        self.assertIn("present #1 clear=(0.200,0.400,0.600", r.stderr + r.stdout)
    def test_arch_mismatch_rejected(self):
        b = build("x86_t1.exe", "ut_x86t1")
        r = subprocess.run([str(b), str(ROOT / "tests" / "t1.exe")], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2); self.assertIn("architecture", r.stderr)
    def test_compat_report(self):
        import aot_compat_report as C
        out = ROOT / "output" / "ut_compat"; C.main([str(ROOT / "tests" / "x86_t2.exe"), "--out-dir", str(out)])
        R = json.loads((out / "compat.json").read_text())
        self.assertTrue(R["architecture"]["pe32"]); self.assertEqual(R["architecture"]["machine"], 0x14C)
        self.assertEqual(R["translation"]["status"], "ok")
        self.assertEqual(R["dependencies"]["missing_functions"], [])
        self.assertIn("UNVERIFIED", R["verdict"])
    def test_x64_still_ok(self):
        import aot_compat_report as C
        out = ROOT / "output" / "ut_compat64"; C.main([str(ROOT / "tests" / "t1.exe"), "--out-dir", str(out)])
        R = json.loads((out / "compat.json").read_text()); self.assertFalse(R["architecture"]["pe32"]); self.assertEqual(R["translation"]["status"], "ok")
    def test_unsupported_arch_reported_not_crashed(self):
        import aot_compat_report as C
        out = ROOT / "output" / "ut_compat_arm"; arm = out / "arm.exe"; out.mkdir(parents=True, exist_ok=True)
        d = bytearray((ROOT / "tests" / "x86_t1.exe").read_bytes()); pe = int.from_bytes(d[0x3c:0x40], "little"); d[pe + 4:pe + 6] = (0xAA64).to_bytes(2, "little"); arm.write_bytes(d)
        C.main([str(arm), "--out-dir", str(out)]); R = json.loads((out / "compat.json").read_text())
        self.assertFalse(R["architecture"]["translatable"]); self.assertEqual(R["translation"]["status"], "not attempted")

if __name__ == "__main__": unittest.main()
