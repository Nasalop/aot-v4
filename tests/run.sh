#!/bin/bash
# Regression test: builds Windows test exes with clang+lld-link (no Windows SDK needed), recompiles them, compares output to a native build.
set -e; cd "$(dirname "$0")"
cc -std=c11 -Wall -Wextra -Werror -I../src/runtime gfx_test.c ../src/runtime/gfx.c -o gfx_test
./gfx_test; rm -f gfx_test
cc -std=c11 -Wall -Wextra -Werror -I../src/runtime com_test.c ../src/runtime/gfx.c -o com_test
./com_test; rm -f com_test
cc -std=c11 -Wall -Wextra -Werror -pthread -I../src/runtime win_file_find_test.c ../src/runtime/win_file_async.c -o win_file_find_test
./win_file_find_test; rm -f win_file_find_test
cc -std=c11 -Wall -Wextra -Werror -pthread -I../src/runtime win_audio_test.c -o win_audio_test
./win_audio_test; rm -f win_audio_test
if ! command -v clang >/dev/null || ! command -v lld-link >/dev/null; then
  echo "SKIP: tests/run.sh requires clang and lld-link" >&2
  exit 0
fi
F="--target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -fno-builtin -mno-avx"
L="lld-link /subsystem:console /entry:mainCRTStartup /nodefaultlib /fixed /base:0x140000000"
lld-link /lib /def:k32.def /machine:x64 /out:kernel32.lib; lld-link /lib /def:crt.def /machine:x64 /out:msvcrt.lib
clang $F -O1 -c t1.c -o t1.obj && $L /out:t1.exe t1.obj kernel32.lib
gcc -O1 -w t2.c -o t2native -lm && ./t2native > t2.expected || true
for O in O0 O2; do clang $F -$O -c t2.c -o t2$O.obj && $L /out:t2$O.exe t2$O.obj msvcrt.lib; done
cd ..
./aot.sh tests/t1.exe t1 >/dev/null && ./output/t1 tests/t1.exe | head -3; 
for O in O0 O2; do ./aot.sh tests/t2$O.exe t2$O >/dev/null; ./output/t2$O tests/t2$O.exe > output/t2$O.out || true; diff -q output/t2$O.out tests/t2.expected && echo "t2$O: PASS"; done
