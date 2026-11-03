#!/usr/bin/env python3
"""Inspect PE imports, dependency files, export forwarders, and runtime coverage."""

import argparse
import json
import os
import re
import sys
from collections import defaultdict
from pathlib import Path

import pefile


ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "src" / "runtime"


def canonical_dll(name):
    return Path(name.replace("\\", "/")).name.lower()


def table_symbols(source, table):
    match = re.search(
        r"const\s+ShimEnt\s+" + re.escape(table) + r"\s*\[\s*\]\s*=\s*\{(.*?)\n\};",
        source,
        re.DOTALL,
    )
    if not match:
        raise ValueError(f"shim table {table} not found")
    body = match.group(1)
    names = {m.group(1).strip() for m in re.finditer(r"\bS\(([^)]+)\)", body)}
    names.update(m.group(1).strip() for m in re.finditer(r"\bE\(([^)]+)\)", body))
    names.update(
        m.group(1) for m in re.finditer(r'\bEA\("([^"]+)"\s*,', body)
    )
    names.update(
        m.group(1) for m in re.finditer(r'\bSA\("([^"]+)"\s*,', body)
    )
    names.update(
        m.group(1) for m in re.finditer(r'\{\s*"([^"]+)"\s*,', body)
    )
    return names


def table_placeholders(source, table):
    match = re.search(
        r"const\s+ShimEnt\s+" + re.escape(table) + r"\s*\[\s*\]\s*=\s*\{(.*?)\n\};",
        source,
        re.DOTALL,
    )
    if not match:
        raise ValueError(f"shim table {table} not found")
    body = match.group(1)
    names = {
        item.group(1)
        for item in re.finditer(r'\bSA\("([^"]+)"\s*,\s*(?:zero|one)\s*\)', body)
    }
    names.update(
        item.group(1)
        for item in re.finditer(r'\{\s*"([^"]+)"\s*,\s*u_(?:zero|one)\s*\}', body)
    )
    return names


def runtime_manifest(runtime_dir=RUNTIME):
    shims = (runtime_dir / "shims.c").read_text(encoding="utf-8")
    file_shims = (runtime_dir / "win_file.c").read_text(encoding="utf-8")
    audio_shims = (runtime_dir / "win_audio.c").read_text(encoding="utf-8")
    gfx_shims = (runtime_dir / "win_gfx.c").read_text(encoding="utf-8")
    d3d10_shims = (runtime_dir / "win_d3d10.c").read_text(encoding="utf-8")
    compat_path = runtime_dir / "win_compat.c"
    compat = table_symbols(compat_path.read_text(encoding="utf-8"), "aot_shim_table_compat") if compat_path.exists() else set()
    sys_shims = table_symbols(
        (runtime_dir / "win_sys.c").read_text(encoding="utf-8"),
        "aot_shim_table_sys",
    )

    crt = table_symbols(shims, "aot_shim_table_crt")
    win32 = table_symbols(shims, "aot_shim_table_win32")
    ntdll = table_symbols(shims, "aot_shim_table_ntdll")
    files = table_symbols(file_shims, "aot_shim_table_file")
    kernel32 = win32 | files | compat | sys_shims
    audio = table_symbols(audio_shims, "aot_shim_table_audio")
    gfx = table_symbols(gfx_shims, "aot_shim_table_gfx")
    d3d10 = table_symbols(d3d10_shims, "aot_shim_table_d3d10")

    d3d11 = {"D3D11CreateDevice", "D3D11CreateDeviceAndSwapChain"}
    dxgi = {name for name in gfx if name.startswith("CreateDXGIFactory")}
    winmm = audio | {"timeGetTime", "timeBeginPeriod", "timeEndPeriod"}
    user32 = gfx - d3d11 - dxgi - winmm
    api_sets = {
        "api-ms-win-core-file-*": files - {"GetLastError", "SetLastError"},
        "api-ms-win-core-heap-*": {name for name in win32 if name.startswith("Heap")},
        "api-ms-win-core-memory-*": {
            "VirtualAlloc", "VirtualFree", "VirtualProtect"
        },
        "api-ms-win-core-libraryloader-*": {
            name for name in win32
            if name.startswith(("LoadLibrary", "GetModuleHandle"))
            or name == "GetProcAddress"
        },
        "api-ms-win-core-processthreads-*": {
            name for name in win32
            if name.startswith(("GetCurrent", "Tls")) or name == "ExitProcess"
        },
        "api-ms-win-core-fibers-*": {
            name for name in win32 if name.startswith("Fls")
        },
        "api-ms-win-core-processenvironment-*": {
            name for name in win32
            if name.startswith(
                (
                    "GetCommandLine",
                    "GetStartupInfo",
                    "GetEnvironmentStrings",
                    "FreeEnvironmentStrings",
                )
            )
        },
        "api-ms-win-core-synch-*": {
            name for name in win32
            if name == "Sleep"
            or name.startswith(
                (
                    "InitializeCriticalSection",
                    "EnterCriticalSection",
                    "LeaveCriticalSection",
                    "DeleteCriticalSection",
                    "TryEnterCriticalSection",
                )
            )
        },
        "api-ms-win-core-sysinfo-*": {
            name for name in win32
            if name.startswith(("GetTickCount", "QueryPerformance"))
            or name in {"GetSystemInfo", "GetSystemTimeAsFileTime"}
        },
        "api-ms-win-core-datetime-*": {
            name for name in win32
            if name.startswith(("GetTickCount", "QueryPerformance"))
            or name in {"GetSystemInfo", "GetSystemTimeAsFileTime"}
        },
        "api-ms-win-core-console-*": {
            name for name in win32
            if name in {"GetStdHandle", "GetConsoleMode"}
            or name.startswith("SetConsole")
        },
        "api-ms-win-core-errorhandling-*": {
            name for name in kernel32
            if name.startswith(("SetUnhandledException", "UnhandledException"))
            or name in {"SetErrorMode", "IsDebuggerPresent"}
        }
        | (files & {"GetLastError", "SetLastError"}),
        "api-ms-win-core-localization-*": {"GetACP"} & win32,
        "api-ms-win-crt-*": crt,
    }
    return {
        "kernel32.dll": kernel32,
        "kernelbase.dll": kernel32,
        "msvcrt.dll": crt,
        "ucrtbase.dll": crt,
        "ntdll.dll": ntdll,
        "user32.dll": user32,
        "winmm.dll": winmm,
        "d3d10.dll": d3d10,
        "d3d11.dll": d3d11,
        "dxgi.dll": dxgi,
        **api_sets,
    }


def runtime_placeholder_manifest(runtime_dir=RUNTIME, manifest=None):
    manifest = manifest or runtime_manifest(runtime_dir)
    shims = (runtime_dir / "shims.c").read_text(encoding="utf-8")
    file_shims = (runtime_dir / "win_file.c").read_text(encoding="utf-8")
    gfx_shims = (runtime_dir / "win_gfx.c").read_text(encoding="utf-8")
    crt = table_placeholders(shims, "aot_shim_table_crt")
    win32 = table_placeholders(shims, "aot_shim_table_win32")
    files = table_placeholders(file_shims, "aot_shim_table_file")
    ntdll = table_placeholders(shims, "aot_shim_table_ntdll")
    gfx = table_placeholders(gfx_shims, "aot_shim_table_gfx")
    all_core = win32 | files
    placeholders = {
        "msvcrt.dll": crt,
        "ucrtbase.dll": crt,
        "kernel32.dll": all_core,
        "kernelbase.dll": all_core,
        "ntdll.dll": ntdll,
        "user32.dll": gfx & manifest["user32.dll"],
        "winmm.dll": gfx & manifest["winmm.dll"],
        "d3d10.dll": set(),
        "d3d11.dll": set(),
        "dxgi.dll": set(),
    }
    for pattern, symbols in manifest.items():
        if pattern.startswith("api-ms-win-core-"):
            placeholders[pattern] = symbols & all_core
        elif pattern == "api-ms-win-crt-*":
            placeholders[pattern] = crt & symbols
    return placeholders


def x86_manifest(runtime_dir=RUNTIME):
    """PE32 imports the runtime can really call (win_x86.c table); anything else is unresolved for 32-bit images."""
    src = (Path(runtime_dir) / "win_x86.c").read_text(encoding="utf-8")
    k32 = set(re.findall(r"\bKL?\((\w+),", src))
    crt = set(re.findall(r"\bCF?\((\w+)[,)]", src))
    out = {"kernel32.dll": k32, "msvcrt.dll": crt}
    for dll, name in re.findall(r'\bD\("([^"]+)",\s*(\w+),', src): out.setdefault(dll, set()).add(name)
    extra = Path(runtime_dir) / "win_x86_dll.c"   # remaining system DLLs: R/U (named) and UO (ordinal / decorated) entries
    if extra.exists():
        t = extra.read_text(encoding="utf-8")
        for dll, name in re.findall(r'\b[RU]\("([^"]+)",\s*(\w+),', t): out.setdefault(dll, set()).add(name)
        for dll, name in re.findall(r'\bUO\("([^"]+)",\s*"([^"]+)"', t): out.setdefault(dll, set()).add(name)
    return out


def pe_architecture(path):
    try:
        pe = pefile.PE(str(path), fast_load=True)
    except (OSError, pefile.PEFormatError):
        return {"machine": 0x8664, "magic": 0x20B, "name": "unknown (unreadable; assuming x86-64)", "pe32": False, "translatable": False}
    machine, magic = pe.FILE_HEADER.Machine, pe.OPTIONAL_HEADER.Magic
    name = {0x14C: "x86 (i386)", 0x8664: "x86-64", 0x1C4: "ARM Thumb-2", 0xAA64: "ARM64"}.get(machine, "machine 0x%x" % machine)
    return {"machine": machine, "magic": magic, "name": name, "pe32": magic == 0x10B,
            "translatable": (magic, machine) in ((0x10B, 0x14C), (0x20B, 0x8664))}


def is_runtime_module(dll, manifest):
    return bool(_implemented_symbols(dll, manifest))


def _implemented_symbols(dll, manifest):
    if dll in manifest:
        return manifest[dll]
    for pattern, symbols in manifest.items():
        if pattern.endswith("*") and dll.startswith(pattern[:-1]):
            return symbols
    return ()


class DependencyInspector:
    def __init__(self, executable, search_dirs=(), recursive=True, manifest=None,
                 scan_system_imports=False, runtime_dir=RUNTIME):
        self.executable = Path(executable).resolve()
        self.search_dirs = [Path(p).resolve() for p in search_dirs]
        self.recursive = recursive
        self.scan_system_imports = scan_system_imports
        self.runtime_dir = Path(runtime_dir)
        self.arch = pe_architecture(self.executable)
        if self.arch["pe32"]:   # 32-bit: only the ABI-verified x86 table counts as implemented; no placeholders exist
            self.manifest = manifest or x86_manifest(self.runtime_dir)
            self.placeholders = {}
        else:
            self.manifest = manifest or runtime_manifest(self.runtime_dir)
            self.placeholders = runtime_placeholder_manifest(
                runtime_dir=self.runtime_dir, manifest=self.manifest
            )
        self.nodes = {}
        self.imports = []
        self.missing_files = set()
        self.unresolved_symbols = []
        self.forwarded_exports = []
        self.system_modules_exports_only = set()
        self._forwarder_visiting = set()
        self._root_key = str(self.executable).casefold()

    def _search_directories(self, parent):
        candidates = [parent, *self.search_dirs]
        system_root = os.environ.get("SystemRoot") or os.environ.get("WINDIR")
        if system_root:
            candidates.extend(
                [
                    Path(system_root) / "System32",
                    Path(system_root) / "SysWOW64",
                    Path(system_root),
                ]
            )
        seen = set()
        for candidate in candidates:
            key = str(candidate).casefold()
            if key not in seen:
                seen.add(key)
                yield candidate

    def _find_dll(self, name, parent):
        basename = canonical_dll(name)
        for directory in self._search_directories(parent):
            candidate = directory / basename
            if candidate.is_file():
                return candidate.resolve()
            if os.name != "nt" and directory.is_dir():
                for entry in directory.iterdir():
                    if entry.name.casefold() == basename:
                        return entry.resolve()
        return None

    @staticmethod
    def _read_pe(path):
        pe = pefile.PE(str(path), fast_load=True)
        directories = [
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"],
        ]
        pe.parse_data_directories(directories=directories)
        return pe

    @staticmethod
    def _import_entries(pe):
        for attr, is_delay in (
            ("DIRECTORY_ENTRY_IMPORT", False),
            ("DIRECTORY_ENTRY_DELAY_IMPORT", True),
        ):
            for descriptor in getattr(pe, attr, ()):
                dll = descriptor.dll.decode("ascii", "replace")
                for item in descriptor.imports:
                    name = (
                        item.name.decode("ascii", "replace")
                        if item.name is not None
                        else f"#{item.ordinal}"
                    )
                    if getattr(getattr(pe, "OPTIONAL_HEADER", None), "Magic", 0) == 0x10B:   # stdcall-decorated names (_Foo@8 / Foo@8) -> Foo
                        name = re.sub(r"@\d+$", "", name)
                    yield dll, name, is_delay

    @staticmethod
    def _export_map(pe):
        result = {}
        for item in getattr(getattr(pe, "DIRECTORY_ENTRY_EXPORT", None), "symbols", ()):
            if item.name is not None:
                result[item.name.decode("ascii", "replace")] = item
            result[f"#{item.ordinal}"] = item
        return result

    def _visit(self, path):
        path = Path(path).resolve()
        key = str(path).casefold()
        if key in self.nodes:
            return self.nodes[key]
        pe = self._read_pe(path)
        node = {"path": str(path), "pe": pe, "exports": self._export_map(pe)}
        self.nodes[key] = node
        if (
            key != self._root_key
            and not self.scan_system_imports
            and self._is_system_path(path)
        ):
            self.system_modules_exports_only.add(str(path))
            return node
        for dll, symbol, is_delay in self._import_entries(pe):
            dependency = self._find_dll(dll, path.parent)
            entry = {
                "imported_by": str(path),
                "dll": canonical_dll(dll),
                "symbol": symbol,
                "delay_load": is_delay,
                "dependency_path": str(dependency) if dependency else None,
                "runtime_implemented": self._is_implemented(dll, symbol),
                "runtime_placeholder": self._is_placeholder(dll, symbol),
            }
            self.imports.append(entry)
            if dependency is None:
                self.missing_files.add(canonical_dll(dll))
            else:
                dependency_node = self._visit(dependency) if self.recursive else None
                if dependency_node:
                    self._resolve_forwarders(dependency_node, dll, symbol)
                    if symbol not in dependency_node["exports"]:
                        self.unresolved_symbols.append(
                            {
                                "dll": canonical_dll(dll),
                                "symbol": symbol,
                                "reason": "not exported by inspected dependency",
                                "path": str(dependency),
                            }
                        )
        return node

    @staticmethod
    def _is_system_path(path):
        system_root = os.environ.get("SystemRoot") or os.environ.get("WINDIR")
        if not system_root:
            return False
        try:
            Path(path).relative_to(Path(system_root).resolve())
            return True
        except ValueError:
            return False

    def _is_implemented(self, dll, symbol):
        name = canonical_dll(dll)
        return symbol in _implemented_symbols(name, self.manifest)

    def _is_placeholder(self, dll, symbol):
        return symbol in _implemented_symbols(
            canonical_dll(dll), self.placeholders
        )

    def _resolve_forwarders(self, source_node, dll, symbol):
        export = source_node["exports"].get(symbol)
        if export is None or not export.forwarder:
            return
        forwarded = export.forwarder.decode("ascii", "replace")
        module, separator, target_symbol = forwarded.rpartition(".")
        if not separator:
            self.unresolved_symbols.append(
                {"dll": canonical_dll(dll), "symbol": symbol,
                 "reason": f"invalid forwarder {forwarded}"}
            )
            return
        target_dll = canonical_dll(module)
        if not target_dll.endswith(".dll"):
            target_dll += ".dll"
        target = self._find_dll(target_dll, Path(source_node["path"]).parent)
        edge = {
            "from": f"{canonical_dll(dll)}!{symbol}",
            "to": f"{target_dll}!{target_symbol}",
        }
        if edge not in self.forwarded_exports:
            self.forwarded_exports.append(edge)
        visit_key = (str(target).casefold() if target else target_dll, target_symbol)
        if visit_key in self._forwarder_visiting:
            return
        self._forwarder_visiting.add(visit_key)
        if target is None:
            self.unresolved_symbols.append(
                {"dll": target_dll, "symbol": target_symbol,
                 "reason": "forwarder target DLL not found"}
            )
            return
        target_node = self._visit(target)
        if target_symbol not in target_node["exports"]:
            self.unresolved_symbols.append(
                {"dll": target_dll, "symbol": target_symbol,
                 "reason": "forwarder target symbol not exported",
                 "path": str(target)}
            )
            return
        self._resolve_forwarders(target_node, target_dll, target_symbol)

    def inspect(self):
        self._visit(self.executable)
        grouped = defaultdict(list)
        missing_functions = []
        placeholder_functions = []
        missing_dlls = set()
        for item in self.imports:
            grouped[item["dll"]].append(item["symbol"])
            if not is_runtime_module(item["dll"], self.manifest):
                missing_dlls.add(item["dll"])
            elif not item["runtime_implemented"]:
                missing_functions.append(
                    {"dll": item["dll"], "symbol": item["symbol"],
                     "imported_by": item["imported_by"]}
                )
            elif item["runtime_placeholder"]:
                placeholder_functions.append(
                    {"dll": item["dll"], "symbol": item["symbol"],
                     "imported_by": item["imported_by"]}
                )
        imported_modules = sorted(grouped)
        covered = sorted(
            dll for dll in imported_modules if is_runtime_module(dll, self.manifest)
        )
        result = {
            "executable": str(self.executable),
            "architecture": self.arch,
            "recursive": self.recursive,
            "scan_system_imports": self.scan_system_imports,
            "inspected_images": sorted(node["path"] for node in self.nodes.values()),
            "system_modules_exports_only": sorted(self.system_modules_exports_only),
            "implemented_dlls": sorted(self.manifest),
            "imported_dlls": imported_modules,
            "implemented_imported_dlls": covered,
            "missing_dlls": sorted(missing_dlls),
            "missing_functions": sorted(
                missing_functions, key=lambda x: (x["dll"], x["symbol"])
            ),
            "placeholder_functions": sorted(
                placeholder_functions, key=lambda x: (x["dll"], x["symbol"])
            ),
            "unresolved_symbols": self.unresolved_symbols,
            "missing_dependency_files": sorted(self.missing_files),
            "forwarded_exports": self.forwarded_exports,
            "imports": self.imports,
            "dynamic_resolution": {
                "note": "Static PE inspection cannot know runtime LoadLibrary/GetProcAddress values; enable AOT_VERBOSE to log attempts.",
                "events": [],
            },
        }
        return result


def markdown_report(report):
    lines = [
        f"# PE dependency report: `{Path(report['executable']).name}`",
        "",
        f"- Recursive inspection: {'yes' if report['recursive'] else 'no'}",
        f"- Images inspected: {len(report['inspected_images'])}",
        f"- Full host system import scan: {'yes' if report['scan_system_imports'] else 'no'}",
        f"- System DLLs inspected for exports/forwarders only: {len(report['system_modules_exports_only'])}",
        "",
        "## Runtime-implemented DLLs",
        "",
        *[f"- `{dll}`" for dll in report["implemented_dlls"]],
        "",
        "## Imported DLL coverage",
        "",
        *[f"- `{dll}`: {'implemented' if dll in report['implemented_imported_dlls'] else 'missing runtime module'}"
          for dll in report["imported_dlls"]],
        "",
        "## Missing DLLs",
        "",
    ]
    lines.extend(f"- `{dll}`" for dll in report["missing_dlls"])
    if not report["missing_dlls"]:
        lines.append("- None")
    lines.extend(["", "## Missing functions", ""])
    lines.extend(
        f"- `{item['dll']}!{item['symbol']}` (imported by `{Path(item['imported_by']).name}`)"
        for item in report["missing_functions"]
    )
    if not report["missing_functions"]:
        lines.append("- None")
    lines.extend(["", "## Placeholder shims", ""])
    lines.extend(
        f"- `{item['dll']}!{item['symbol']}` (mapped to a placeholder handler; not full API behavior)"
        for item in report["placeholder_functions"]
    )
    if not report["placeholder_functions"]:
        lines.append("- None encountered")
    lines.extend(["", "## Unresolved symbols / dependency files", ""])
    lines.extend(
        f"- `{item['dll']}!{item['symbol']}`: {item['reason']}"
        for item in report["unresolved_symbols"]
    )
    lines.extend(
        f"- DLL file not found: `{dll}`" for dll in report["missing_dependency_files"]
    )
    if not report["unresolved_symbols"] and not report["missing_dependency_files"]:
        lines.append("- None")
    lines.extend(["", "## Forwarded exports", ""])
    lines.extend(f"- `{item['from']}` → `{item['to']}`" for item in report["forwarded_exports"])
    if not report["forwarded_exports"]:
        lines.append("- None encountered")
    lines.extend(["", "## Runtime dynamic-resolution events", ""])
    lines.extend(
        f"- **{event['status']}** `{event['operation']}`: `{event['target']}`"
        for event in report["dynamic_resolution"]["events"]
    )
    if not report["dynamic_resolution"]["events"]:
        lines.append("- No runtime log supplied or no dynamic calls were recorded")
    lines.extend(
        [
            "",
            "## Imported symbols",
            "",
            "| DLL | Symbol | Delay-loaded | Runtime status | Dependency file |",
            "|---|---|---:|---|---|",
        ]
    )
    for item in report["imports"]:
        status = "implemented" if item["runtime_implemented"] else "missing"
        path = item["dependency_path"] or "not found"
        lines.append(
            f"| `{item['dll']}` | `{item['symbol']}` | "
            f"{'yes' if item['delay_load'] else 'no'} | {status} | `{path}` |"
        )
    lines.extend(["", f"> {report['dynamic_resolution']['note']}", ""])
    return "\n".join(lines)


def parse_runtime_log(path):
    events = []
    patterns = (
        (
            re.compile(r"\[aot\] dynamic LoadLibrary(?:Ex)?[AW] (resolved|unresolved): (.*)$"),
            "LoadLibrary",
            False,
        ),
        (
            re.compile(r"\[aot\] dynamic GetProcAddress (resolved|unresolved): (.*)$"),
            "GetProcAddress",
            False,
        ),
        (
            re.compile(r"\[aot\] dynamic GetProcAddress uses placeholder shim: (.*)$"),
            "GetProcAddress",
            True,
        ),
    )
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        for pattern, operation, is_placeholder in patterns:
            match = pattern.search(line)
            if match:
                events.append(
                    {
                        "operation": operation,
                        "status": "placeholder" if is_placeholder else match.group(1),
                        "target": (match.group(1) if is_placeholder else match.group(2)).strip(),
                    }
                )
                break
    return events


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--search-dir", action="append", default=[], type=Path)
    parser.add_argument("--no-recursive", action="store_true")
    parser.add_argument(
        "--scan-system-imports",
        action="store_true",
        help="also recurse through the host OS DLL import graph",
    )
    parser.add_argument("--runtime-log", type=Path)
    parser.add_argument("--json", type=Path, help="write JSON report")
    parser.add_argument("--markdown", type=Path, help="write Markdown report")
    args = parser.parse_args(argv)
    inspector = DependencyInspector(
        args.executable,
        args.search_dir,
        not args.no_recursive,
        scan_system_imports=args.scan_system_imports,
    )
    report = inspector.inspect()
    if args.runtime_log:
        report["dynamic_resolution"]["events"] = parse_runtime_log(args.runtime_log)
        report["dynamic_resolution"]["log"] = str(args.runtime_log.resolve())
    else:
        report["dynamic_resolution"]["events"] = []
    rendered = markdown_report(report)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2), encoding="utf-8")
    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(rendered, encoding="utf-8")
    if not args.json and not args.markdown:
        print(rendered)
    return 0


if __name__ == "__main__":
    sys.exit(main())
