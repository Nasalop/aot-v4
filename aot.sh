#!/bin/bash
# usage: ./aot.sh game.exe [outname]   -> builds output/<outname> (run it as: output/<outname> game.exe [args])
# env: CC (default gcc; for Android use the NDK aarch64 clang), PY (python), JOBS
set -euo pipefail
cd "$(dirname "$0")"
EXE=${1:?usage: aot.sh game.exe [outname]}; NAME=${2:-$(basename "${EXE%.*}")}
CC=${CC:-gcc}; PY=${PY:-python3}; W=output/$NAME.gen
command -v "$PY" >/dev/null || { echo "error: Python interpreter not found: $PY" >&2; exit 127; }
command -v "$CC" >/dev/null || { echo "error: C compiler not found: $CC" >&2; exit 127; }
rm -rf "$W"; mkdir -p "$W"
"$PY" src/transpiler/aot_transpiler.py "$EXE" "$W"
find "$W" -maxdepth 1 -name '*.c' -print0 | xargs -0 -r -P "${JOBS:-$(nproc)}" -I{} "$CC" -O1 -w -fno-strict-aliasing -fwrapv -Isrc/runtime -Ithird_party/vkd3d/include -c {} -o {}.o
"$CC" -O2 -w -fno-strict-aliasing -Isrc/runtime src/runtime/runtime.c src/runtime/win_resolver.c src/runtime/shims.c src/runtime/win_compat.c src/runtime/win_file.c src/runtime/win_file_async.c src/runtime/win_audio.c src/runtime/win_d3d10.c src/runtime/win_sys.c src/runtime/win_x86.c src/runtime/win_x86_dll.c src/runtime/win_input.c src/runtime/win_gfx.c src/runtime/dxvk_proxy.c src/runtime/hlsl_compile.c src/runtime/gfx_backend.c src/runtime/gfx_draw_batch.c src/runtime/dxbc_translate_stub.c "$W"/*.o -lm -pthread -ldl ${AOT_GFX_LIBS:-} -o "output/$NAME"
echo "[+] built output/$NAME  -> run: output/$NAME $EXE"
