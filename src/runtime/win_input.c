/* Win32 window classes, message queue/loop, keyboard/mouse state, XInput. Fed by the platform via aot_input_*(). */
#define _GNU_SOURCE
#include "shims.h"
#include "win_input.h"
#include <pthread.h>
#include <time.h>

#define A(n) argn(c, (n))
static inline uint64_t argn(CPU *c, int n) { return n == 0 ? c->r[1] : n == 1 ? c->r[2] : n == 2 ? c->r[8] : n == 3 ? c->r[9] : LD(uint64_t, c->r[4] + 8 + 8 * n); }
#define SYS(n) static uint64_t in_##n(CPU *c)
#define E(n) { #n, in_##n }
#define EA(n, f) { n, in_##f }

enum { WM_CREATE = 1, WM_DESTROY = 2, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_PAINT = 0xF, WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_ERASEBKGND = 0x14, WM_ACTIVATEAPP = 0x1C,
       WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202,
       WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_MOUSEWHEEL = 0x20A, WM_TIMER = 0x113 };
#define HWND_BASE 0x10001ull
typedef struct { uint64_t hwnd, wparam, lparam; uint32_t msg, time; int32_t x, y; } Msg;
#define QN 512
static Msg q[QN]; static int qh, qt;
static pthread_mutex_t M = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t QC = PTHREAD_COND_INITIALIZER;
static uint8_t keys[256]; static int32_t mx, my; static uint32_t view_w = 1, view_h = 1; static int view_known; static int quit_posted; static uint32_t shown;
static uint32_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static void remove_at(int i) {   /* M held: uncouple slot i, keeping the remaining order */
  for (int j = i; j != qh; j = (j + QN - 1) % QN) q[j] = q[(j + QN - 1) % QN];
  qh = (qh + 1) % QN;
}
static void post(uint64_t hwnd, uint32_t msg, uint64_t w, uint64_t l) {
  pthread_mutex_lock(&M);
  /* Windows keeps at most one pending WM_PAINT per window.  An idle message loop synthesises one every
     16 ms, so without coalescing a screen that is waiting for a tap fills all 512 slots with paint and
     the tap itself is dropped on a full ring - which is exactly "the touch controls silently stopped". */
  if (msg == WM_PAINT) {
    for (int i = qh; i != qt; i = (i + 1) % QN) if (q[i].msg == WM_PAINT) { pthread_mutex_unlock(&M); return; }
  }
  if ((qt + 1) % QN == qh) {
    /* Full: a stale move or an already coalesced paint is worth less than the input that just arrived.
       The cursor position lives in mx/my, so losing a WM_MOUSEMOVE loses nothing. */
    int victim = -1;
    for (int i = qh; i != qt; i = (i + 1) % QN) {
      if (q[i].msg == WM_MOUSEMOVE) { victim = i; break; }
      if (victim < 0 && q[i].msg == WM_PAINT) victim = i;
    }
    if (victim < 0) {
      static int n; if (n++ < 20) fprintf(stderr, "[in] queue full (%d slots), nothing droppable; message %#x lost\n", QN, msg);
      pthread_mutex_unlock(&M); return;
    }
    uint32_t vmsg = q[victim].msg; remove_at(victim);
    { static int n; if (n++ < 20) fprintf(stderr, "[in] queue full: dropped queued %#x to keep %#x\n", vmsg, msg); }
  }
  q[qt] = (Msg){ hwnd ? hwnd : HWND_BASE, w, l, msg, now_ms(), mx, my }; qt = (qt + 1) % QN;
  pthread_cond_broadcast(&QC); pthread_mutex_unlock(&M);
}

/* ---- window classes / windows ---- */
typedef struct { char name[64]; uint64_t proc; int used; } Class;
typedef struct { uint64_t proc; int used, ansi; int64_t user; char title[128]; uint64_t style; } Win;
static Class classes[32]; static Win wins[16];
static void cname(char *out, uint64_t p, int wide) {
  out[0] = 0; if (!p) return;
  if (p < 0x10000) { snprintf(out, 64, "#%u", (unsigned)p); return; }
  if (wide) { const uint16_t *w = (const uint16_t *)(uintptr_t)p; int i = 0; for (; i < 63 && w[i]; i++) out[i] = (char)w[i]; out[i] = 0; }
  else snprintf(out, 64, "%.63s", (const char *)(uintptr_t)p);
}
static uint64_t reg(CPU *c, int wide, int ex) {
  uint64_t wc = A(0); char nm[64]; uint64_t cp, pr;   /* WNDCLASS(EX) layouts: PE32 ex proc@8 name@40, plain proc@4 name@36 */
  if (aot_is_pe32) { cp = LD(uint32_t, wc + (ex ? 40 : 36)); pr = LD(uint32_t, wc + (ex ? 8 : 4)); } else { cp = LD(uint64_t, wc + 64); pr = LD(uint64_t, wc + 8); }
  cname(nm, cp, wide);
  for (int i = 0; i < 32; i++) if (!classes[i].used) { classes[i].used = 1; classes[i].proc = pr; snprintf(classes[i].name, 64, "%s", nm); (void)ex; return 0xC000 + i; }
  return 0;
}
SYS(RegisterClassExA) { return reg(c, 0, 1); }
SYS(RegisterClassExW) { return reg(c, 1, 1); }
SYS(RegisterClassA) { return reg(c, 0, 0); }
SYS(RegisterClassW) { return reg(c, 1, 0); }
static uint64_t call_proc(CPU *c, Win *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) { return w && w->proc ? aot_call(c, w->proc, 4, hwnd, msg, wp, lp) : 0; }
static Win *win_of(uint64_t h) { if (h < HWND_BASE) return NULL; uint64_t i = h - HWND_BASE; return i < 16 && wins[i].used ? &wins[i] : NULL; }
static uint64_t create(CPU *c, int wide) {
  char nm[64]; cname(nm, A(1), wide); uint64_t proc = 0;
  for (int i = 0; i < 32; i++) if (classes[i].used && (A(1) == 0xC000u + (uint64_t)i || !strcmp(classes[i].name, nm))) proc = classes[i].proc;
  for (int i = 0; i < 16; i++) if (!wins[i].used) {
    Win *w = &wins[i]; memset(w, 0, sizeof *w); w->used = 1; w->proc = proc; w->ansi = !wide; w->style = (uint32_t)A(3);
    uint64_t h = HWND_BASE + (uint64_t)i; uint8_t *cs = aot_gcalloc(1, 80);   /* CREATESTRUCT must live below 4 GB for PE32 guests */
    if (aot_is_pe32) { ST(uint32_t, cs, (uint32_t)A(11)); ST(int32_t, cs + 16, (int32_t)A(7)); ST(int32_t, cs + 20, (int32_t)A(6)); ST(int32_t, cs + 24, (int32_t)A(5)); ST(int32_t, cs + 28, (int32_t)A(4)); ST(uint32_t, cs + 32, (uint32_t)A(3)); }
    else { ST(uint64_t, cs, A(11)); ST(int32_t, cs + 40, (int32_t)A(7)); ST(int32_t, cs + 44, (int32_t)A(6)); ST(uint32_t, cs + 48, (uint32_t)A(3)); }
    call_proc(c, w, h, WM_NCCREATE, 0, (uint64_t)(uintptr_t)cs); call_proc(c, w, h, WM_CREATE, 0, (uint64_t)(uintptr_t)cs); aot_gfree(cs); return h;
  }
  return 0;
}
SYS(CreateWindowExA) { return create(c, 0); }
SYS(CreateWindowExW) { return create(c, 1); }
SYS(ShowWindow) {
  if (!shown++) { uint32_t w, h; aot_win_client_size(&w, &h); uint64_t hw = A(0);
    post(hw, WM_ACTIVATEAPP, 1, 0); post(hw, WM_ACTIVATE, 1, 0); post(hw, WM_SETFOCUS, 0, 0); post(hw, WM_SIZE, 0, (uint64_t)(w & 0xffff) | ((uint64_t)(h & 0xffff) << 16)); post(hw, WM_PAINT, 0, 0); }
  return 0;
}
SYS(DestroyWindow) { Win *w = win_of(A(0)); if (!w) return 0; call_proc(c, w, A(0), WM_DESTROY, 0, 0); call_proc(c, w, A(0), WM_NCDESTROY, 0, 0); w->used = 0; return 1; }
SYS(IsWindow) { return win_of(A(0)) != NULL; }
SYS(DefWindowProc) {
  uint32_t m = (uint32_t)A(1);
  if (m == WM_CLOSE) { in_DestroyWindow(c); return 0; }
  if (m == WM_NCCREATE || m == WM_ERASEBKGND) return 1;
  return 0;
}
SYS(GetWindowLongPtr) { Win *w = win_of(A(0)); int32_t i = (int32_t)A(1); if (!w) return 0; if (i == -21) return (uint64_t)w->user; if (i == -4) return w->proc; if (i == -16) return w->style; return 0; }
SYS(SetWindowLongPtr) { Win *w = win_of(A(0)); int32_t i = (int32_t)A(1); if (!w) return 0; uint64_t old = 0; if (i == -21) { old = (uint64_t)w->user; w->user = (int64_t)A(2); } else if (i == -4) { old = w->proc; w->proc = A(2); } else if (i == -16) { old = w->style; w->style = A(2); } return old; }
SYS(SetWindowTextA) { Win *w = win_of(A(0)); if (w) snprintf(w->title, sizeof w->title, "%.127s", (const char *)(uintptr_t)A(1)); return 1; }
SYS(GetWindowTextA) { return 0; }

/* ---- message pump ---- */
static int take(Msg *out, uint64_t hwnd, uint32_t lo, uint32_t hi, int remove) {   /* M held */
  for (int i = qh; i != qt; i = (i + 1) % QN) {
    Msg *m = &q[i]; if ((hwnd && m->hwnd != hwnd && m->msg != WM_QUIT) || (lo | hi) && (m->msg < lo || m->msg > hi)) continue;
    *out = *m; if (remove) { for (int j = i; j != qh; j = (j + QN - 1) % QN) q[j] = q[(j + QN - 1) % QN]; qh = (qh + 1) % QN; } return 1;
  }
  return 0;
}
static void fill(uint64_t p, const Msg *m) {
  if (aot_is_pe32) { memset((void *)(uintptr_t)p, 0, 28); ST(uint32_t, p, (uint32_t)m->hwnd); ST(uint32_t, p + 4, m->msg); ST(uint32_t, p + 8, (uint32_t)m->wparam); ST(uint32_t, p + 12, (uint32_t)m->lparam); ST(uint32_t, p + 16, m->time); ST(int32_t, p + 20, m->x); ST(int32_t, p + 24, m->y); return; }
   memset((void *)(uintptr_t)p, 0, 48); ST(uint64_t, p, m->hwnd); ST(uint32_t, p + 8, m->msg); ST(uint64_t, p + 16, m->wparam); ST(uint64_t, p + 24, m->lparam); ST(uint32_t, p + 32, m->time); ST(int32_t, p + 36, m->x); ST(int32_t, p + 40, m->y); }
SYS(PeekMessage) {
  Msg m; pthread_mutex_lock(&M); int ok = take(&m, A(1), (uint32_t)A(2), (uint32_t)A(3), (A(4) & 1)); pthread_mutex_unlock(&M);
  if (ok) fill(A(0), &m); return ok;
}
SYS(GetMessage) {
  Msg m; struct timespec dl;
  pthread_mutex_lock(&M);
  for (;;) {
    if (take(&m, A(1), (uint32_t)A(2), (uint32_t)A(3), 1)) break;
    clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 16000000L; if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    if (pthread_cond_timedwait(&QC, &M, &dl)) { m = (Msg){ A(1) ? A(1) : HWND_BASE, 0, 0, WM_PAINT, now_ms(), mx, my }; break; }   /* idle: synthesise WM_PAINT */
  }
  pthread_mutex_unlock(&M); fill(A(0), &m); return m.msg == WM_QUIT ? 0 : 1;
}
SYS(TranslateMessage) {
  uint64_t p = A(0); uint32_t m = LD(uint32_t, p + (aot_is_pe32 ? 4 : 8)), vk = aot_is_pe32 ? LD(uint32_t, p + 8) : (uint32_t)LD(uint64_t, p + 16);
  if (m == WM_KEYDOWN) { int ch = 0; int sh = keys[0x10] & 0x80;
    if (vk >= 'A' && vk <= 'Z') ch = sh ? (int)vk : (int)vk + 32; else if (vk >= '0' && vk <= '9') ch = (int)vk; else if (vk == 0x20) ch = ' '; else if (vk == 0x0D) ch = '\r'; else if (vk == 0x08) ch = 8; else if (vk == 0x1B) ch = 27;
    if (ch) { post(aot_is_pe32 ? LD(uint32_t, p) : LD(uint64_t, p), WM_CHAR, (uint64_t)ch, 1); return 1; } }
  return 0;
}
SYS(DispatchMessage) {
  uint64_t p = A(0); int x = aot_is_pe32; uint64_t h = x ? LD(uint32_t, p) : LD(uint64_t, p); uint32_t m = LD(uint32_t, p + (x ? 4 : 8)); Win *w = win_of(h);
  if (m == WM_PAINT && w && !w->proc) return 0;
  return call_proc(c, w, h, m, x ? LD(uint32_t, p + 8) : LD(uint64_t, p + 16), x ? LD(uint32_t, p + 12) : LD(uint64_t, p + 24));
}
SYS(SendMessage) { return call_proc(c, win_of(A(0)), A(0), (uint32_t)A(1), A(2), A(3)); }
SYS(PostMessage) { post(A(0), (uint32_t)A(1), A(2), A(3)); return 1; }
SYS(PostQuitMessage) { quit_posted = 1; post(HWND_BASE, WM_QUIT, A(0), 0); return 0; }
SYS(PostThreadMessage) { post(0, (uint32_t)A(1), A(2), A(3)); return 1; }

/* ---- keyboard / mouse state ---- */
SYS(GetAsyncKeyState) { uint32_t k = (uint32_t)A(0) & 0xff; return keys[k] ? 0x8000 : 0; }
SYS(GetKeyState) { uint32_t k = (uint32_t)A(0) & 0xff; return keys[k] ? 0x8000 : 0; }
SYS(GetKeyboardState) { uint8_t *o = (uint8_t *)(uintptr_t)A(0); for (int i = 0; i < 256; i++) o[i] = keys[i] ? 0x80 : 0; return 1; }
SYS(GetCursorPos) { if (A(0)) { ST(int32_t, A(0), mx); ST(int32_t, A(0) + 4, my); } return 1; }
SYS(SetCursorPos) { mx = (int32_t)A(0); my = (int32_t)A(1); return 1; }
SYS(ScreenToClient) { return 1; }
SYS(one) { return 1; }
SYS(zero) { return 0; }
SYS(hwnd) { return HWND_BASE; }
SYS(MapVirtualKey) { return (uint32_t)A(0); }
SYS(RegisterWindowMessage) { return 0xC100; }
SYS(GetMessageTime) { return now_ms(); }
SYS(BeginPaint) { if (A(1)) memset((void *)(uintptr_t)A(1), 0, 72); return 1; }

void aot_input_view(uint32_t w, uint32_t h) {
  static int logged; view_w = w ? w : 1; view_h = h ? h : 1; view_known = (w && h) != 0;
  if (view_known && !logged++) fprintf(stderr, "[in] surface size %ux%u (touch coordinates are scaled into the client area)\n", w, h);
}
void aot_input_mouse(int kind, float x, float y) {   /* kind: 0 move, 1 left down, 2 left up, 3 right down, 4 right up */
  if (kind < 0 || kind > 4) { fprintf(stderr, "[in] mouse: rejected event kind %d\n", kind); return; }
  uint32_t cw, ch; aot_win_client_size(&cw, &ch);
  /* Without a reported surface size the incoming coordinates are already surface pixels; scaling them
     by client/1 put every tap thousands of pixels off-screen, so a press that should hit a button missed. */
  if (view_known) { mx = (int32_t)(x * (float)cw / (float)view_w); my = (int32_t)(y * (float)ch / (float)view_h); }
  else { mx = (int32_t)x; my = (int32_t)y; }
  uint64_t lp = (uint64_t)(uint16_t)mx | ((uint64_t)(uint16_t)my << 16), wp = (keys[1] ? 1 : 0) | (keys[2] ? 2 : 0);
  static const uint32_t mm[] = { WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN, WM_RBUTTONUP };
  if (kind == 1) { keys[1] = 1; wp |= 1; } else if (kind == 2) { keys[1] = 0; wp &= ~1ull; } else if (kind == 3) { keys[2] = 1; wp |= 2; } else if (kind == 4) { keys[2] = 0; wp &= ~2ull; }
  post(HWND_BASE, mm[kind], wp, lp);
  { static int n; if (n++ < 300) fprintf(stderr, "[in] mouse kind=%d raw=(%.1f,%.1f) -> (%d,%d) view=%ux%u known=%d client=%ux%u msg=%#x\n",
      kind, x, y, mx, my, view_w, view_h, view_known, cw, ch, mm[kind]); }
}
void aot_input_key(int vk, int down) {
  if (vk < 0 || vk > 255) { fprintf(stderr, "[in] key: rejected vk=%d\n", vk); return; }
  keys[vk] = (uint8_t)down; post(HWND_BASE, down ? WM_KEYDOWN : WM_KEYUP, (uint64_t)vk, down ? 1 : 0xC0000001u);
  { static int n; if (n++ < 300) fprintf(stderr, "[in] key vk=%d (%#x) down=%d\n", vk, (unsigned)vk, down); }
}
void aot_input_quit(void) { quit_posted = 1; post(HWND_BASE, WM_QUIT, 0, 0); }


/* ---- DirectInput feed (keyboard scancodes, relative mouse) for the dinput8 shim in win_x86_dll.c ---- */
static uint8_t vk2dik[256];
static void dik_init(void) {
  if (vk2dik['A']) return;
  const char *r1 = "QWERTYUIOP", *r2 = "ASDFGHJKL", *r3 = "ZXCVBNM";
  for (int i = 0; r1[i]; i++) vk2dik[(int)r1[i]] = (uint8_t)(16 + i);
  for (int i = 0; r2[i]; i++) vk2dik[(int)r2[i]] = (uint8_t)(30 + i);
  for (int i = 0; r3[i]; i++) vk2dik[(int)r3[i]] = (uint8_t)(44 + i);
  for (int i = 0; i < 9; i++) vk2dik['1' + i] = (uint8_t)(2 + i);
  vk2dik['0'] = 11; vk2dik[0x1B] = 1; vk2dik[0x08] = 14; vk2dik[0x09] = 15; vk2dik[0x0D] = 28; vk2dik[0x11] = 29; vk2dik[0x10] = 42; vk2dik[0x12] = 56; vk2dik[0x20] = 57;
  for (int i = 0; i < 10; i++) vk2dik[0x70 + i] = (uint8_t)(59 + i);
  vk2dik[0x7A] = 87; vk2dik[0x7B] = 88; vk2dik[0x26] = 200; vk2dik[0x25] = 203; vk2dik[0x27] = 205; vk2dik[0x28] = 208; vk2dik[0x24] = 199; vk2dik[0x23] = 207;
  vk2dik[0x21] = 201; vk2dik[0x22] = 209; vk2dik[0x2D] = 210; vk2dik[0x2E] = 211;
}
void aot_input_dik(uint8_t out[256]) {
  dik_init(); memset(out, 0, 256); for (int v = 3; v < 256; v++) if (keys[v] && vk2dik[v]) out[vk2dik[v]] = 0x80;
  { static int n; if (n++ < 150) { int down = 0; for (int i = 0; i < 256; i++) if (out[i]) down++;
      fprintf(stderr, "[in] GetDeviceState(keyboard): %d key(s) reported down\n", down); } }
}
void aot_input_dimouse(int32_t *dx, int32_t *dy, uint8_t b[3]) {
  static int32_t lx, ly; static int have; int32_t x = mx, y = my;
  *dx = have ? x - lx : 0; *dy = have ? y - ly : 0; lx = x; ly = y; have = 1; b[0] = keys[1] ? 0x80 : 0; b[1] = keys[2] ? 0x80 : 0; b[2] = keys[4] ? 0x80 : 0;
  { static int n; if (n++ < 150) fprintf(stderr, "[in] GetDeviceState/GetDeviceData(mouse): pos=(%d,%d) delta=(%d,%d) buttons=%02x%02x%02x\n",
      x, y, *dx, *dy, b[0], b[1], b[2]); }
}
/* ---- XInput ---- */
static AotPad pad; static int pad_on; static uint32_t pad_packet;
void aot_input_pad(const AotPad *p) { if (!p) { pad_on = 0; return; } if (!pad_on || memcmp(&pad, p, sizeof pad)) pad_packet++; pad = *p; pad_on = 1; }
SYS(XInputGetState) {
  if ((uint32_t)A(0) != 0 || !pad_on) return 1167;
  uint64_t o = A(1); memset((void *)(uintptr_t)o, 0, 16); ST(uint32_t, o, pad_packet); ST(uint16_t, o + 4, pad.buttons); ST(uint8_t, o + 6, pad.lt); ST(uint8_t, o + 7, pad.rt);
  ST(int16_t, o + 8, pad.lx); ST(int16_t, o + 10, pad.ly); ST(int16_t, o + 12, pad.rx); ST(int16_t, o + 14, pad.ry); return 0;
}
SYS(XInputSetState) { return (uint32_t)A(0) == 0 && pad_on ? 0 : 1167; }
SYS(XInputGetCapabilities) {
  if ((uint32_t)A(0) != 0 || !pad_on) return 1167;
  uint64_t o = A(2); memset((void *)(uintptr_t)o, 0, 20); ST(uint8_t, o, 1); ST(uint8_t, o + 1, 1); ST(uint16_t, o + 4, 0xF3FF); ST(uint8_t, o + 6, 255); ST(uint8_t, o + 7, 255);
  ST(int16_t, o + 8, 32767); ST(int16_t, o + 10, 32767); ST(int16_t, o + 12, 32767); ST(int16_t, o + 14, 32767); return 0;
}
SYS(XInputGetBatteryInformation) { return 1167; }

const ShimEnt aot_shim_table_input[] = {
  E(RegisterClassExA), E(RegisterClassExW), E(RegisterClassA), E(RegisterClassW), E(CreateWindowExA), E(CreateWindowExW), E(ShowWindow), E(DestroyWindow), E(IsWindow),
  EA("DefWindowProcA", DefWindowProc), EA("DefWindowProcW", DefWindowProc), EA("GetWindowLongPtrA", GetWindowLongPtr), EA("GetWindowLongPtrW", GetWindowLongPtr), EA("GetWindowLongA", GetWindowLongPtr),
  EA("GetWindowLongW", GetWindowLongPtr), EA("SetWindowLongPtrA", SetWindowLongPtr), EA("SetWindowLongPtrW", SetWindowLongPtr), EA("SetWindowLongA", SetWindowLongPtr), EA("SetWindowLongW", SetWindowLongPtr),
  EA("SetWindowTextA", SetWindowTextA), EA("GetWindowTextA", GetWindowTextA),
  EA("PeekMessageA", PeekMessage), EA("PeekMessageW", PeekMessage), EA("GetMessageA", GetMessage), EA("GetMessageW", GetMessage), E(TranslateMessage), EA("DispatchMessageA", DispatchMessage), EA("DispatchMessageW", DispatchMessage),
  EA("SendMessageA", SendMessage), EA("SendMessageW", SendMessage), EA("PostMessageA", PostMessage), EA("PostMessageW", PostMessage), E(PostQuitMessage), EA("PostThreadMessageA", PostThreadMessage), EA("PostThreadMessageW", PostThreadMessage),
  E(GetAsyncKeyState), E(GetKeyState), E(GetKeyboardState), E(GetCursorPos), E(SetCursorPos), EA("ScreenToClient", ScreenToClient), EA("ClientToScreen", ScreenToClient),
  EA("MapVirtualKeyA", MapVirtualKey), EA("MapVirtualKeyW", MapVirtualKey), EA("RegisterWindowMessageA", RegisterWindowMessage), EA("RegisterWindowMessageW", RegisterWindowMessage), E(GetMessageTime),
  EA("SetCapture", hwnd), EA("ReleaseCapture", one), EA("SetCursor", zero), EA("GetFocus", hwnd), EA("IsWindowVisible", one), EA("GetParent", zero), EA("InvalidateRect", one), EA("ValidateRect", one),
  E(BeginPaint), EA("EndPaint", one), EA("SetTimer", one), EA("KillTimer", one), EA("EnableWindow", one), EA("IsZoomed", zero), EA("GetDesktopWindow", hwnd), EA("GetCapture", zero), EA("TrackMouseEvent", one),
  E(XInputGetState), E(XInputSetState), E(XInputGetCapabilities), E(XInputGetBatteryInformation), EA("XInputEnable", zero),
  { 0, 0 }
};
