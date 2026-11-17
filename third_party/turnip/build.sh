#!/usr/bin/env bash
# build.sh <outdir>: stage the bundled Turnip (Mesa Freedreno Vulkan) drivers for the APK.
#   libturnip_a7xx.so   Turnip v27.0.0-r2 "Standard"  Adreno 6xx / 7xx (Snapdragon 8 Gen 3 and older)
#   libturnip_a8xx.so   Turnip v27.0.0-r2 "A8xx"      Adreno 810/812/825/829/830/840 (Snapdragon 8 Elite)
#
# These are the drivers' own prebuilt releases (Mesa main, short of a full Mesa tree and a KGSL kernel this build
# cannot compile them here), and the zips contain exactly the ICD this runtime loads: libvulkan_freedreno.so.
# Every download is checked against the SHA-256 published in that release's notes.
#
# A download or hash failure is NOT fatal: the APK then ships without that driver, and the in-game choice falls back
# to the system driver with one [gpu] line in the log. Nothing else in the build depends on these files.
set -uo pipefail
OUT=$(realpath -m "${1:?usage: build.sh <outdir>}")
TAG=v27.0.0-20261114-r2
BASE="https://github.com/The412Banner/Banners-Turnip/releases/download/$TAG"
mkdir -p "$OUT"

stage() {   # stage <zip> <sha256> <staged name> <what it is for>
  local zip=$1 sha=$2 dst=$3 what=$4 tmp src
  if [ -s "$OUT/$dst" ]; then echo "turnip: $dst already staged ($(du -h "$OUT/$dst" | cut -f1))"; return 0; fi
  tmp=$(mktemp -d)
  if ! curl -fsSL --retry 3 --connect-timeout 20 -o "$tmp/$zip" "$BASE/$zip"; then
    echo "WARNING: turnip: could not download $zip ($what); the APK ships without $dst"; rm -rf "$tmp"; return 1
  fi
  if ! echo "$sha  $tmp/$zip" | sha256sum -c - >/dev/null 2>&1; then
    echo "WARNING: turnip: $zip does not match the published SHA-256; not staging it"; rm -rf "$tmp"; return 1
  fi
  if ! ( cd "$tmp" && unzip -q -o "$zip" ); then
    echo "WARNING: turnip: $zip could not be unpacked"; rm -rf "$tmp"; return 1
  fi
  src=$(find "$tmp" -name 'libvulkan_freedreno.so' | head -1)
  if [ -z "$src" ]; then echo "WARNING: turnip: $zip has no libvulkan_freedreno.so"; rm -rf "$tmp"; return 1; fi
  cp -L "$src" "$OUT/$dst.part" && mv "$OUT/$dst.part" "$OUT/$dst"
  rm -rf "$tmp"
  [ -s "$OUT/$dst" ] || { echo "WARNING: turnip: could not write $OUT/$dst"; return 1; }
  echo "turnip: staged $dst ($(du -h "$OUT/$dst" | cut -f1)) from $zip - $what"
}

stage "Turnip-$TAG.zip"      "40f5becf92911aea9b18e1ecb9e1d4e790ebb1dde6d5a863f6a9c1d2d083bb8f" libturnip_a7xx.so "Adreno 6xx/7xx" || true
stage "Turnip-$TAG-A8xx.zip" "13485ce18af8b861efcbf3d25560b15285c2298f460e0d3b7c32abbfd1f17f7a" libturnip_a8xx.so "Adreno 8xx"     || true

# Optional, same one line each, if you ever want them on the menu:
#   Adreno 710/720/722 (real-hardware registers): Turnip-$TAG-710-720-Test.zip  1520e89d43f24fe2054d1cdd8fdf54dcc58d62fb5b004abf06b052b9d6b6eca9
#   Adreno 740 with One UI flicker:              Turnip-$TAG-8G2-OneUI.zip     5817969aaabee2ae7b5301c05ac36dace942c735e15691fa963cbdaeb82d395d
ls -la "$OUT" 2>/dev/null
exit 0
