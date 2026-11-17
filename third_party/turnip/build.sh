#!/usr/bin/env bash
# build.sh <outdir>: stage the bundled Turnip (Mesa Freedreno Vulkan) drivers for the APK.
#   libturnip_a7xx.so   Turnip v27.0.0-R2        Adreno 6xx / 7xx (Snapdragon 8 Gen 3 and older)
#   libturnip_a8xx.so   Turnip Gen8 V39          Adreno 810/812/825/829/830/840 (Snapdragon 8 Elite)
#
# STATUS: loaded at run time by dxvk_proxy.c (turnip_setup) through libadrenotools; the hook libraries come from the app's CMake build.
#
# Source: StevenMXZ/Adreno-Tools-Drivers release v39 ("Turnip Gen8 V39 / Turnip v27.0.0-R2", Mesa build, Vulkan 1.4.367,
# 2026-11-07). Both zips are straight from that release and contain exactly the ICD the runtime loads:
# libvulkan_freedreno.so (8.36 MB / 8.37 MB) plus meta.json. Every download is checked against the SHA-256 that release
# publishes for the file.
#
# A download or hash failure is NOT fatal: the APK simply ships without that file. Nothing else depends on them.
set -uo pipefail
OUT=$(realpath -m "${1:?usage: build.sh <outdir>}")
TAG=v39
BASE="https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/download/$TAG"
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

stage "Turnip_v27.0.0-R2.zip" "8d30be022582d0c03b1918c019540491d00439e14e9e5e1359fe0bb19004f781" libturnip_a7xx.so "Adreno 6xx/7xx" || true
stage "Turnip_Gen8_V39.zip"   "572e415c11b636ea2a4c4968c60a9b24e459302549c8ea38e814dbd07e40654d" libturnip_a8xx.so "Adreno 8xx"     || true

# Same release, other two archives, if a variant is ever wanted (same one line each):
#   Turnip_Gen8_V39_Patched.zip  bd457b30f195cf93a334b5302c221b1b5cb59c79c485935decf95d560a38c8a2   extra unmerged patches for AAA games/performance; upstream says use unpatched if you hit issues
#   Turnip_v27.0.0-R2-Oneui.zip  a397a00394aab5a5acc2901d98c98dcf9d2ac4169f12464d654eb51dab92ac03   Adreno 740 on newer One UI, when the standard build makes the UI flicker
ls -la "$OUT" 2>/dev/null
exit 0
