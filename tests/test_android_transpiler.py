import importlib.metadata
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANDROID_PYTHON = ROOT / "android" / "app" / "src" / "main" / "python"
CAPSTONE_NATIVE_DIRECTORY = Path(
    importlib.metadata.distribution("capstone").locate_file("capstone/lib")
)
if os.name == "nt":
    os.environ["LIBCAPSTONE_PATH"] = str(CAPSTONE_NATIVE_DIRECTORY)
sys.path[:0] = [
    str(ANDROID_PYTHON),
    str(ROOT / "src" / "transpiler"),
    str(ROOT / "src" / "tools"),
]

import aot_android


class OnDeviceTranspilerTests(unittest.TestCase):
    def test_translates_x64_fixture_and_writes_dependency_report(self):
        executable = ROOT / "game" / "stresst_test.exe"
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "translated"
            output.mkdir()
            result = json.loads(
                aot_android.translate(
                    executable,
                    output,
                    ROOT / "src" / "runtime",
                    CAPSTONE_NATIVE_DIRECTORY,
                )
            )

            self.assertGreater(result["instruction_count"], 0)
            self.assertGreater(result["region_count"], 0)
            self.assertTrue((output / "aot_tab.c").is_file())
            self.assertTrue((output / "aot_part0.c").is_file())
            self.assertTrue(Path(result["dependency_report"]).is_file())


if __name__ == "__main__":
    unittest.main()
