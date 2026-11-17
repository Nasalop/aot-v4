/* Minimal Vulkan "loader" for a bundled driver (libaotvkicd.so).
 *
 * DXVK asks dlopen() for a Vulkan loader and then dlsym()s vkGetInstanceProcAddr/vkCreateInstance. The Android
 * system loader only looks at the phone's own ICDs (and only honours VK_ICD_FILENAMES on debuggable apps), so a
 * driver we ship can never be reached through it. This library is that loader instead, for the four entry points
 * DXVK uses: it loads the ICD named by AOT_VK_ICD and forwards everything to it through the driver's own
 * vk_icdGetInstanceProcAddr. Nothing is installed process-wide: DXVK dlopens this file by absolute path only
 * when a Turnip driver was chosen, so the normal path (Android's libvulkan.so.1) is never touched.
 *
 * Kill switch: AOT_GPU_DRIVERS=0 in aot_gpu.c, or pick "System driver" in the menu. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define EXPORT __attribute__((visibility("default")))

typedef void *(*ICD_GIPA)(void *instance, const char *name);
typedef VkResult (*ICD_NEGOTIATE)(uint32_t *version);

static void *g_icd;
static ICD_GIPA g_gipa;
static char g_err[256];

static int icd_open(void) {
  if (g_icd) return 1;
  const char *p = getenv("AOT_VK_ICD");
  if (!p || !*p) { snprintf(g_err, sizeof g_err, "AOT_VK_ICD is not set"); return 0; }
  if (!(g_icd = dlopen(p, RTLD_NOW | RTLD_LOCAL))) { snprintf(g_err, sizeof g_err, "%s", dlerror()); return 0; }
  /* The loader/ICD interface: negotiate down to what this driver supports, then talk through its proc-addr hook. */
  ICD_NEGOTIATE neg = (ICD_NEGOTIATE)dlsym(g_icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
  if (neg) { uint32_t v = 5; (void)neg(&v); }
  g_gipa = (ICD_GIPA)dlsym(g_icd, "vk_icdGetInstanceProcAddr");
  if (!g_gipa) g_gipa = (ICD_GIPA)dlsym(g_icd, "vkGetInstanceProcAddr");
  if (!g_gipa) { snprintf(g_err, sizeof g_err, "%s exposes no vk_icdGetInstanceProcAddr", p); dlclose(g_icd); g_icd = NULL; return 0; }
  return 1;
}

static void *icd_proc(void *instance, const char *name) { return icd_open() ? g_gipa(instance, name) : NULL; }

EXPORT VkResult vkEnumerateInstanceVersion(uint32_t *api) {
  PFN_vkEnumerateInstanceVersion f = (PFN_vkEnumerateInstanceVersion)icd_proc(NULL, "vkEnumerateInstanceVersion");
  if (f) return f(api);
  if (api) *api = VK_API_VERSION_1_0;
  return VK_SUCCESS;
}

EXPORT VkResult vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *props) {
  if (count) *count = 0;
  if (layer) return VK_SUCCESS;   /* this build ships no layers, so no layer has extensions */
  PFN_vkEnumerateInstanceExtensionProperties f =
      (PFN_vkEnumerateInstanceExtensionProperties)icd_proc(NULL, "vkEnumerateInstanceExtensionProperties");
  return f ? f(NULL, count, props) : VK_SUCCESS;
}

EXPORT VkResult vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *props) {
  (void)props; if (count) *count = 0;
  return VK_SUCCESS;
}

EXPORT VkResult vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *out) {
  PFN_vkCreateInstance f = (PFN_vkCreateInstance)icd_proc(NULL, "vkCreateInstance");
  if (!f) {
    fprintf(stderr, "[gpu] the bundled Vulkan driver has no instance entry point: %s\n", g_err);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  return f(ci, alloc, out);
}

EXPORT PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name) {
  if (!name) return NULL;
  if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
  if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)vkCreateInstance;
  if (!strcmp(name, "vkEnumerateInstanceVersion")) return (PFN_vkVoidFunction)vkEnumerateInstanceVersion;
  if (!strcmp(name, "vkEnumerateInstanceExtensionProperties")) return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
  if (!strcmp(name, "vkEnumerateInstanceLayerProperties")) return (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties;
  return (PFN_vkVoidFunction)icd_proc((void *)instance, name);
}

/* Does this driver actually come up on this phone? Called by aot_gpu.c before the game starts, so a driver that
   cannot create an instance (wrong GPU, no /dev/kgsl, wrong Adreno generation) is rejected and the system driver
   is used instead of failing the game. Fills desc with what came back, for the log. */
EXPORT int aotvk_self_test(const char *icd, char *desc, int cap) {
  if (desc && cap > 0) desc[0] = 0;
  if (!icd || !*icd) { if (desc && cap > 0) snprintf(desc, cap, "no driver path"); return -1; }
  if (g_icd) { dlclose(g_icd); g_icd = NULL; g_gipa = NULL; }
  setenv("AOT_VK_ICD", icd, 1);
  if (!icd_open()) { if (desc && cap > 0) snprintf(desc, cap, "%s", g_err); return -1; }

  VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "aotgame", .apiVersion = VK_API_VERSION_1_1 };
  const char *exts[2] = { "VK_KHR_surface", "VK_KHR_android_surface" };
  VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                              .enabledExtensionCount = 2, .ppEnabledExtensionNames = exts };
  VkInstance inst = NULL;
  VkResult r = vkCreateInstance(&ci, NULL, &inst);
  if (r != VK_SUCCESS || !inst) {
    if (desc && cap > 0) snprintf(desc, cap, "vkCreateInstance returned %d", (int)r);
    return -1;
  }
  int ok = -1;
  uint32_t n = 0;
  PFN_vkEnumeratePhysicalDevices epd = (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(inst, "vkEnumeratePhysicalDevices");
  PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties");
  if (!epd || !gpp) { if (desc && cap > 0) snprintf(desc, cap, "driver has no adapter entry points"); }
  else if (epd(inst, &n, NULL) != VK_SUCCESS || n == 0) { if (desc && cap > 0) snprintf(desc, cap, "no Vulkan adapter"); }
  else {
    VkPhysicalDevice pd[8];
    if (n > 8) n = 8;
    if (epd(inst, &n, pd) == VK_SUCCESS && n > 0) {
      VkPhysicalDeviceProperties p;
      memset(&p, 0, sizeof p);
      gpp(pd[0], &p);
      /* The driver name lives in VkPhysicalDeviceDriverProperties (Vulkan 1.2), reached through properties2. */
      char drv[192] = "driver name not reported";
      PFN_vkGetPhysicalDeviceProperties2 gpp2 =
          (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2");
      if (gpp2) {
        VkPhysicalDeviceDriverProperties dp;
        memset(&dp, 0, sizeof dp);
        dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 p2;
        memset(&p2, 0, sizeof p2);
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &dp;
        gpp2(pd[0], &p2);
        if (dp.driverName[0]) snprintf(drv, sizeof drv, "%s %.80s", dp.driverName, dp.driverInfo);
      }
      if (desc && cap > 0)
        snprintf(desc, cap, "%s | %s | Vulkan %u.%u.%u", p.deviceName, drv,
                 VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
      ok = 0;
    }
  }
  PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)vkGetInstanceProcAddr(inst, "vkDestroyInstance");
  if (di) di(inst, NULL);
  return ok;
}
