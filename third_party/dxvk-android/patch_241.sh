#!/usr/bin/env bash
# Usage: patch_241.sh <dxvk-2.4.1-src-dir>. Same job as patch.sh, for the 2.4.1 tree: Android WSI, no SDL/GLFW requirement.
# (2.4.1 already treats VK_SUBOPTIMAL_KHR as success in the presenter, so the presenter edits of patch.sh are not needed.)
set -e; D=$1; H=$(dirname "$(readlink -f "$0")")
mkdir -p $D/src/wsi/android; cp $H/wsi_android_241.cpp $D/src/wsi/android/wsi_android.cpp
python3 - "$D" <<'P'
import sys; d=sys.argv[1]
def sub(f,a,b):
    s=open(d+'/'+f).read(); assert s.count(a)==1,(f,a); open(d+'/'+f,'w').write(s.replace(a,b))
sub('src/wsi/meson.build',"  'glfw/wsi_monitor_glfw.cpp',","  'android/wsi_android.cpp',\n  'glfw/wsi_monitor_glfw.cpp',")
sub('src/wsi/meson.build',"""  wsi_deps += [
    lib_sdl2.partial_dependency(compile_args: true, includes: true),
    lib_glfw.partial_dependency(compile_args: true, includes: true),
  ]""","  wsi_deps += [ cpp.find_library('android') ]")
sub('src/wsi/wsi_platform.cpp',"  static const WsiBootstrap *wsiBootstrap[] = {","  extern WsiBootstrap AndroidWSI;\n  static const WsiBootstrap *wsiBootstrap[] = {\n    &AndroidWSI,")
sub('src/wsi/wsi_platform.cpp','        throw DxvkError("DXVK_WSI_DRIVER environment variable unset");','        hint = "Android";')
sub('meson.build',"""  if (not lib_sdl2.found() and not lib_glfw.found())
    error('SDL2 or GLFW are required to build dxvk-native')
  endif""","")
P
python3 "$H/patch_vkloader.py" "$D"
