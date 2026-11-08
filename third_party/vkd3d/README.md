vkd3d-shader (HLSL -> DXBC compiler) from Wine's bundled vkd3d at the commit in WINE_COMMIT. LGPL-2.1+ (COPYING.LGPL).
include/ = its public headers; shim/ = tiny Windows/Wine header subset so it builds outside Wine; build.sh produces libvkd3dshader.so for the APK.
