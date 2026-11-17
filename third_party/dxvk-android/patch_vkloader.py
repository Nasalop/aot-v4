#!/usr/bin/env python3
"""patch_vkloader.py <dxvk-src-dir>: DXVK 3.x / 2.x take vkGetInstanceProcAddr from AOT_VK_GIPA (hex pointer) when the runtime sets it,
instead of dlopen("libvulkan.so"). The runtime sets it to the entry point of a private libvulkan opened through libadrenotools, which is
how a custom (Turnip) driver gets in. Unset = the stock behaviour, byte for byte."""
import sys
f = sys.argv[1] + '/src/vulkan/vulkan_loader.cpp'
s = open(f).read()
a = "    std::tie(m_library, m_getInstanceProcAddr) = loadVulkanLibrary();\n"
b = ('    PFN_vkGetInstanceProcAddr aotGipa = nullptr;\n'
     '#ifndef _WIN32\n'
     '    if (const char* e = std::getenv("AOT_VK_GIPA")) aotGipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(static_cast<uintptr_t>(std::strtoull(e, nullptr, 16)));\n'
     '#endif\n'
     '    if (aotGipa) { m_library = nullptr; m_getInstanceProcAddr = aotGipa; }\n'
     '    else std::tie(m_library, m_getInstanceProcAddr) = loadVulkanLibrary();\n')
assert s.count(a) == 1, 'loader ctor anchor not found'
s = s.replace(a, b).replace('#include <tuple>\n', '#include <tuple>\n#include <cstdlib>\n#include <cstdint>\n', 1)
assert '#include <cstdlib>' in s
open(f, 'w').write(s)
