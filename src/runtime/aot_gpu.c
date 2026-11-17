/* Bundled Vulkan driver selection: the phone's own driver, or one of the Turnip builds shipped in jniLibs.
 *
 * Turnip is Mesa's open-source Vulkan driver for Adreno. It is loaded through libaotvkicd.so (aot_vk_icd.c), which
 * DXVK is pointed at with AOT_VULKAN_LIB; the Android system loader is never replaced, so a driver that fails to
 * start cannot break the normal path. Selection is a product decision, not a hidden default: the menu asks once,
 * the default is "System driver", and every failure falls back to it with one [gpu] line in the log.
 *
 * One-line kill switch: build with -DAOT_GPU_DRIVERS=0 and this file never touches the Vulkan environment. */
#include "aot_gpu.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef AOT_GPU_DRIVERS
#define AOT_GPU_DRIVERS 1
#endif

static char g_libdir[512];   /* the app's nativeLibraryDir: the only place a driver may live at targetSdk 34 */
static int  g_want;          /* what the menu asked for */
static int  g_active;        /* what is in effect */

void aot_gpu_init(const char *native_lib_dir, int driver) {
  snprintf(g_libdir, sizeof g_libdir, "%s", native_lib_dir ? native_lib_dir : "");
  g_want = (driver >= 0 && driver <= 2) ? driver : 0;
}

static const char *driver_so(int d) {
  if (d == 1) return "libturnip_a7xx.so";   /* Adreno 6xx / 7xx */
  if (d == 2) return "libturnip_a8xx.so";   /* Adreno 8xx    */
  return NULL;
}

static const char *driver_name(int d) { return d == 1 ? "Turnip (Adreno 6xx/7xx)" : d == 2 ? "Turnip (Adreno 8xx)" : "system"; }

/* Called from init_dxvk() before the first DXVK instance exists. Returns the driver that will be used. */
int aot_gpu_apply(void) {
  g_active = 0;
  unsetenv("AOT_VK_ICD");
  unsetenv("AOT_VULKAN_LIB");
  const char *so = AOT_GPU_DRIVERS ? driver_so(g_want) : NULL;
  if (!so) { fprintf(stderr, "[gpu] Vulkan driver: system (the phone's own driver)\n"); return 0; }

  char icd[600], wrapper[600];
  snprintf(icd, sizeof icd, "%s/%s", g_libdir, so);
  snprintf(wrapper, sizeof wrapper, "%s/libaotvkicd.so", g_libdir);
  if (!g_libdir[0] || access(icd, R_OK) != 0 || access(wrapper, R_OK) != 0) {
    fprintf(stderr, "[gpu] %s was selected but this build has no such driver (%s); using the system driver\n", driver_name(g_want), icd);
    return 0;
  }

  void *h = dlopen(wrapper, RTLD_NOW | RTLD_LOCAL);
  if (!h) { fprintf(stderr, "[gpu] cannot load %s: %s; using the system driver\n", wrapper, dlerror()); return 0; }
  int (*self_test)(const char *, char *, int) = (int (*)(const char *, char *, int))dlsym(h, "aotvk_self_test");
  char desc[320] = "";
  if (!self_test || self_test(icd, desc, sizeof desc) != 0) {
    fprintf(stderr, "[gpu] %s does not run on this phone (%s); using the system driver\n", driver_name(g_want), desc[0] ? desc : "no self-test entry point");
    unsetenv("AOT_VK_ICD");
    return 0;
  }

  /* Leave both set: DXVK dlopens the wrapper by this path and the wrapper loads the ICD by the other. */
  setenv("AOT_VK_ICD", icd, 1);
  setenv("AOT_VULKAN_LIB", wrapper, 1);
  fprintf(stderr, "[gpu] Vulkan driver: %s -> %s\n", driver_name(g_want), desc);
  g_active = g_want;
  return g_active;
}

int aot_gpu_driver(void) { return g_active; }
