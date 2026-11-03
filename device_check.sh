#!/bin/bash
# Verifies the recompiler works on THIS device (ARM64): transpile + compile + run the bundled test exes.
set -e; cd "$(dirname "$0")"
echo "arch: $(uname -m)"; ok=1
./aot.sh tests/t1.exe t1 >/dev/null; ./output/t1 tests/t1.exe > output/t1.out || true
grep -q "div:-1234,-567,18446744073708317" output/t1.out && echo "t1 PASS" || { echo "t1 FAIL"; ok=0; }
for O in O0 O2; do ./aot.sh tests/t2$O.exe t2$O >/dev/null; ./output/t2$O tests/t2$O.exe > output/t2$O.out || true
  diff -q output/t2$O.out tests/t2.expected >/dev/null && echo "t2$O PASS" || { echo "t2$O FAIL"; ok=0; }; done
./aot.sh tests/t3.exe t3 >/dev/null; ./output/t3 tests/t3.exe 2>&1 | grep -q 'd3d11 test: 120 frames' && echo 't3 (D3D11 clear/present) PASS' || { echo 't3 FAIL'; ok=0; }
gcc -w -Isrc/runtime tests/gfx_test.c src/runtime/gfx.c -lm -o output/gfxt 2>/dev/null || cc -w -Isrc/runtime tests/gfx_test.c src/runtime/gfx.c -lm -o output/gfxt
./output/gfxt && echo "gfx PASS"
[ $ok = 1 ] && echo "ALL GOOD - send me this output" || echo "FAILURES - send me this output"
