// Android WSI driver for DXVK Native: one fixed window (ANativeWindow), one monitor.
#include "wsi_platform.h"
#include "wsi_monitor.h"
#include "wsi_window.h"
#include "../util/util_error.h"
#include <android/native_window.h>
#include <vulkan/vulkan_android.h>
#include <cstring>
#include <dlfcn.h>
#include <cstdio>

extern "C" { static ANativeWindow* g_aw; __attribute__((visibility("default"))) void dxvk_android_set_window(void* w){ g_aw = (ANativeWindow*)w; } }

namespace dxvk::wsi {
  class AndroidWsiDriver : public WsiDriver {
    static uint32_t W() { return g_aw ? (uint32_t)ANativeWindow_getWidth(g_aw)  : 1920; }
    static uint32_t H() { return g_aw ? (uint32_t)ANativeWindow_getHeight(g_aw) : 1080; }
    static HMONITOR mon() { return (HMONITOR)(uintptr_t)1; }
  public:
    std::vector<const char*> getInstanceExtensions() override { return { "VK_KHR_surface", "VK_KHR_android_surface" }; }
    HMONITOR getDefaultMonitor() override { return mon(); }
    HMONITOR enumMonitors(uint32_t i) override { return i == 0 ? mon() : nullptr; }
    HMONITOR enumMonitors(const LUID*[], uint32_t, uint32_t i) override { return i == 0 ? mon() : nullptr; }
    bool getDisplayName(HMONITOR, WCHAR (&n)[32]) override { const char* s = "Display1"; for (int i = 0; i < 32; i++) n[i] = i < 8 ? s[i] : 0; return true; }
    bool getDesktopCoordinates(HMONITOR, RECT* r) override { r->left = r->top = 0; r->right = W(); r->bottom = H(); return true; }
    bool getDisplayMode(HMONITOR, uint32_t n, WsiMode* m) override { if (n) return false; return getCurrentDisplayMode(nullptr, m); }
    bool getCurrentDisplayMode(HMONITOR, WsiMode* m) override { *m = WsiMode{ W(), H(), { 60, 1 }, 32, false }; return true; }
    bool getDesktopDisplayMode(HMONITOR h, WsiMode* m) override { return getCurrentDisplayMode(h, m); }
    WsiEdidData getMonitorEdid(HMONITOR) override { return {}; }
    void getWindowSize(HWND, uint32_t* w, uint32_t* h) override { if (w) *w = W(); if (h) *h = H(); }
    void resizeWindow(HWND, DxvkWindowState*, uint32_t, uint32_t) override {}   // surface size is fixed by the device
    void saveWindowState(HWND, DxvkWindowState*, bool) override {}
    void restoreWindowState(HWND, DxvkWindowState*, bool) override {}
    bool setWindowMode(HMONITOR, HWND, DxvkWindowState*, const WsiMode&) override { return true; }
    bool enterFullscreenMode(HMONITOR, HWND, DxvkWindowState*, bool) override { return true; }
    bool leaveFullscreenMode(HWND, DxvkWindowState*) override { return true; }
    bool restoreDisplayMode() override { return true; }
    HMONITOR getWindowMonitor(HWND) override { return mon(); }
    bool isWindow(HWND) override { return g_aw != nullptr; }
    bool isMinimized(HWND) override { return false; }
    bool isOccluded(HWND) override { return false; }
    void updateFullscreenWindow(HMONITOR, HWND, bool) override {}
    VkResult createSurface(HWND, PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkSurfaceKHR* out) override {
      if (!g_aw) { auto pw = (void* (*)())dlsym(RTLD_DEFAULT, "aot_present_window"); if (pw) g_aw = (ANativeWindow*)pw(); }
      if (!g_aw) { fprintf(stderr, "[dxvk-wsi] no ANativeWindow set in this library\n"); return VK_ERROR_INITIALIZATION_FAILED; }
      auto fn = (PFN_vkCreateAndroidSurfaceKHR)gipa(inst, "vkCreateAndroidSurfaceKHR");
      if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
      VkAndroidSurfaceCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR }; ci.window = g_aw;
      VkResult r = fn(inst, &ci, nullptr, out); fprintf(stderr, "[dxvk-wsi] vkCreateAndroidSurfaceKHR(window=%p) -> %d\n", (void*)g_aw, (int)r); return r;
    }
  };
  static bool createAndroid(WsiDriver** d) { *d = new AndroidWsiDriver(); return true; }
  WsiBootstrap AndroidWSI = { "Android", createAndroid };
}
