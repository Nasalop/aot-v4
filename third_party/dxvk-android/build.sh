#!/usr/bin/env bash
# build.sh <outdir>: cross-build DXVK Native (DXGI + D3D11 + D3D10 core) for arm64 Android, fix sonames so the APK can dlopen them by plain name.
set -euo pipefail
OUT=$(realpath -m "$1"); H=$(dirname "$(readlink -f "$0")"); W=$(mktemp -d)
git clone --depth 1 --branch v3.1.1 https://github.com/doitsujin/dxvk.git "$W/dxvk"
cd "$W/dxvk" && git submodule update --init --recursive --depth 1 && bash "$H/patch.sh" .
pip install --quiet meson patchelf 2>/dev/null || pip install --quiet --break-system-packages meson patchelf
sudo apt-get install -y glslang-tools >/dev/null
T=${ANDROID_NDK_LATEST_HOME:?}/toolchains/llvm/prebuilt/linux-x86_64/bin
cat > "$W/cross.txt" <<EOF
[binaries]
c = '$T/aarch64-linux-android28-clang'
cpp = '$T/aarch64-linux-android28-clang++'
ar = '$T/llvm-ar'
strip = '$T/llvm-strip'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
[built-in options]
cpp_args = ['-Wno-error']
EOF
meson setup --cross-file "$W/cross.txt" --buildtype release -Denable_d3d8=false -Denable_d3d9=false build >/dev/null
ninja -C build src/dxgi/libdxvk_dxgi.so src/d3d11/libdxvk_d3d11.so src/d3d10/libdxvk_d3d10core.so
mkdir -p "$OUT"
cp build/src/dxgi/libdxvk_dxgi.so build/src/d3d11/libdxvk_d3d11.so build/src/d3d10/libdxvk_d3d10core.so "$OUT/"
cd "$OUT"
for l in dxgi d3d11 d3d10core; do
  patchelf --set-soname libdxvk_$l.so libdxvk_$l.so
  for d in dxgi d3d11; do patchelf --replace-needed libdxvk_$d.so.0 libdxvk_$d.so libdxvk_$l.so 2>/dev/null || true; done
  "$T/llvm-strip" --strip-unneeded libdxvk_$l.so
done
ls -la "$OUT"
