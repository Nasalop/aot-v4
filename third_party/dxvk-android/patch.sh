#!/usr/bin/env bash
# Usage: patch.sh <dxvk-src-dir>. Adds the Android WSI and drops the SDL/GLFW requirement.
set -e; D=$1; H=$(dirname "$(readlink -f "$0")")
mkdir -p $D/src/wsi/android; cp $H/wsi_android.cpp $D/src/wsi/android/
python3 - "$D" <<'P'
import sys,re; d=sys.argv[1]
def sub(f,a,b,c=1):
    s=open(d+'/'+f).read(); assert a in s,(f,a); open(d+'/'+f,'w').write(s.replace(a,b,c))
sub('src/wsi/meson.build',"  'glfw/wsi_monitor_glfw.cpp',","  'android/wsi_android.cpp',\n  'glfw/wsi_monitor_glfw.cpp',")
sub('src/wsi/meson.build',"""  wsi_deps += [
    lib_sdl3.partial_dependency(compile_args: true, includes: true),
    lib_sdl2.partial_dependency(compile_args: true, includes: true),
    lib_glfw.partial_dependency(compile_args: true, includes: true),
  ]""","  wsi_deps += [ cpp.find_library('android') ]")
sub('src/wsi/wsi_platform.cpp',"  static const WsiBootstrap *wsiBootstrap[] = {","  extern WsiBootstrap AndroidWSI;\n  static const WsiBootstrap *wsiBootstrap[] = {\n    &AndroidWSI,")
sub('src/wsi/wsi_platform.cpp','        throw DxvkError("DXVK_WSI_DRIVER environment variable unset");','        hint = "Android";')
sub('meson.build',"""  if (not lib_sdl3.found() and not lib_sdl2.found() and not lib_glfw.found())
    error('SDL3, SDL2, or GLFW are required to build dxvk-native')
  endif""","")
P
