// Android WSI for the native 1.9.x DXVK line (free-function WSI API, replaces the SDL2 files): one fixed ANativeWindow, one monitor.
#include "../wsi_window.h"
#include "../wsi_monitor.h"
#include "../wsi_mode.h"
#include "../wsi_presenter.h"
#include <android/native_window.h>
#include "../../vulkan/vulkan_loader.h"
// 1.9.x vendors only the core Vulkan headers, so declare the one Android type we need (layout per the Vulkan spec).
typedef struct VkAndroidSurfaceCreateInfoKHR { VkStructureType sType; const void* pNext; VkFlags flags; struct ANativeWindow* window; } VkAndroidSurfaceCreateInfoKHR;
typedef VkResult (VKAPI_PTR *PFN_vkCreateAndroidSurfaceKHR)(VkInstance, const VkAndroidSurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
#define AOT_STYPE_ANDROID_SURFACE ((VkStructureType)1000008000)
#include <cstdio>
#include <cstdlib>

static ANativeWindow* g_aw;
static ANativeWindow* aw() { if (!g_aw) { const char* e = getenv("AOT_ANDROID_WINDOW"); if (e) g_aw = (ANativeWindow*)strtoull(e, nullptr, 16); } return g_aw; }
extern "C" { __attribute__((visibility("default"))) void dxvk_android_set_window(void* w){ g_aw = (ANativeWindow*)w; } }

namespace dxvk::wsi {
  static uint32_t W() { return aw() ? (uint32_t)ANativeWindow_getWidth(aw())  : 1920; }
  static uint32_t H() { return aw() ? (uint32_t)ANativeWindow_getHeight(aw()) : 1080; }
  static HMONITOR mon() { return (HMONITOR)(uintptr_t)1; }

  HMONITOR getDefaultMonitor() { return mon(); }
  HMONITOR enumMonitors(uint32_t i) { return i == 0 ? mon() : nullptr; }
  bool getDisplayName(HMONITOR, WCHAR (&n)[32]) { const char* s = "Display1"; for (int i = 0; i < 32; i++) n[i] = i < 8 ? s[i] : 0; return true; }
  bool getDesktopCoordinates(HMONITOR, RECT* r) { r->left = r->top = 0; r->right = W(); r->bottom = H(); return true; }
  bool getCurrentDisplayMode(HMONITOR, WsiMode* m) { *m = WsiMode{ W(), H(), { 60, 1 }, 32, false }; return true; }
  bool getDisplayMode(HMONITOR h, uint32_t n, WsiMode* m) { if (n) return false; return getCurrentDisplayMode(h, m); }
  bool getDesktopDisplayMode(HMONITOR h, WsiMode* m) { return getCurrentDisplayMode(h, m); }
  void getWindowSize(HWND, uint32_t* w, uint32_t* h) { if (w) *w = W(); if (h) *h = H(); }
  void resizeWindow(HWND, DxvkWindowState*, uint32_t, uint32_t) {}
  bool setWindowMode(HMONITOR, HWND, const WsiMode*, bool) { return true; }
  bool enterFullscreenMode(HMONITOR, HWND, DxvkWindowState*, bool) { return true; }
  bool leaveFullscreenMode(HWND, DxvkWindowState*) { return true; }
  bool restoreDisplayMode(HMONITOR) { return true; }
  HMONITOR getWindowMonitor(HWND) { return mon(); }
  bool isWindow(HWND) { return aw() != nullptr; }

  VkResult createSurface(HWND, const Rc<vk::InstanceFn>& vki, VkSurfaceKHR* out) {
    if (!aw()) { fprintf(stderr, "[dxvk-wsi] no ANativeWindow set in this library\n"); return VK_ERROR_INITIALIZATION_FAILED; }
    auto fn = (PFN_vkCreateAndroidSurfaceKHR)vki->sym("vkCreateAndroidSurfaceKHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkAndroidSurfaceCreateInfoKHR ci = { AOT_STYPE_ANDROID_SURFACE }; ci.window = aw();
    VkResult r = fn(vki->instance(), &ci, nullptr, out); fprintf(stderr, "[dxvk-wsi] vkCreateAndroidSurfaceKHR(window=%p) -> %d\n", (void*)aw(), (int)r); return r;
  }
}
