#!/usr/bin/env bash
# build.sh <outdir>: stage the bundled Turnip (Mesa Freedreno Vulkan) drivers for the APK.
#   libturnip_a8xx.so           A8xx "Gen8"      Adreno 810/825/829/830/840 (Snapdragon 8 Elite)
#   libturnip_a8xx_patched.so   A8xx "Patched"    same GPUs, extra shader/compute fixes (optional; gpu_driver.txt = turnip_a8xx_patched)
#   libturnip_a7xx.so           A7xx "Standard"  Adreno 6xx / 7xx (Snapdragon 8 Gen 3 and older)
#
# Primary source: StevenMXZ/Adreno-Tools-Drivers releases, newest of V39 then V38 (override: TURNIP_VERSIONS="39 38").
# The release list is read from the GitHub API, the zip for each variant is picked by name, and its SHA-256 is checked against the
# digest the API publishes for that asset. Each zip carries libvulkan_freedreno.so + meta.json (AdrenoTools layout); meta.json names the library.
# Fallback if that source cannot be reached, has no matching asset or fails the hash: the previous Banners-Turnip v27.0.0-r2 pair.
# A failure is NEVER fatal: the APK just ships without that file and the runtime uses the stock driver. dxvk_proxy.c (turnip_setup) loads them.
set -uo pipefail
OUT=$(realpath -m "${1:?usage: build.sh <outdir>}")
VERS=${TURNIP_VERSIONS:-"39 38"}
mkdir -p "$OUT"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT

# pick <variant> -> prints "name<TAB>url<TAB>sha256" for the best matching asset, or nothing
pick() {
python3 - "$1" "$VERS" "$W/releases.json" <<'PY'
import json, re, sys
variant, vers, f = sys.argv[1], sys.argv[2].split(), sys.argv[3]
try: rels = json.load(open(f))
except Exception: sys.exit(0)
def ok(a):
    n = a['name'].lower()
    if not n.endswith('.zip'): return False
    if variant == 'a8xx': return ('a8xx' in n or 'gen8' in n) and 'patch' not in n and 'a7xx' not in n
    if variant == 'a8xx_patched': return ('a8xx' in n or 'gen8' in n) and 'patch' in n
    if variant == 'a7xx': return 'a7xx' in n and not re.search(r'oneui|one_ui|glitch|710|720|8g2|test|a8xx|gen8', n)
for v in vers:
    pat = re.compile(r'(?<![0-9.])v?%s(?![0-9])' % re.escape(v), re.I)
    for r in rels:   # newest first
        for a in r.get('assets', []):
            if ok(a) and (re.search(r'(?<![0-9])v%s(?![0-9])' % re.escape(v), a['name'], re.I) or pat.search(r.get('name') or '') or pat.search(r.get('tag_name') or '')):
                d = (a.get('digest') or '').replace('sha256:', '')
                print('%s\t%s\t%s' % (a['name'], a['browser_download_url'], d)); sys.exit(0)
PY
}

fetch_releases() {
  local hdr=(); [ -n "${GITHUB_TOKEN:-}" ] && hdr=(-H "Authorization: Bearer $GITHUB_TOKEN")
  curl -fsSL --retry 3 --connect-timeout 20 "${hdr[@]}" -H "Accept: application/vnd.github+json" \
    "https://api.github.com/repos/StevenMXZ/Adreno-Tools-Drivers/releases?per_page=100" -o "$W/releases.json" || { echo "WARNING: turnip: could not read the StevenMXZ release list"; rm -f "$W/releases.json"; return 1; }
}

unpack_driver() {   # unpack_driver <zip> <dst>
  local zip=$1 dst=$2 d lib src
  d=$(mktemp -d -p "$W"); ( cd "$d" && unzip -q -o "$zip" ) || return 1
  lib=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get('libraryName',''))" "$(find "$d" -name meta.json | head -1)" 2>/dev/null)
  src=""; [ -n "$lib" ] && src=$(find "$d" -name "$lib" | head -1)
  [ -n "$src" ] || src=$(find "$d" -name 'libvulkan_freedreno.so' | head -1)
  [ -n "$src" ] || src=$(find "$d" -name '*.so' | head -1)
  [ -n "$src" ] || return 1
  cp -L "$src" "$OUT/$dst.part" && mv "$OUT/$dst.part" "$OUT/$dst" && [ -s "$OUT/$dst" ]
}

stage_new() {   # stage_new <variant> <staged name> <what>
  local variant=$1 dst=$2 what=$3 line name url sha got
  [ -s "$OUT/$dst" ] && { echo "turnip: $dst already staged"; return 0; }
  [ -s "$W/releases.json" ] || return 1
  line=$(pick "$variant"); [ -n "$line" ] || { echo "turnip: no $variant asset for V$VERS in the StevenMXZ releases"; return 1; }
  IFS=$'\t' read -r name url sha <<<"$line"
  curl -fsSL --retry 3 --connect-timeout 20 -o "$W/$name" "$url" || { echo "WARNING: turnip: could not download $name"; return 1; }
  if [ -n "$sha" ]; then
    got=$(sha256sum "$W/$name" | cut -d' ' -f1)
    [ "$got" = "$sha" ] || { echo "WARNING: turnip: $name does not match the SHA-256 GitHub published for it; not staging it"; return 1; }
  else echo "turnip: note: no digest published for $name, relying on TLS only"; fi
  unpack_driver "$W/$name" "$dst" || { echo "WARNING: turnip: $name could not be unpacked / has no driver library"; return 1; }
  echo "turnip: staged $dst ($(du -h "$OUT/$dst" | cut -f1)) from $name - $what"
}

# ---- fallback: the earlier Banners-Turnip pair, pinned by SHA-256 ----
BTAG=v27.0.0-20261114-r2; BBASE="https://github.com/The412Banner/Banners-Turnip/releases/download/$BTAG"
stage_banner() {   # stage_banner <zip> <sha256> <staged name> <what>
  local zip=$1 sha=$2 dst=$3 what=$4
  [ -s "$OUT/$dst" ] && return 0
  curl -fsSL --retry 3 --connect-timeout 20 -o "$W/$zip" "$BBASE/$zip" || { echo "WARNING: turnip: fallback $zip not downloadable ($what)"; return 1; }
  echo "$sha  $W/$zip" | sha256sum -c - >/dev/null 2>&1 || { echo "WARNING: turnip: fallback $zip fails its SHA-256"; return 1; }
  unpack_driver "$W/$zip" "$dst" && echo "turnip: staged $dst from fallback $zip - $what"
}

fetch_releases || true
# ---- every driver zip in the V39 / V38 releases, staged as libturnip_<slug>.so so the in-app picker can offer them all ----
# Names of the release assets are not assumed: whatever .zip assets those releases carry are staged (max TURNIP_MAX, default 8).
stage_all() {
python3 - "$VERS" "$W/releases.json" "${TURNIP_MAX:-8}" <<'PY'
import json, re, sys
vers, f, mx = sys.argv[1].split(), sys.argv[2], int(sys.argv[3])
try: rels = json.load(open(f))
except Exception: sys.exit(0)
out, seen = [], set()
for v in vers:
    pat = re.compile(r'(?<![0-9])v?%s(?![0-9])' % re.escape(v), re.I)
    for r in rels:
        if not (pat.search(r.get('name') or '') or pat.search(r.get('tag_name') or '')): continue
        for a in r.get('assets', []):
            n = a['name']
            if not n.lower().endswith('.zip') or n in seen: continue
            if re.search(r'a6xx|a5xx|a4xx|a3xx', n, re.I) and not re.search(r'a7xx|a8xx|gen8', n, re.I): continue
            seen.add(n); out.append((n, a['browser_download_url'], (a.get('digest') or '').replace('sha256:', '')))
for n, u, d in out[:mx]: print('%s\t%s\t%s' % (n, u, d))
PY
}
stage_all | while IFS=$'\t' read -r name url sha; do
  slug=$(echo "${name%.zip}" | tr 'A-Z' 'a-z' | tr -c 'a-z0-9\n' '_' | cut -c1-40)
  dst="libturnip_${slug}.so"; [ -s "$OUT/$dst" ] && continue
  curl -fsSL --retry 3 --connect-timeout 20 -o "$W/$name" "$url" || { echo "WARNING: turnip: could not download $name"; continue; }
  if [ -n "$sha" ]; then got=$(sha256sum "$W/$name" | cut -d' ' -f1); [ "$got" = "$sha" ] || { echo "WARNING: turnip: $name fails its SHA-256"; continue; }; fi
  unpack_driver "$W/$name" "$dst" && echo "turnip: staged $dst from $name"
done
stage_new a8xx         libturnip_a8xx.so         "Adreno 8xx"           || stage_banner "Turnip-$BTAG-A8xx.zip" 13485ce18af8b861efcbf3d25560b15285c2298f460e0d3b7c32abbfd1f17f7a libturnip_a8xx.so "Adreno 8xx (fallback)" || true
stage_new a7xx         libturnip_a7xx.so         "Adreno 6xx/7xx"       || stage_banner "Turnip-$BTAG.zip"      40f5becf92911aea9b18e1ecb9e1d4e790ebb1dde6d5a863f6a9c1d2d083bb8f libturnip_a7xx.so "Adreno 6xx/7xx (fallback)" || true
stage_new a8xx_patched libturnip_a8xx_patched.so "Adreno 8xx patched"  || true
ls -la "$OUT" 2>/dev/null
exit 0
