/* EDIT-menu settings: frame-rate cap and the upscaler.
   Kept deliberately small and self-contained so the whole feature can be removed by deleting this file, the call
   sites and the menu, or switched off at runtime by choosing OFF in the menu. */
#include "aot_settings.h"
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

static int s_fps;            /* 0 = uncapped, 30, 60 */
static int s_upscale;        /* off by default: nothing about rendering changes until the menu turns it on */
static int s_quality = 2;    /* 2 = balanced */
static int s_reported;
static void *s_vendor;       /* the vendor SGSR2 library, when this build ships one */

/* Render scale per level, as a percentage of the native size. These are the usual upscalers' steps: the more the
   image is scaled up, the fewer pixels the game shades. */
static const int QUAL_PCT[5] = { 83, 77, 67, 59, 50 };
static const char *const QUAL_NAME[5] = { "ultra quality", "quality", "balanced", "performance", "ultra performance" };

static void probe_vendor(void) {
  static const char *const names[] = { "libSGSR2.so", "libsgsr2.so", "libSGSR.so", "libsgsr.so", "libGameSuperResolution.so" };
  for (unsigned i = 0; i < sizeof names / sizeof names[0] && !s_vendor; i++) {
    s_vendor = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (s_vendor) { fprintf(stderr, "[gfx] upscaler: vendor library %s loaded\n", names[i]); return; }
  }
  fprintf(stderr, "[gfx] upscaler: no vendor SGSR2 library in this build; using the render-scale path "
                  "(the game renders smaller and the display scales it up)\n");
}

void aot_settings_init(void) {
  if (s_reported) return;
  s_reported = 1;
  probe_vendor();
  aot_settings_report();
}

void aot_settings_report(void) {
  if (!s_upscale) { fprintf(stderr, "[gfx] settings: fps cap %s, upscaler off\n", s_fps ? (s_fps == 30 ? "30" : "60") : "off"); return; }
  fprintf(stderr, "[gfx] settings: fps cap %s, upscaler %s at %s (%d%% of native size; takes effect from the next\n"
                  "      window/swapchain creation, so restart the game after changing it)\n",
          s_fps ? (s_fps == 30 ? "30" : "60") : "off", s_vendor ? "SGSR2 (vendor)" : "SGSR2 (render scale)",
          QUAL_NAME[s_quality], QUAL_PCT[s_quality]);
}

void aot_settings_set(int fps_cap, int upscale, int quality) {
  if (!s_reported) aot_settings_init();
  if (fps_cap != 0 && fps_cap != 30 && fps_cap != 60) fps_cap = 0;
  s_fps = fps_cap;
  s_upscale = upscale != 0;
  s_quality = quality < 0 ? 0 : quality > 4 ? 4 : quality;
  aot_settings_report();
}

int aot_settings_fps(void) { return s_fps; }

int aot_settings_scale(uint32_t *w, uint32_t *h) {
  if (!w || !h) return 100;
  int pct = s_upscale ? QUAL_PCT[s_quality] : 100;
  if (pct < 100 && *w > 320 && *h > 240) {
    *w = (uint32_t)((uint64_t)*w * (uint32_t)pct / 100u) & ~1u;   /* even sizes: some drivers dislike odd back buffers */
    *h = (uint32_t)((uint64_t)*h * (uint32_t)pct / 100u) & ~1u;
  }
  return pct;
}
