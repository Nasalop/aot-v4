"""On-device PE translation entry point used by the Android activity."""

import json
import os
from pathlib import Path


def translate(executable, output_directory, runtime_directory, native_library_directory):
    os.environ["LIBCAPSTONE_PATH"] = str(native_library_directory)

    import pefile
    import aot_transpiler
    import pe_dependency_inspect

    executable = Path(executable).resolve()
    output_directory = Path(output_directory).resolve()
    runtime_directory = Path(runtime_directory).resolve()

    pe = pefile.PE(str(executable), fast_load=True)
    machine, magic = pe.FILE_HEADER.Machine, pe.OPTIONAL_HEADER.Magic
    if not ((machine == 0x8664 and magic == 0x20B) or (machine == 0x14C and magic == 0x10B)):
        raise ValueError("This translator supports x86-64 (PE32+) and 32-bit x86 (PE32) executables only.")

    rec = aot_transpiler.Rec(str(executable)).discover()
    instruction_count, block_count, region_count, unsupported = aot_transpiler.emit(
        rec, str(output_directory)
    )

    inspector = pe_dependency_inspect.DependencyInspector(
        executable,
        search_dirs=[executable.parent],
        runtime_dir=runtime_directory,
    )
    dependencies = inspector.inspect()
    dependency_json = output_directory / "dependency-report.json"
    dependency_json.write_text(json.dumps(dependencies, indent=2), encoding="utf-8")
    dependency_markdown = output_directory / "dependency-report.md"
    dependency_markdown.write_text(
        pe_dependency_inspect.markdown_report(dependencies), encoding="utf-8"
    )

    return json.dumps(
        {
            "executable": str(executable),
            "output_directory": str(output_directory),
            "instruction_count": instruction_count,
            "block_count": block_count,
            "region_count": region_count,
            "unsupported_instructions": [
                {"mnemonic": mnemonic, "count": count}
                for mnemonic, count in unsupported.most_common()
            ],
            "dependency_report": str(dependency_json),
            "missing_dlls": dependencies["missing_dlls"],
            "missing_functions": dependencies["missing_functions"],
            "unresolved_symbols": dependencies["unresolved_symbols"],
        }
    )
