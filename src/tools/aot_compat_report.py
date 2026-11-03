#!/usr/bin/env python3
"""Compatibility report for a Windows exe: architecture, translation (unsupported instructions), imports, compile status.
usage: aot_compat_report.py game.exe [--out-dir DIR] [--search-dir D]... [--compile]
Never claims a game runs: 'boots' is only reported by actually executing it (--run-seconds N)."""
import argparse, json, math, subprocess, sys, tempfile, shutil, os
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src" / "tools")); sys.path.insert(0, str(ROOT / "src" / "transpiler"))
import pefile
from pe_dependency_inspect import DependencyInspector, pe_architecture

PACKER_SECTIONS = {".bind": "SecuROM/DRM-style", ".securom": "SecuROM", ".vmp0": "VMProtect", ".vmp1": "VMProtect", ".themida": "Themida",
                   ".winlice": "WinLicense", "upx0": "UPX", "upx1": "UPX", ".aspack": "ASPack", ".enigma1": "Enigma", ".arch": "StarForce?", ".sforce": "StarForce"}

def entropy(b):
    if not b: return 0.0
    n = len(b); c = [0] * 256
    for x in b: c[x] += 1
    return -sum(v / n * math.log2(v / n) for v in c if v)

def pe_facts(path):
    pe = pefile.PE(str(path)); oh = pe.OPTIONAL_HEADER; secs = []; flags = []
    ep = oh.AddressOfEntryPoint; ep_sec = None
    for s in pe.sections:
        nm = s.Name.rstrip(b"\0").decode("ascii", "replace"); en = entropy(s.get_data()[:1 << 20]); ex = bool(s.Characteristics & 0x20000000)
        secs.append({"name": nm, "rva": s.VirtualAddress, "vsize": s.Misc_VirtualSize, "rawsize": s.SizeOfRawData, "exec": ex, "entropy": round(en, 2)})
        if s.VirtualAddress <= ep < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData): ep_sec = nm
        if nm.lower() in PACKER_SECTIONS: flags.append("section %s -> %s" % (nm, PACKER_SECTIONS[nm.lower()]))
        if ex and en > 7.2 and s.SizeOfRawData > 0x1000: flags.append("executable section %s entropy %.2f (packed/encrypted?)" % (nm, en))
    if ep_sec is None: flags.append("entry point outside all sections")
    elif ep_sec not in (".text", "text", "CODE", ".code"): flags.append("entry point in non-.text section %s" % ep_sec)
    if hasattr(pe, "OPTIONAL_HEADER") and oh.DATA_DIRECTORY[14].VirtualAddress: flags.append(".NET/CLR image (not native x86)")
    return {"image_base": hex(oh.ImageBase), "entry_rva": hex(ep), "entry_section": ep_sec, "size_of_image": oh.SizeOfImage, "subsystem": oh.Subsystem,
            "sections": secs, "protection_hints": flags}

def translate(path, outdir):
    import aot_transpiler as T
    r = {"status": "not attempted"}
    try:
        rec = T.Rec(str(path)).discover()
        ni, nb, nr, un = T.emit(rec, str(outdir))
        r = {"status": "ok" if not un else "ok-with-traps", "instructions": ni, "blocks": nb, "regions": nr,
             "unsupported_instruction_traps": dict(un), "unsupported_total": sum(un.values())}
    except SystemExit as e: r = {"status": "failed", "error": str(e)}
    except Exception as e: r = {"status": "failed", "error": "%s: %s" % (type(e).__name__, e)}
    return r

def compile_status(path):
    cc = shutil.which(os.environ.get("CC", "gcc"))
    if not cc: return {"status": "skipped", "error": "no C compiler"}
    name = "compat_" + Path(path).stem
    p = subprocess.run(["bash", str(ROOT / "aot.sh"), str(path), name], capture_output=True, text=True, cwd=ROOT)
    tail = (p.stdout + p.stderr).strip().splitlines()[-6:]
    return {"status": "ok" if p.returncode == 0 else "failed", "binary": str(ROOT / "output" / name) if p.returncode == 0 else None, "log_tail": tail}

def markdown(R):
    a, t, d = R["architecture"], R["translation"], R["dependencies"]; L = []
    L += ["# Compatibility report: `%s`" % Path(R["executable"]).name, "", "## Architecture", "",
          "- Machine: **%s** (%s)" % (a["name"], "PE32" if a["pe32"] else "PE32+"), "- Translatable by this tool: **%s**" % ("yes" if a["translatable"] else "NO"),
          "- ImageBase %s, entry RVA %s (section %s), SizeOfImage 0x%x" % (R["facts"]["image_base"], R["facts"]["entry_rva"], R["facts"]["entry_section"], R["facts"]["size_of_image"]), "",
          "### Protection / packing hints", ""] + (["- " + h for h in R["facts"]["protection_hints"]] or ["- none detected"])
    L += ["", "## Translation", "", "- Status: **%s**" % t["status"]]
    if "error" in t: L.append("- Error: %s" % t["error"])
    if "instructions" in t: L.append("- %d instructions, %d blocks, %d regions" % (t["instructions"], t["blocks"], t["regions"]))
    L += ["", "### Unsupported instructions (become runtime traps)", ""] + (["- `%s` x%d" % kv for kv in sorted(t.get("unsupported_instruction_traps", {}).items(), key=lambda x: -x[1])] or ["- none"])
    L += ["", "## Imports", "", "- Imported DLLs: %d, symbols: %d" % (len(d["imported_dlls"]), len(d["imports"])),
          "- Missing DLLs (no runtime module): " + (", ".join("`%s`" % x for x in d["missing_dlls"]) or "none"), "", "### Unresolved APIs (will abort when called; never stubbed)", ""]
    L += ["- `%s!%s`" % (m["dll"], m["symbol"]) for m in d["missing_functions"]] or ["- none"]
    L += ["", "### Imports from DLLs with no runtime module (what a replacement layer must provide)", ""]
    for dll in d["missing_dlls"]: L += ["- `%s`: %s" % (dll, ", ".join(sorted({i["symbol"] for i in d["imports"] if i["dll"] == dll})))]
    if not d["missing_dlls"]: L.append("- none")
    L += ["", "### Resolved APIs", ""] + ["- `%s!%s`" % (i["dll"], i["symbol"]) for i in d["imports"] if i["runtime_implemented"]] or ["- none"]
    if d["placeholder_functions"]: L += ["", "### Placeholder-backed APIs (not full behaviour)", ""] + ["- `%s!%s`" % (m["dll"], m["symbol"]) for m in d["placeholder_functions"]]
    delay = [i for i in d["imports"] if i["delay_load"]]
    if delay and a["pe32"]: L += ["", "### Delay-load imports (NOT bound for PE32 yet)", ""] + ["- `%s!%s`" % (i["dll"], i["symbol"]) for i in delay]
    L += ["", "### Unresolved dependency files", ""] + (["- `%s`" % x for x in d["missing_dependency_files"] if x not in d["implemented_imported_dlls"]] or ["- none"])
    c = R["compile"]; L += ["", "## Compilation", "", "- Status: **%s**" % c["status"]] + (["  `%s`" % x for x in c.get("log_tail", [])] if c["status"] == "failed" else [])
    L += ["", "## Verdict", "", R["verdict"], ""]
    return "\n".join(L)

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument("exe", type=Path); ap.add_argument("--out-dir", type=Path)
    ap.add_argument("--search-dir", action="append", default=[], type=Path); ap.add_argument("--compile", action="store_true"); a = ap.parse_args(argv)
    out = a.out_dir or ROOT / "output" / "compat"; out.mkdir(parents=True, exist_ok=True)
    arch = pe_architecture(a.exe)
    R = {"executable": str(a.exe.resolve()), "architecture": arch, "facts": pe_facts(a.exe)}
    gen = Path(tempfile.mkdtemp(prefix="aot_compat_"))
    R["translation"] = translate(a.exe, gen) if arch["translatable"] else {"status": "not attempted", "error": "architecture %s not supported" % arch["name"]}
    shutil.rmtree(gen, ignore_errors=True)
    R["dependencies"] = DependencyInspector(a.exe, a.search_dir, recursive=False).inspect()
    R["compile"] = compile_status(a.exe) if a.compile and R["translation"]["status"].startswith("ok") else {"status": "not attempted"}
    d, t = R["dependencies"], R["translation"]; blockers = []
    if not arch["translatable"]: blockers.append("unsupported architecture")
    if t["status"] == "failed": blockers.append("translation failed")
    if t.get("unsupported_total"): blockers.append("%d unsupported instructions" % t["unsupported_total"])
    if d["missing_functions"] or d["missing_dlls"]: blockers.append("%d unresolved APIs, %d missing DLLs" % (len(d["missing_functions"]), len(d["missing_dlls"])))
    if R["facts"]["protection_hints"]: blockers.append("possible packing/DRM")
    R["verdict"] = ("No known static blockers; boot is still UNVERIFIED until it is run." if not blockers else "NOT known to run. Blockers: " + "; ".join(blockers) + ". Boot is UNVERIFIED.")
    (out / "compat.json").write_text(json.dumps(R, indent=2, default=str)); (out / "compat.md").write_text(markdown(R), encoding="utf-8")
    print(markdown(R)); return 0

if __name__ == "__main__": sys.exit(main())
