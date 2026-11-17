/* NativeActivity host. Game modes (AOT_ANDROID_APP): extract game.exe, redirect stdout/stderr to <ext>/log.txt, run the
 * recompiled game on a thread and feed touch/keyboard/gamepad input to the Win32 layer. Otherwise: idle host for the Java UI. */
#include <android_native_app_glue.h>
#ifdef AOT_ANDROID_APP
#include <android/log.h>
#include <jni.h>
#include <android/input.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "win_input.h"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "aotgame", __VA_ARGS__)
extern int aot_main(int, char **);
extern void aot_android_set_window(ANativeWindow *);
static struct android_app *g_app;
/* A finished or crashed guest must end the process: parking the thread here leaves the activity alive with a dead renderer, which is the white screen. */
void aot_exit(int code) { fflush(stdout); dprintf(2, "\n[aot] program exited with code %d\n", code); LOG("exit %d", code); _exit(code); }

/* Android performance hint session (ADPF, API 33+, looked up at run time because the app's minSdk is lower). Without it the governor sees one thread at ~60% load and keeps
   the cores at a modest clock; with it the system knows this thread has a 16.6 ms deadline and raises the frequency whenever frames run long. */
#include <dlfcn.h>
#include <time.h>
#include <sys/syscall.h>
typedef void *(*hint_mgr_fn)(void); typedef void *(*hint_new_fn)(void *, const int32_t *, size_t, int64_t); typedef int (*hint_rep_fn)(void *, int64_t);
static void *g_hint; static hint_rep_fn g_hint_rep; static uint64_t g_hint_last;
static void hint_start(void) {
  hint_mgr_fn gm = (hint_mgr_fn)dlsym(RTLD_DEFAULT, "APerformanceHint_getManager"); hint_new_fn cs = (hint_new_fn)dlsym(RTLD_DEFAULT, "APerformanceHint_createSession");
  g_hint_rep = (hint_rep_fn)dlsym(RTLD_DEFAULT, "APerformanceHint_reportActualWorkDuration");
  if (!gm || !cs || !g_hint_rep) { LOG("performance hint API not available"); return; }
  void *mgr = gm(); if (!mgr) return; int32_t tid = (int32_t)syscall(SYS_gettid);
  g_hint = cs(mgr, &tid, 1, 16666666); LOG("performance hint session %s", g_hint ? "created" : "refused");
}
#include "aot_settings.h"   /* EDIT-menu settings: fps cap (below) and the upscaler (applied to the client size) */
#include "aot_gpu.h"         /* Vulkan driver choice (system / bundled Turnip), applied before DXVK starts */
void aot_present_tick(void) {   /* once per presented frame, from the game thread */
  /* Frame-rate cap from the EDIT menu: hold each presented frame in its slot. A late frame does not build up a debt,
     and with the cap off nothing extra runs at all. */
  int cap = aot_settings_fps();
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  uint64_t now = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
  if (cap > 0) {
    static uint64_t next_ns; uint64_t step = 1000000000ull / (unsigned)cap;
    if (!next_ns || now > next_ns + step * 4) next_ns = now;   /* first frame, or resuming after a stall */
    if (next_ns > now) { struct timespec s; uint64_t d = next_ns - now; s.tv_sec = (time_t)(d / 1000000000ull); s.tv_nsec = (long)(d % 1000000000ull); nanosleep(&s, NULL); }
    next_ns += step;
    clock_gettime(CLOCK_MONOTONIC, &t); now = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
  }
  if (!g_hint) return;
  if (g_hint_last) g_hint_rep(g_hint, (int64_t)(now - g_hint_last)); g_hint_last = now;
}
static void *game_thread(void *arg) {
  (void)arg; hint_start();
  static int started; if (__atomic_exchange_n(&started, 1, __ATOMIC_SEQ_CST)) return NULL;   /* activity re-creation must not start a second game (it would fail to map the image again) */
  const char *ext = g_app->activity->externalDataPath; char dir[512], exe[600], lg[600];
  snprintf(dir, sizeof dir, "%s/game", ext); mkdir(ext, 0777); mkdir(dir, 0777);
  snprintf(exe, sizeof exe, "%s/game.exe", dir); snprintf(lg, sizeof lg, "%s/log.txt", ext);
  /* The driver menu stores its choice before game_dir.txt is written, so it is always on disk before the game can
     start: "<driver id> <nativeLibraryDir>". No file (first run, or an older install) keeps the system driver. */
  { char dp[700]; snprintf(dp, sizeof dp, "%s/driver.txt", ext);
    FILE *df = fopen(dp, "r");
    if (df) { int id = 0; char ld[512] = ""; if (fscanf(df, "%d %511s", &id, ld) == 2) { aot_gpu_init(ld, id); LOG("driver choice %d from %s", id, dp); } fclose(df); } }
  int fd = open(lg, O_WRONLY | O_CREAT | O_TRUNC, 0666); if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
  if (access(exe, R_OK) != 0) {   /* first run: copy bundled exe out of the APK */
    AAsset *a = AAssetManager_open(g_app->activity->assetManager, "game.exe", AASSET_MODE_STREAMING);
    FILE *o = a ? fopen(exe, "wb") : NULL; char buf[65536]; int n;
    if (a && o) { while ((n = AAsset_read(a, buf, sizeof buf)) > 0) fwrite(buf, 1, n, o); fclose(o); AAsset_close(a); }
    else { dprintf(2, "cannot extract game.exe from APK\n"); return NULL; }
  }
  /* wait until the Java side has stored the user-selected game folder (all-files access + folder picker) */
  char gd[600] = "", gp[700]; snprintf(gp, sizeof gp, "%s/game_dir.txt", ext);
  for (;;) { FILE *f = fopen(gp, "r"); if (f) { if (fgets(gd, sizeof gd, f)) gd[strcspn(gd, "\r\n")] = 0; fclose(f); if (gd[0]) break; } usleep(300000); }
  if (access(gd, R_OK | X_OK) != 0) { dprintf(2, "[aot] selected game folder is not readable: %s (grant All files access, then re-select)\n", gd); unlink(gp); return NULL; }
  snprintf(lg, sizeof lg, "%s/aot_log.txt", gd);   /* log goes into the game folder so it can be opened from any file manager */
  { int fd2 = open(lg, O_WRONLY | O_CREAT | O_TRUNC, 0666); if (fd2 < 0) { snprintf(lg, sizeof lg, "%s/log.txt", ext); fd2 = open(lg, O_WRONLY | O_CREAT | O_APPEND, 0666); } if (fd2 >= 0) { dup2(fd2, 1); dup2(fd2, 2); } }
  chdir(gd); LOG("starting %s with game data in %s (log: %s)", exe, gd, lg);
  dprintf(2, "[boot] game folder %s, log %s: entering the runtime\n", gd, lg);   /* dprintf is not silenced in release builds */
  char *av[] = { "aotgame", exe, NULL };
  int rc = aot_main(2, av);
  dprintf(2, "\n[aot] game returned %d\n", rc); LOG("game returned %d", rc);
  return NULL;
}

/* ---- input ---- */
static AotPad pad;
static int key_to_vk(int k) {
  if (k >= AKEYCODE_A && k <= AKEYCODE_Z) return 'A' + (k - AKEYCODE_A);
  if (k >= AKEYCODE_0 && k <= AKEYCODE_9) return '0' + (k - AKEYCODE_0);
  if (k >= AKEYCODE_F1 && k <= AKEYCODE_F12) return 0x70 + (k - AKEYCODE_F1);
  switch (k) {
    case AKEYCODE_DPAD_UP: return 0x26; case AKEYCODE_DPAD_DOWN: return 0x28; case AKEYCODE_DPAD_LEFT: return 0x25; case AKEYCODE_DPAD_RIGHT: return 0x27;
    case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: return 0x0D; case AKEYCODE_SPACE: return 0x20; case AKEYCODE_DEL: return 0x08; case AKEYCODE_TAB: return 0x09;
    case AKEYCODE_ESCAPE: return 0x1B; case AKEYCODE_SHIFT_LEFT: case AKEYCODE_SHIFT_RIGHT: return 0x10; case AKEYCODE_CTRL_LEFT: case AKEYCODE_CTRL_RIGHT: return 0x11;
    case AKEYCODE_ALT_LEFT: case AKEYCODE_ALT_RIGHT: return 0x12; case AKEYCODE_PAGE_UP: return 0x21; case AKEYCODE_PAGE_DOWN: return 0x22;
    case AKEYCODE_MOVE_HOME: return 0x24; case AKEYCODE_MOVE_END: return 0x23; case AKEYCODE_FORWARD_DEL: return 0x2E; case AKEYCODE_INSERT: return 0x2D;
  }
  return 0;
}
static uint16_t pad_bit(int k) {
  switch (k) {
    case AKEYCODE_BUTTON_A: return 0x1000; case AKEYCODE_BUTTON_B: return 0x2000; case AKEYCODE_BUTTON_X: return 0x4000; case AKEYCODE_BUTTON_Y: return 0x8000;
    case AKEYCODE_BUTTON_L1: return 0x100; case AKEYCODE_BUTTON_R1: return 0x200; case AKEYCODE_BUTTON_THUMBL: return 0x40; case AKEYCODE_BUTTON_THUMBR: return 0x80;
    case AKEYCODE_BUTTON_START: return 0x10; case AKEYCODE_BUTTON_SELECT: return 0x20;
    case AKEYCODE_DPAD_UP: return 1; case AKEYCODE_DPAD_DOWN: return 2; case AKEYCODE_DPAD_LEFT: return 4; case AKEYCODE_DPAD_RIGHT: return 8;
  }
  return 0;
}
static int16_t ax16(float v, int inv) { if (v > 1) v = 1; if (v < -1) v = -1; return (int16_t)((inv ? -v : v) * 32767.0f); }
static int32_t on_input(struct android_app *app, AInputEvent *e) {
  (void)app; int type = AInputEvent_getType(e), src = AInputEvent_getSource(e);
  int gamepad = (src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD || (src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
  if (type == AINPUT_EVENT_TYPE_KEY) {
    int k = AKeyEvent_getKeyCode(e), down = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN;
    if (k == AKEYCODE_BACK) { if (down) { aot_input_key(0x1B, 1); aot_input_key(0x1B, 0); } return 1; }
    uint16_t pb = pad_bit(k);
    if (pb && (gamepad || (k >= AKEYCODE_BUTTON_A && k <= AKEYCODE_BUTTON_SELECT))) {
      if (down) pad.buttons |= pb; else pad.buttons &= ~pb; aot_input_pad(&pad); return 1;
    }
    if (k == AKEYCODE_BUTTON_L2) { pad.lt = down ? 255 : 0; aot_input_pad(&pad); return 1; }
    if (k == AKEYCODE_BUTTON_R2) { pad.rt = down ? 255 : 0; aot_input_pad(&pad); return 1; }
    int vk = key_to_vk(k); if (vk && AKeyEvent_getRepeatCount(e) == 0) { aot_input_key(vk, down); return 1; }
    return 0;
  }
  if (type == AINPUT_EVENT_TYPE_MOTION) {
    if (gamepad && !(src & AINPUT_SOURCE_TOUCHSCREEN)) {
      pad.lx = ax16(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0), 0); pad.ly = ax16(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0), 1);
      pad.rx = ax16(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0), 0); pad.ry = ax16(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0), 1);
      float lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0), rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0);
      if (lt == 0 && rt == 0) { lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0); rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0); }
      pad.lt = (uint8_t)(lt * 255.f); pad.rt = (uint8_t)(rt * 255.f);
      float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0), hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
      pad.buttons &= ~0xF; if (hy < -0.5f) pad.buttons |= 1; if (hy > 0.5f) pad.buttons |= 2; if (hx < -0.5f) pad.buttons |= 4; if (hx > 0.5f) pad.buttons |= 8;
      aot_input_pad(&pad); return 1;
    }
    /* touch -> Win32 mouse: a finger is look travel only (no button), so swiping can never click. */
    int a = AMotionEvent_getAction(e), act = a & AMOTION_EVENT_ACTION_MASK, idx = (a & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    float x = AMotionEvent_getX(e, 0), y = AMotionEvent_getY(e, 0);
    if (g_app->window) aot_input_view((uint32_t)ANativeWindow_getWidth(g_app->window), (uint32_t)ANativeWindow_getHeight(g_app->window));
    switch (act) {
      /* A finger on the surface only moves the pointer: clicks come from the on-screen buttons (the Java
         overlay in GameActivity.ControlsView) and from a physical gamepad, so dragging to look around can
         never fire the weapon, open a door or pick a menu entry. */
      case AMOTION_EVENT_ACTION_DOWN: aot_input_mouse(5, x, y); break;   /* gesture start, then travel = look */
      case AMOTION_EVENT_ACTION_MOVE: case AMOTION_EVENT_ACTION_POINTER_DOWN: case AMOTION_EVENT_ACTION_POINTER_UP:
        aot_input_mouse(0, x, y); break;
      case AMOTION_EVENT_ACTION_UP: case AMOTION_EVENT_ACTION_CANCEL:
        aot_input_mouse(6, x, y); break;   /* gesture end: no pending offset is left to snap the view */
    }
    return 1;
  }
  return 0;
}
/* Called by the Java on-screen controller overlay (GameActivity.ControlsView). */
JNIEXPORT void JNICALL Java_com_aot_game_GameActivity_nativeKey0(JNIEnv *env, jclass cls, jint vk, jint down) { (void)env; (void)cls; { static int n; if (n++ < 200) LOG("overlay key vk=%d down=%d", vk, down); } aot_input_key(vk, down); }
JNIEXPORT void JNICALL Java_com_aot_game_GameActivity_nativeMouse0(JNIEnv *env, jclass cls, jint kind, jfloat x, jfloat y) { (void)env; (void)cls; { static int n; if (n++ < 200) LOG("overlay mouse kind=%d at (%d,%d)", kind, (int)x, (int)y); } aot_input_mouse(kind, x, y); }
/* A physical gamepad delivers its events to the Activity, not to this native input queue, so GameActivity
   forwards them here: the same AotPad the native path fills, so XInputGetState sees one device either way. */
JNIEXPORT void JNICALL Java_com_aot_game_GameActivity_nativePadPresent0(JNIEnv *env, jclass cls, jint on) {
  (void)env; (void)cls; aot_input_pad_present(on != 0);
}
JNIEXPORT void JNICALL Java_com_aot_game_GameActivity_nativePad0(JNIEnv *env, jclass cls, jint buttons, jint lt, jint rt, jint lx, jint ly, jint rx, jint ry) {
  (void)env; (void)cls;
  AotPad p; memset(&p, 0, sizeof p);
  p.buttons = (uint16_t)buttons; p.lt = (uint8_t)lt; p.rt = (uint8_t)rt;
  p.lx = (int16_t)lx; p.ly = (int16_t)ly; p.rx = (int16_t)rx; p.ry = (int16_t)ry;
  pad = p; aot_input_pad(&pad);
}
extern volatile uint64_t aot_frames, aot_ns[4];
JNIEXPORT jlong JNICALL Java_com_aot_game_GameActivity_nativeNs0(JNIEnv *env, jclass cls, jint k) { (void)env; (void)cls; return k >= 0 && k < 4 ? (jlong)aot_ns[k] : 0; }
JNIEXPORT jlong JNICALL Java_com_aot_game_GameActivity_nativeFrames0(JNIEnv *env, jclass cls) { (void)env; (void)cls; return (jlong)aot_frames; }
JNIEXPORT void JNICALL Java_com_aot_game_GameActivity_nativeSettings0(JNIEnv *env, jclass cls, jint fps, jint upscale, jint quality) {
  (void)env; (void)cls; aot_settings_set((int)fps, (int)upscale, (int)quality);
}
/* Vulkan driver from the menu: 0 = the phone's own driver, 1 = bundled Turnip for Adreno 6xx/7xx, 2 = for Adreno 8xx.
   Takes effect the next time the game starts; a driver that does not come up is replaced by the system one. */
JNIEXPORT void JNICALL Java_com_aot_game_GameActivity_nativeDriver0(JNIEnv *env, jclass cls, jint id, jstring libdir) {
  (void)cls; char ld[512] = "";
  if (libdir) { const char *s = (*env)->GetStringUTFChars(env, libdir, NULL); if (s) { snprintf(ld, sizeof ld, "%s", s); (*env)->ReleaseStringUTFChars(env, libdir, s); } }
  aot_gpu_init(ld, (int)id);
  LOG("driver choice %d (native libs %s)", (int)id, ld[0] ? ld : "?");
}
JNIEXPORT void JNICALL Java_com_aot_game_GameActivity_nativeView0(JNIEnv *env, jclass cls, jint w, jint h) { (void)env; (void)cls; LOG("overlay view size %dx%d", w, h); aot_input_view((uint32_t)w, (uint32_t)h); }
static void on_cmd(struct android_app *app, int32_t cmd) {
  if (cmd == APP_CMD_INIT_WINDOW) aot_android_set_window(app->window);
  else if (cmd == APP_CMD_TERM_WINDOW) aot_android_set_window(NULL);
  else if (cmd == APP_CMD_DESTROY) aot_input_quit();
}
void android_main(struct android_app *app) {
  g_app = app; app->onAppCmd = on_cmd; app->onInputEvent = on_input;
  pthread_t th; pthread_create(&th, NULL, game_thread, NULL);
  for (;;) { int ev; struct android_poll_source *src;
    while (ALooper_pollAll(100, NULL, &ev, (void **)&src) >= 0) { if (src) src->process(app, src); if (app->destroyRequested) return; } }
}
#else
void android_main(struct android_app *app) {
  for (;;) { int ev; struct android_poll_source *src;
    while (ALooper_pollAll(100, NULL, &ev, (void **)&src) >= 0) { if (src) src->process(app, src); if (app->destroyRequested) return; } }
}
#endif
