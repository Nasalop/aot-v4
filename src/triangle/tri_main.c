/* NativeActivity entry for the Vulkan triangle self-test (built when AOT_MODE=triangle). */
#include <android_native_app_glue.h>
#include <android/log.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include "vk_triangle.h"

typedef struct { TriCtx *ctx; int ready, stale; } S;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void fail_fill_red(ANativeWindow *w) {   /* best effort: visible failure signal */
  ANativeWindow_setBuffersGeometry(w, 0, 0, WINDOW_FORMAT_RGBA_8888);
  ANativeWindow_Buffer b; if (ANativeWindow_lock(w, &b, 0) != 0) return;
  for (int y = 0; y < b.height; y++) { uint32_t *p = (uint32_t *)b.bits + (size_t)y * b.stride; for (int x = 0; x < b.width; x++) p[x] = 0xFF0000FFu; }
  ANativeWindow_unlockAndPost(w);
}
static void on_cmd(struct android_app *app, int32_t cmd) {
  S *s = (S *)app->userData;
  switch (cmd) {
    case APP_CMD_INIT_WINDOW:
      if (!app->window) break;
      s->ctx = tri_create();
      if (tri_init(s->ctx, app->window) == 0) s->ready = 1; else { tri_log("init failed; see lines above"); tri_destroy(s->ctx); s->ctx = 0; fail_fill_red(app->window); }
      break;
    case APP_CMD_TERM_WINDOW: s->ready = 0; if (s->ctx) { tri_destroy(s->ctx); s->ctx = 0; } break;
    case APP_CMD_WINDOW_RESIZED: case APP_CMD_CONFIG_CHANGED: s->stale = 1; break;
  }
}
void android_main(struct android_app *app) {
  static S s; memset(&s, 0, sizeof s); app->userData = &s; app->onAppCmd = on_cmd;
  char path[600]; snprintf(path, sizeof path, "%s/triangle_log.txt", app->activity->externalDataPath ? app->activity->externalDataPath : "/sdcard"); tri_open_log(path);
  tri_log("triangle self-test starting"); double t0 = now();
  for (;;) {
    int ev; struct android_poll_source *src;
    while (ALooper_pollAll(s.ready ? 0 : -1, 0, &ev, (void **)&src) >= 0) { if (src) src->process(app, src); if (app->destroyRequested) { if (s.ctx) tri_destroy(s.ctx); return; } }
    if (s.ready) {
      int rc = s.stale ? 1 : tri_draw(s.ctx, (float)(now() - t0));
      if (rc == 1) { s.stale = 0; if (tri_recreate(s.ctx)) s.ready = 0; } else if (rc < 0) s.ready = 0;
    }
  }
}
