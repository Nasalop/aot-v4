#include "../dxvk_platform_exts.h"
namespace dxvk {
  DxvkPlatformExts DxvkPlatformExts::s_instance;
  std::string_view DxvkPlatformExts::getName() { return "Android WSI"; }
  DxvkNameSet DxvkPlatformExts::getInstanceExtensions() { DxvkNameSet n; n.add("VK_KHR_surface"); n.add("VK_KHR_android_surface"); return n; }
  DxvkNameSet DxvkPlatformExts::getDeviceExtensions(uint32_t) { return DxvkNameSet(); }
  void DxvkPlatformExts::initInstanceExtensions() {}
  void DxvkPlatformExts::initDeviceExtensions(const DxvkInstance*) {}
}
