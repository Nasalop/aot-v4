import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(
    0, str(Path(__file__).resolve().parents[1] / "src" / "tools")
)

import pe_dependency_inspect as inspector


class DependencyInspectorTests(unittest.TestCase):
    def test_scans_real_pe_imports_and_matches_runtime_coverage(self):
        executable = Path(__file__).with_name("complex_game.exe")
        report = inspector.DependencyInspector(
            executable, recursive=False
        ).inspect()
        self.assertIn("msvcrt.dll", report["imported_dlls"])
        imported = {
            entry["symbol"]
            for entry in report["imports"]
            if entry["dll"] == "msvcrt.dll"
        }
        self.assertTrue({"printf", "sin", "sqrt", "exit"}.issubset(imported))
        self.assertIn("msvcrt.dll", report["implemented_imported_dlls"])
        self.assertEqual(report["missing_functions"], [])

    def test_report_distinguishes_placeholder_handlers(self):
        executable = Path(__file__).parents[1] / "game" / "stresst_test.exe"
        report = inspector.DependencyInspector(
            executable, recursive=False
        ).inspect()
        self.assertIn(
            {"dll": "user32.dll", "symbol": "ShowWindow",
             "imported_by": str(executable.resolve())},
            report["placeholder_functions"],
        )
        self.assertEqual(report["missing_functions"], [])

    def test_runtime_coverage_is_dll_scoped(self):
        dependency = inspector.DependencyInspector("unused.exe", recursive=False)
        self.assertTrue(dependency._is_implemented("msvcrt.dll", "printf"))
        self.assertFalse(dependency._is_implemented("kernel32.dll", "printf"))
        self.assertTrue(
            dependency._is_implemented("ntdll.dll", "RtlCaptureContext")
        )
        self.assertTrue(
            dependency._is_implemented("kernel32.dll", "RtlCaptureContext")
        )

    def test_local_dependency_imports_are_recursed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "game.exe"
            child = Path(directory) / "helper.dll"
            root.touch()
            child.touch()
            root_pe = SimpleNamespace(
                DIRECTORY_ENTRY_IMPORT=[
                    SimpleNamespace(
                        dll=b"helper.dll",
                        imports=[SimpleNamespace(name=b"HelperCall", ordinal=None)],
                    )
                ],
                DIRECTORY_ENTRY_EXPORT=SimpleNamespace(symbols=[]),
                DIRECTORY_ENTRY_DELAY_IMPORT=[],
            )
            child_pe = SimpleNamespace(
                DIRECTORY_ENTRY_IMPORT=[],
                DIRECTORY_ENTRY_EXPORT=SimpleNamespace(symbols=[]),
                DIRECTORY_ENTRY_DELAY_IMPORT=[],
            )
            dependency = inspector.DependencyInspector(root)
            with patch.object(
                dependency, "_read_pe", side_effect=[root_pe, child_pe]
            ):
                report = dependency.inspect()
            self.assertEqual(len(report["inspected_images"]), 2)
            self.assertEqual(
                [item["dll"] for item in report["imports"]], ["helper.dll"]
            )

    def test_static_import_reader_preserves_ordinals(self):
        item = SimpleNamespace(name=None, ordinal=123)
        descriptor = SimpleNamespace(
            dll=b"EXAMPLE.dll", imports=[item]
        )
        fake_pe = SimpleNamespace(DIRECTORY_ENTRY_IMPORT=[descriptor])
        self.assertEqual(
            list(inspector.DependencyInspector._import_entries(fake_pe)),
            [("EXAMPLE.dll", "#123", False)],
        )

    def test_delay_import_reader_identifies_delayed_symbols(self):
        descriptor = SimpleNamespace(
            dll=b"late.dll",
            imports=[SimpleNamespace(name=b"LateCall", ordinal=None)],
        )
        fake_pe = SimpleNamespace(DIRECTORY_ENTRY_DELAY_IMPORT=[descriptor])
        self.assertEqual(
            list(inspector.DependencyInspector._import_entries(fake_pe)),
            [("late.dll", "LateCall", True)],
        )

    def test_forwarded_export_chains_are_followed(self):
        root = {
            "path": str(Path("kernel32.dll")),
            "exports": {
                "First": SimpleNamespace(forwarder=b"KERNELBASE.Second")
            },
        }
        second = {
            "path": str(Path("kernelbase.dll")),
            "exports": {
                "Second": SimpleNamespace(forwarder=b"FINAL.Third")
            },
        }
        final = {
            "path": str(Path("final.dll")),
            "exports": {"Third": SimpleNamespace(forwarder=None)},
        }
        dependency = inspector.DependencyInspector("unused.exe", recursive=False)
        with patch.object(
            dependency, "_find_dll",
            side_effect=[Path("kernelbase.dll"), Path("final.dll")],
        ), patch.object(
            dependency, "_visit", side_effect=[second, final]
        ):
            dependency._resolve_forwarders(root, "kernel32.dll", "First")
        self.assertEqual(
            dependency.forwarded_exports,
            [
                {"from": "kernel32.dll!First", "to": "kernelbase.dll!Second"},
                {"from": "kernelbase.dll!Second", "to": "final.dll!Third"},
            ],
        )
        self.assertEqual(dependency.unresolved_symbols, [])

    def test_dynamic_resolution_is_reported_as_runtime_observed(self):
        report = inspector.DependencyInspector(
            Path(__file__).with_name("complex_game.exe"), recursive=False
        ).inspect()
        self.assertIn(
            "AOT_VERBOSE",
            report["dynamic_resolution"]["note"],
        )

    def test_dynamic_runtime_log_parser_keeps_success_and_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "runtime.log"
            log.write_text(
                "[aot] dynamic LoadLibraryA resolved: kernel32.dll\n"
                "[aot] dynamic GetProcAddress unresolved: user32.dll!Missing\n"
                "[aot] dynamic GetProcAddress uses placeholder shim: user32.dll!ShowWindow\n",
                encoding="utf-8",
            )
            self.assertEqual(
                inspector.parse_runtime_log(log),
                [
                    {
                        "operation": "LoadLibrary",
                        "status": "resolved",
                        "target": "kernel32.dll",
                    },
                    {
                        "operation": "GetProcAddress",
                        "status": "unresolved",
                        "target": "user32.dll!Missing",
                    },
                    {
                        "operation": "GetProcAddress",
                        "status": "placeholder",
                        "target": "user32.dll!ShowWindow",
                    },
                ],
            )

    def test_windows_path_separator_is_normalized(self):
        self.assertEqual(
            inspector.canonical_dll(r"C:\Windows\System32\KERNEL32.DLL"),
            "kernel32.dll",
        )

    def test_api_set_imports_use_runtime_alias_coverage(self):
        dependency = inspector.DependencyInspector("unused.exe", recursive=False)
        manifest = dependency.manifest
        self.assertTrue(
            inspector.is_runtime_module("api-ms-win-core-heap-l1-1-0.dll", manifest)
        )
        self.assertTrue(
            dependency._is_implemented("api-ms-win-core-heap-l1-1-0.dll", "HeapAlloc")
        )
        self.assertFalse(
            dependency._is_implemented("api-ms-win-core-heap-l1-1-0.dll", "Sleep")
        )
        self.assertFalse(
            inspector.is_runtime_module("api-ms-win-core-unknown-l1-1-0.dll", manifest)
        )
        self.assertTrue(
            dependency._is_implemented(
                "api-ms-win-core-file-l1-1-0.dll", "CreateFileW"
            )
        )
        self.assertFalse(
            dependency._is_implemented(
                "api-ms-win-core-file-l1-1-0.dll", "SetLastError"
            )
        )
        self.assertFalse(
            dependency._is_implemented(
                "api-ms-win-core-processthreads-l1-1-0.dll",
                "GetCurrentDirectoryA",
            )
        )

if __name__ == "__main__":
    unittest.main()
