#!/usr/bin/env bash
# Usage: patch_19.sh <dxvk-native-1.9.2-src-dir>. Swaps the SDL2 window layer of Joshua-Ashton's native 1.9.2 for an Android one.
set -e; D=$1; H=$(dirname "$(readlink -f "$0")")
mkdir -p $D/src/wsi/android; cp $H/wsi_android_19.cpp $D/src/wsi/android/wsi_android.cpp
cp $H/dxvk_android_exts.cpp $D/src/dxvk/platform/dxvk_android_exts.cpp
python3 - "$D" <<'P'
import sys; d=sys.argv[1]
def sub(f,a,b):
    s=open(d+'/'+f).read(); assert s.count(a)==1,(f,a); open(d+'/'+f,'w').write(s.replace(a,b))
sub('src/wsi/meson.build',"""wsi_sdl2_src = [
  'sdl2/wsi_mode_sdl2.cpp',
  'sdl2/wsi_monitor_sdl2.cpp',
  'sdl2/wsi_presenter_sdl2.cpp',
  'sdl2/wsi_window_sdl2.cpp',
]""","wsi_sdl2_src = [ 'android/wsi_android.cpp' ]")
sub('src/dxvk/meson.build',"  'platform/dxvk_sdl2_exts.cpp'","  'platform/dxvk_android_exts.cpp'")
sub('meson.build',"    lib_sdl2    = dxvk_compiler.find_library('SDL2')\n    wrc = find_program('echo')","    lib_sdl2    = dxvk_compiler.find_library('android')\n    wrc = find_program('echo')")
P
