import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src" / "transpiler"))

import aot_transpiler


class SseMemoryMoveTests(unittest.TestCase):
    def translate(self, code):
        decoder = aot_transpiler.Cs(
            aot_transpiler.CS_ARCH_X86, aot_transpiler.CS_MODE_64
        )
        decoder.detail = True
        instruction = next(decoder.disasm(bytes.fromhex(code), 0x1000))
        gen = aot_transpiler.Gen(None)
        gen.i = instruction
        operands = instruction.operands
        xmm0 = operands[0].type == aot_transpiler.X86_OP_REG
        xmm1 = operands[1].type == aot_transpiler.X86_OP_REG
        return gen.sse(instruction, instruction.mnemonic, operands, len(operands),
                       xmm0, xmm1, ())

    def test_movlps_memory_store_uses_low_qword(self):
        self.assertEqual(
            self.translate("0f 13 00"),
            "ST(uint64_t,(uint64_t)(r0),c->x[0].q[0]);",
        )

    def test_movhps_memory_store_uses_high_qword(self):
        self.assertEqual(
            self.translate("0f 17 00"),
            "ST(uint64_t,(uint64_t)(r0),c->x[0].q[1]);",
        )

    def test_movlps_memory_load_updates_low_qword(self):
        self.assertEqual(
            self.translate("0f 12 00"),
            "c->x[0].q[0]=LD(uint64_t,(uint64_t)(r0));",
        )

    def test_movhps_memory_load_updates_high_qword(self):
        self.assertEqual(
            self.translate("0f 16 00"),
            "c->x[0].q[1]=LD(uint64_t,(uint64_t)(r0));",
        )

class ImageFingerprintTests(unittest.TestCase):
    def test_fingerprint_matches_fnv1a64(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "image.exe"
            path.write_bytes(b"hello")
            self.assertEqual(aot_transpiler.image_fingerprint(path), 0xA430D84680AABD0B)


if __name__ == "__main__":
    unittest.main()


class SseComparePredicates(unittest.TestCase):
    """Every cmp<pred>{ps,pd,ss,sd} pseudo-op must translate (no silent gaps)."""
    def test_all_32_predicates_translate(self):
        decoder = aot_transpiler.Cs(aot_transpiler.CS_ARCH_X86, aot_transpiler.CS_MODE_64)
        decoder.detail = True
        seen = 0
        for sfx, prefix in (('ps', '0f'), ('pd', '66 0f'), ('ss', 'f3 0f'), ('sd', 'f2 0f')):
            for imm in range(8):
                ins = next(decoder.disasm(bytes.fromhex('%s c2 c1 %02x' % (prefix, imm)), 0x1000))
                gen = aot_transpiler.Gen(None); gen.i = ins
                ops = ins.operands
                out = gen.sse(ins, ins.mnemonic, ops, len(ops), True, True, ())
                self.assertIn('?', out, ins.mnemonic); seen += 1
        self.assertEqual(seen, 32)


class NativeEquivalence(unittest.TestCase):
    """rcl/rcr/loop/cmp predicates: translated output must equal native gcc output."""
    def test_insn_t6_matches_native(self):
        import subprocess
        root = Path(__file__).resolve().parents[1]
        subprocess.run(["./aot.sh", "tests/insn_t6.exe", "ut_insn_t6"], cwd=root, check=True, capture_output=True)
        out = subprocess.run(["./output/ut_insn_t6", "tests/insn_t6.exe"], cwd=root, capture_output=True, text=True).stdout
        self.assertEqual(out, (root / "tests" / "insn_t6.expected").read_text())
