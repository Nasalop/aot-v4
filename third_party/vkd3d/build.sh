#!/usr/bin/env bash
# build.sh <outdir>: build vkd3d-shader (Wine's bundled copy: HLSL -> DXBC compiler) into libvkd3dshader.so for arm64 Android.
set -euo pipefail
OUT=$(realpath -m "$1"); H=$(dirname "$(readlink -f "$0")"); W=$(mktemp -d); COMMIT=$(cat "$H/WINE_COMMIT")
sudo apt-get install -y flex bison >/dev/null
mkdir -p "$W/wine" && cd "$W/wine" && git init -q && git remote add o https://github.com/wine-mirror/wine.git
git sparse-checkout init --cone && git sparse-checkout set libs/vkd3d
git fetch -q --depth 1 --filter=blob:none o "$COMMIT" && git checkout -q FETCH_HEAD
V=$PWD/libs/vkd3d; S=$V/libs/vkd3d-shader; G=$W/gen; mkdir -p "$G"
flex -o "$G/hlsl.yy.c" $S/hlsl.l; bison -d -o "$G/hlsl.tab.c" $S/hlsl.y 2>/dev/null
flex -o "$G/preproc.yy.c" $S/preproc.l; bison -d -o "$G/preproc.tab.c" $S/preproc.y 2>/dev/null
T=${ANDROID_NDK_LATEST_HOME:?}/toolchains/llvm/prebuilt/linux-x86_64/bin; CC=$T/aarch64-linux-android28-clang
CF="-O2 -w -fPIC -fvisibility=default -D_GNU_SOURCE -DLIBVKD3D_SOURCE -DLIBVKD3D_SHADER_SOURCE -DVKD3D_NO_WIN32_TYPES -DVKD3D_NO_VULKAN_H -DHAVE_ATOMIC_EXCHANGE_N=1 -DHAVE_BUILTIN_ADD_OVERFLOW=1 -DHAVE_BUILTIN_CLZ=1 -DHAVE_BUILTIN_CTZ=1 -DHAVE_SYNC_ADD_AND_FETCH=1 -DHAVE_SYNC_BOOL_COMPARE_AND_SWAP=1 -fno-strict-aliasing -I$G -I$V -I$V/include -I$V/include/private -I$S -I$H/shim -include $H/shim/compat.h"
objs=(); for f in $V/libs/vkd3d-common/{debug,error,memory,utf8}.c $S/{checksum,d3d_asm,d3dbc,dxbc,dxil,fx,glsl,hlsl,hlsl_codegen,hlsl_constant_ops,ir,msl,spirv,tpf,vkd3d_shader_main}.c $G/{hlsl.yy,hlsl.tab,preproc.yy,preproc.tab}.c; do
  o=$G/$(basename "$f" .c).o; $CC $CF -c "$f" -o "$o"; objs+=("$o"); done
mkdir -p "$OUT"; $CC -shared -o "$OUT/libvkd3dshader.so" "${objs[@]}" -lm -ldl
"$T/llvm-strip" --strip-unneeded "$OUT/libvkd3dshader.so"; ls -la "$OUT/libvkd3dshader.so"
