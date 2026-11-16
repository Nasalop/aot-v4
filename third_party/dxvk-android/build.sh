#!/usr/bin/env bash
# build.sh <outdir>: cross-build DXVK Native (DXGI + D3D11 + D3D10 core) for arm64 Android in three tiers and fix sonames so the APK can dlopen them by plain name.
#   tier 0  DXVK 3.1.1           libdxvk_{dxgi,d3d11,d3d10core}.so
#   tier 1  DXVK 2.4.1           libdxvk_{dxgi,d3d11,d3d10core}_t1.so
#   tier 2  DXVK native 1.9.2    libdxvk_{dxgi,d3d11,d3d10core}_t2.so   (the newest native 1.x line: upstream 1.10.x has no Linux/native build)
# The runtime (dxvk_proxy.c) probes the tiers in this order on the phone's own Vulkan driver and uses the first that works.
# Tier 0 is required. Tiers 1 and 2 are best effort: if one fails to build the APK simply ships without it and the runtime skips it.
set -euo pipefail
OUT=$(realpath -m "$1"); H=$(dirname "$(readlink -f "$0")"); W=$(mktemp -d)
pip install --quiet meson patchelf 2>/dev/null || pip install --quiet --break-system-packages meson patchelf
sudo apt-get install -y glslang-tools >/dev/null
T=${ANDROID_NDK_LATEST_HOME:?}/toolchains/llvm/prebuilt/linux-x86_64/bin
cat > "$W/cross.txt" <<EOT
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
EOT
mkdir -p "$OUT"
# build_tier <suffix> <git url> <git tag> <patch script> <meson options...>
build_tier() {
  local sfx=$1 url=$2 tag=$3 patch=$4; shift 4
  local S="$W/src$sfx"
  git clone --depth 1 --branch "$tag" "$url" "$S" || return 1
  ( cd "$S" && git submodule update --init --recursive --depth 1 && bash "$H/$patch" . \
    && meson setup --cross-file "$W/cross.txt" --buildtype release "$@" build >/dev/null && ninja -C build ) || return 1
  local l f
  for l in dxgi d3d11 d3d10core; do
    f=$(find "$S/build/src" -maxdepth 3 -name "*${l}*.so" -not -name "*.so.*" | head -1)
    [ -n "$f" ] || f=$(find "$S/build/src" -maxdepth 3 -name "*${l}*.so*" | head -1)
    [ -n "$f" ] || { echo "tier$sfx: no $l library produced"; return 1; }
    cp -L "$f" "$OUT/libdxvk_${l}${sfx}.so"
  done
  ( cd "$OUT"
    for l in dxgi d3d11 d3d10core; do
      patchelf --set-soname libdxvk_${l}${sfx}.so libdxvk_${l}${sfx}.so
      for n in $(patchelf --print-needed libdxvk_${l}${sfx}.so); do
        for d in dxgi d3d11; do case "$n" in *${d}.so*|${d}.so*) patchelf --replace-needed "$n" libdxvk_${d}${sfx}.so libdxvk_${l}${sfx}.so ;; esac; done
      done
      "$T/llvm-strip" --strip-unneeded libdxvk_${l}${sfx}.so
    done )
}
# Tier 0 keeps the original behaviour exactly (same options, same names): a failure here fails the build.
build_tier "" https://github.com/doitsujin/dxvk.git v3.1.1 patch.sh -Denable_d3d8=false -Denable_d3d9=false
build_tier _t1 https://github.com/doitsujin/dxvk.git v2.4.1 patch_241.sh -Denable_d3d8=false -Denable_d3d9=false || { echo "WARNING: DXVK 2.4.1 tier failed to build; shipping without it"; rm -f "$OUT"/libdxvk_*_t1.so; }
build_tier _t2 https://github.com/Joshua-Ashton/dxvk-native.git native-1.9.2 patch_19.sh -Denable_d3d9=false || { echo "WARNING: DXVK 1.9.2 tier failed to build; shipping without it"; rm -f "$OUT"/libdxvk_*_t2.so; }
ls -la "$OUT"
