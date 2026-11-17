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
sub('src/dxvk/dxvk_presenter.cpp',"    if (m_acquireStatus != VK_SUCCESS && m_swapchain)\n      Logger::info","    if (m_acquireStatus == VK_SUBOPTIMAL_KHR) m_acquireStatus = VK_SUCCESS;  /* Android: rotated display makes every present SUBOPTIMAL; rebuilding each frame is useless */\n\n    if (m_acquireStatus != VK_SUCCESS && m_swapchain)\n      Logger::info")
sub('src/dxvk/dxvk_presenter.cpp',"    if (status != VK_SUCCESS) {\n      Logger::info","    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR) {\n      Logger::info")
P
# Runtime choice of the Vulkan loader, so a bundled Turnip driver can be used without replacing the system library.
# Not fatal: a tree without the expected literal simply keeps loading Android's own libvulkan.so.1.
python3 - "$D" <<'P' || true
import sys
d = sys.argv[1]; p = d + '/src/vulkan/loader.cpp'
try: s = open(p).read()
except OSError:
    print('turnip: no src/vulkan/loader.cpp in this tree; DXVK keeps the system Vulkan loader'); raise SystemExit(0)
if '"libvulkan.so.1"' not in s:
    print('turnip: DXVK does not name libvulkan.so.1 here; keeping the system loader'); raise SystemExit(0)
s = s.replace('"libvulkan.so.1"', 'aot_vulkan_lib_name()')
s = ('#include <cstdlib>\n'
     '/* AOT_VULKAN_LIB names a bundled Vulkan driver wrapper (libaotvkicd.so); unset means Android\'s own\n'
     '   libvulkan.so.1, exactly as upstream, so the system-driver path never changes. */\n'
     'static inline const char* aot_vulkan_lib_name() {\n'
     '  const char* p = std::getenv("AOT_VULKAN_LIB");\n'
     '  return (p && *p) ? p : "libvulkan.so.1";\n'
     '}\n') + s
open(p, 'w').write(s)
print('turnip: DXVK now honours AOT_VULKAN_LIB')
P
