/* PE32 imports from the remaining system DLLs (user32 extras, advapi32, ole32, oleaut32, shell32, iphlpapi, wininet, winmm,
 * ws2_32, dinput8, dsound, eax, xinput1_3, d3dx10_36, binkw32). Each entry is either a real implementation or an explicit,
 * documented failure result (never a silent success). Stdcall pop sizes are per the Win32 headers.
 * Tables are parsed by src/tools/pe_dependency_inspect.py: R(dll,name,pop) = reuse existing shim, U(dll,name,pop) = u_<name> here,
 * UO(dll,"#ord",pop,fn) = ordinal import. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "shims.h"
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>
#include <sys/syscall.h>

#define A(n) argn(c, (n))
static inline uint64_t argn(CPU *c, int n) { return n == 0 ? c->r[1] : n == 1 ? c->r[2] : n == 2 ? c->r[8] : n == 3 ? c->r[9] : LD(uint64_t, c->r[4] + 8 + 8 * n); }
#define P(n) ((void *)(uintptr_t)(uint32_t)A(n))
#define S(n) uint64_t u_##n(CPU *c)
static void note_once(int *flag, const char *msg) { if (!__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED)) { fflush(stdout); fprintf(stderr, "[aot] %s\n", msg); } }

/* ---- user32 extras (the rest reuse win_input.c / win_gfx.c) ---- */
S(SetWindowsHookExA) { static int n; note_once(&n, "SetWindowsHookExA: low-level hooks are not supported; returning failure"); aot_last_error = 1427; return 0; }
S(UnhookWindowsHookEx) { return 0; }
S(CallNextHookEx) { return 0; }   /* no next hook in the chain */
S(LoadStringA) { char *b = P(2); int n = (int)A(3); if (b && n > 0) b[0] = 0; aot_last_error = 1814; return 0; }   /* no string resources: ERROR_RESOURCE_NAME_NOT_FOUND */
S(SystemParametersInfoA) { aot_last_error = 87; return 0; }
S(ToUnicode) {   /* US-layout ASCII only: (vk, scan, keystate*, buf*, cch, flags) */
  uint32_t vk = (uint32_t)A(0); const uint8_t *ks = P(2); uint16_t *b = P(3); int sh = ks && (ks[0x10] & 0x80), ch = 0;
  if (vk >= 'A' && vk <= 'Z') ch = sh ? (int)vk : (int)vk + 32; else if (vk >= '0' && vk <= '9') ch = (int)vk; else if (vk == 0x20) ch = ' ';
  if (!ch || (int)A(4) < 1) return 0; b[0] = (uint16_t)ch; return 1;
}

/* ---- advapi32 ---- */
S(CryptAcquireContextA) { if (P(0)) ST(uint32_t, A(0), 1); return 1; }
S(CryptGenRandom) { uint8_t *b = P(2); uint32_t n = (uint32_t)A(1), got = 0; while (got < n) { ssize_t r = syscall(SYS_getrandom, b + got, n - got, 0); if (r <= 0) return 0; got += (uint32_t)r; } return 1; }
S(CryptReleaseContext) { return 1; }
S(GetUserNameA) { char *b = P(0); uint32_t *sz = P(1); if (!sz) return 0; if (!b || *sz < 7) { *sz = 7; aot_last_error = 122; return 0; } memcpy(b, "player", 7); *sz = 7; return 1; }
/* Registry: only the vendor key a normal install creates (HKLM\\SOFTWARE\\Ubisoft\\...) exists, with the install-directory values pointing at the
 * folder the game runs from. Everything else fails honestly (ERROR_FILE_NOT_FOUND) and is logged so missing keys are visible. */
#define REGH_BASE 0x7e000000u
static char reg_open_path[16][160]; static int reg_open_n;
static int reg_vendor_key(const char *p) { return !strncasecmp(p, "software\\ubisoft", 17) || !strncasecmp(p, "software\\wow6432node\\ubisoft", 30); }
S(RegOpenKeyExA) {
  const char *sub = P(1) ? (const char *)P(1) : ""; uint32_t root = (uint32_t)A(0);
  fflush(stdout); fprintf(stderr, "[aot] RegOpenKeyEx root=0x%x \"%s\"\n", root, sub);
  if ((root == 0x80000002u || root == 0x80000001u) && reg_vendor_key(sub) && reg_open_n < 16 && P(4)) {
    snprintf(reg_open_path[reg_open_n], sizeof reg_open_path[0], "%s", sub); ST(uint32_t, A(4), REGH_BASE + (uint32_t)reg_open_n++); return 0;
  }
  return 2;
}
S(RegQueryValueExA) {
  uint32_t h = (uint32_t)A(0); const char *nm = P(1) ? (const char *)P(1) : "";
  static const char *dirs[] = { "installdir", "install dir", "installpath", "path", "gamepath", "rootdir", "installationdir", "installlocation" };
  int ok = 0; if (h >= REGH_BASE && h < REGH_BASE + 16) for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++) if (!strcasecmp(nm, dirs[i])) ok = 1;
  fflush(stdout); fprintf(stderr, "[aot] RegQueryValueEx \"%s\" -> %s\n", nm, ok ? "game folder" : "not found");
  if (!ok) return 2;
  char dir[512]; if (!getcwd(dir, sizeof dir)) return 2;
  uint32_t need = (uint32_t)strlen(dir) + 1, *sz = P(5);
  if (P(3)) ST(uint32_t, A(3), 1);                       /* REG_SZ */
  if (!P(4)) { if (sz) *sz = need; return 0; }
  if (!sz || *sz < need) { if (sz) *sz = need; return 234; }  /* ERROR_MORE_DATA */
  memcpy(P(4), dir, need); *sz = need; return 0;
}
S(RegCloseKey) { return 0; }

/* ---- ole32 / oleaut32 / shell32 ---- */
S(CoInitialize) { return 0; }
S(CoUninitialize) { return 0; }
S(CoCreateInstance) { if (P(4)) ST(uint32_t, A(4), 0); return 0x80040154u; }   /* REGDB_E_CLASSNOTREG: no COM classes are registered */
S(SysAllocString) {
  const uint16_t *s = P(0); if (!s) return 0; uint32_t n = 0; while (s[n]) n++;
  uint8_t *b = aot_gmalloc(4 + 2 * n + 2); if (!b) return 0; ST(uint32_t, b, 2 * n); memcpy(b + 4, s, 2 * n + 2); return (uint32_t)(uintptr_t)(b + 4);
}
S(SHCreateDirectoryExA) {
  char p[512]; snprintf(p, sizeof p, "%s", (const char *)P(1)); for (char *q = p; *q; q++) if (*q == '\\') *q = '/';
  int existed = 0; for (char *q = p + 1; ; q++) { char ch = *q; if (ch == '/' || !ch) { *q = 0; if (mkdir(p, 0777) && errno == EEXIST) existed = 1; else existed = 0; *q = ch; } if (!ch) break; }
  return existed ? 183 : 0;
}
S(SHGetSpecialFolderPathA) { char *b = P(1); if (!b || !getcwd(b, 260)) return 0; return 1; }   /* every special folder maps to the game data directory */

/* ---- iphlpapi / wininet / winmm ---- */
S(GetAdaptersInfo) { return 232; }                                    /* ERROR_NO_DATA: no network adapters are reported */
S(InternetGetCookieW) { aot_last_error = 259; return 0; }
S(InternetSetCookieW) { aot_last_error = 5; return 0; }
S(waveOutGetDevCapsA) { return 6; }                                   /* MMSYSERR_NODRIVER */

/* ---- DirectInput / DirectSound / EAX / XInput / D3DX10 / Bink: explicit "unavailable" results ---- */
/* DirectSound (PE32): a complete software device. Buffers hold real memory (Lock/Unlock work), play cursors advance with wall-clock time at the buffer's
 * frequency, looping/stop semantics follow the API, so a game's streaming code runs normally. The mixed audio is NOT sent anywhere yet (no Android audio
 * backend): that is announced once on stderr. 3D buffers / IKsPropertySet (EAX) are not offered: QueryInterface answers E_NOINTERFACE. */
void aot_thunk_set_pop(uint64_t addr, int pop);
typedef struct { uint32_t vt; int ref; } DSObj;
typedef struct { uint32_t vt; int ref; uint8_t *data; uint32_t size, flags, pos, freq, align; int playing, looping; int32_t vol, pan; uint64_t t0; uint8_t wfx[18]; } DSBuf;
static uint32_t ds_vt_dev, ds_vt_buf, ds_vt_lis;
static const uint8_t IID_DSL_[16] = {0x84,0xFA,0x9A,0x27,0x81,0x49,0xCE,0x11,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60};
static const uint8_t IID_IUnknown_[16] = {0,0,0,0,0,0,0,0,0xC0,0,0,0,0,0,0,0x46}, IID_DS_[16] = {0x83,0xFA,0x9A,0x27,0x81,0x49,0xCE,0x11,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60},
  IID_DS8_[16] = {0x93,0x7E,0x0A,0xC5,0x95,0xF3,0x34,0x48,0x9E,0xF6,0x7F,0xA9,0x9D,0xE8,0x25,0x26}, IID_DSB_[16] = {0x85,0xFA,0x9A,0x27,0x81,0x49,0xCE,0x11,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60},
  IID_DSB8_[16] = {0x49,0xA4,0x25,0x68,0x24,0x75,0x82,0x4D,0x92,0x0F,0x50,0xE3,0x6A,0xB3,0xAB,0x1E};
#define DS_E_NOIFACE 0x80004002u
#define DS_E_PARAM 0x80070057u
#define DS_E_UNSUPPORTED 0x88780038u
static uint64_t ds_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint32_t ds_cursor(DSBuf *b) {
  if (!b->playing) return b->pos;
  uint64_t raw = b->pos + (ds_now() - b->t0) * ((uint64_t)b->freq * b->align) / 1000000000ull;
  if (b->looping) return (uint32_t)(raw % b->size);
  if (raw >= b->size) { b->playing = 0; b->pos = 0; return 0; }
  return (uint32_t)raw;
}
static const uint8_t IID_DS3DB_[16] = { 0x86, 0xfa, 0x9a, 0x27, 0x81, 0x49, 0xce, 0x11, 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60 };   /* IDirectSound3DBuffer */
static uint32_t ds_vt_3db;
static uint32_t ds_qi(uint32_t self, const uint8_t *iid, uint32_t *out, int dev) {
  if (!out) return DS_E_PARAM;
  int ok = !memcmp(iid, IID_IUnknown_, 16) || (dev == 3 ? !memcmp(iid, IID_DS3DB_, 16) : dev == 2 ? !memcmp(iid, IID_DSL_, 16) : dev ? (!memcmp(iid, IID_DS_, 16) || !memcmp(iid, IID_DS8_, 16)) : (!memcmp(iid, IID_DSB_, 16) || !memcmp(iid, IID_DSB8_, 16)));
  if (!ok) { fprintf(stderr, "[aot] DirectSound QueryInterface: IID {%08x-%04x-%04x-...} not supported\n", *(uint32_t *)iid, *(uint16_t *)(iid + 4), *(uint16_t *)(iid + 6)); *out = 0; return DS_E_NOIFACE; }
  *out = self; __atomic_add_fetch(&((DSObj *)(uintptr_t)self)->ref, 1, __ATOMIC_RELAXED); return 0;
}
#define DSD(n) static uint64_t dsd_##n(CPU *c)
#define DSB(n) static uint64_t dsb_##n(CPU *c)
#define THIS_B ((DSBuf *)(uintptr_t)(uint32_t)A(0))
DSD(QueryInterface) { return ds_qi((uint32_t)A(0), P(1), P(2), 1); }
DSD(AddRef) { return (uint32_t)__atomic_add_fetch(&((DSObj *)P(0))->ref, 1, __ATOMIC_RELAXED); }
DSD(Release) { DSObj *o = P(0); int r = __atomic_sub_fetch(&o->ref, 1, __ATOMIC_RELAXED); if (!r) aot_gfree(o); return (uint32_t)r; }
static DSBuf *ds_newbuf(uint32_t flags, uint32_t size, const uint8_t *wfx) {
  DSBuf *b = aot_gcalloc(1, sizeof *b); if (!b) return NULL; b->vt = ds_vt_buf; b->ref = 1; b->flags = flags; b->size = size ? size : 0x10000; b->data = aot_gcalloc(1, b->size);
  if (!b->data) { aot_gfree(b); return NULL; }
  static const uint8_t def[18] = { 1,0, 2,0, 0x44,0xAC,0,0, 0x10,0xB1,2,0, 4,0, 16,0, 0,0 };   /* PCM 44.1 kHz 16-bit stereo */
  memcpy(b->wfx, wfx ? wfx : def, 18); b->freq = *(uint32_t *)(b->wfx + 4); b->align = *(uint16_t *)(b->wfx + 12); if (!b->freq) b->freq = 44100; if (!b->align) b->align = 4; return b;
}
DSD(CreateSoundBuffer) {
  const uint8_t *d = P(1); uint32_t *out = P(2); if (!d || !out || *(uint32_t *)d < 20) return DS_E_PARAM;
  uint32_t flags = *(uint32_t *)(d + 4), bytes = *(uint32_t *)(d + 8); const uint8_t *wfx = (const uint8_t *)(uintptr_t)*(uint32_t *)(d + 16);
  if (!(flags & 1) && (!wfx || bytes < 4)) return DS_E_PARAM;   /* secondary buffers need a size and a format */
  DSBuf *b = ds_newbuf(flags, (flags & 1) ? 0 : bytes, (flags & 1) ? NULL : wfx); if (!b) return 0x8007000E; *out = (uint32_t)(uintptr_t)b; return 0;
}
DSD(GetCaps) { uint8_t *p = P(1); if (!p || *(uint32_t *)p < 96) return DS_E_PARAM; uint32_t sz = *(uint32_t *)p; memset(p, 0, sz); *(uint32_t *)p = sz;
  *(uint32_t *)(p + 4) = 0x20 | 0x10 | 0x8 | 0x800 | 0x2 | 0x200;   /* emulated driver, continuous rate, 16-bit stereo */
  *(uint32_t *)(p + 8) = 100; *(uint32_t *)(p + 12) = 200000; *(uint32_t *)(p + 20) = 1; return 0; }
DSD(DuplicateSoundBuffer) { DSBuf *s = (DSBuf *)P(1); uint32_t *out = P(2); if (!s || !out) return DS_E_PARAM; DSBuf *b = ds_newbuf(s->flags, s->size, s->wfx); if (!b) return 0x8007000E; memcpy(b->data, s->data, s->size); *out = (uint32_t)(uintptr_t)b; return 0; }
DSD(Ok) { return 0; }
DSD(GetSpeakerConfig) { uint32_t *o = P(1); if (!o) return DS_E_PARAM; *o = 3; return 0; }   /* DSSPEAKER_STEREO */
DSD(Unsupported) { return DS_E_UNSUPPORTED; }
DSD(DbQI) { return ds_qi((uint32_t)A(0), P(1), P(2), 3); }
DSD(LisQI) { return ds_qi((uint32_t)A(0), P(1), P(2), 2); }
DSB(QueryInterface) {
  if (P(1) && P(2) && !memcmp(P(1), IID_DS3DB_, 16)) {   /* 3D buffer: positioning parameters are accepted but spatialization is not applied */
    static int n3; note_once(&n3, "DirectSound: 3D buffer accepts settings but 3D positioning is not applied");
    DSObj *l = aot_gcalloc(1, sizeof *l); if (!l) return 0x8007000E; l->vt = ds_vt_3db; l->ref = 1; *(uint32_t *)P(2) = (uint32_t)(uintptr_t)l; return 0; }
  if (P(1) && P(2) && !memcmp(P(1), IID_DSL_, 16)) {   /* 3D listener: parameters are accepted but spatialization is not applied */
    static int n; note_once(&n, "DirectSound: 3D listener accepts settings but 3D positioning is not applied");
    DSObj *l = aot_gcalloc(1, sizeof *l); if (!l) return 0x8007000E; l->vt = ds_vt_lis; l->ref = 1; *(uint32_t *)P(2) = (uint32_t)(uintptr_t)l; return 0; }
  return ds_qi((uint32_t)A(0), P(1), P(2), 0);
}
DSB(Release) { DSBuf *b = THIS_B; int r = __atomic_sub_fetch(&b->ref, 1, __ATOMIC_RELAXED); if (!r) { aot_gfree(b->data); aot_gfree(b); } return (uint32_t)r; }
DSB(GetCaps) { uint32_t *p = P(1); if (!p || p[0] < 20) return DS_E_PARAM; DSBuf *b = THIS_B; p[1] = b->flags; p[2] = b->size; p[3] = 0; p[4] = 0; return 0; }
DSB(GetCurrentPosition) { DSBuf *b = THIS_B; uint32_t pl = ds_cursor(b), *pp = P(1), *pw = P(2); if (pp) *pp = pl; if (pw) *pw = (pl + b->freq * b->align / 50) % b->size / b->align * b->align; return 0; }   /* write cursor ~20 ms ahead */
DSB(GetFormat) { DSBuf *b = THIS_B; uint8_t *f = P(1); uint32_t n = (uint32_t)A(2), *w = P(3); if (f) memcpy(f, b->wfx, n < 18 ? n : 18); if (w) *w = 18; return 0; }
DSB(GetVolume) { uint32_t *o = P(1); if (!o) return DS_E_PARAM; *o = (uint32_t)THIS_B->vol; return 0; }
DSB(GetPan) { uint32_t *o = P(1); if (!o) return DS_E_PARAM; *o = (uint32_t)THIS_B->pan; return 0; }
DSB(GetFrequency) { uint32_t *o = P(1); if (!o) return DS_E_PARAM; *o = THIS_B->freq; return 0; }
DSB(GetStatus) { DSBuf *b = THIS_B; uint32_t *o = P(1); if (!o) return DS_E_PARAM; ds_cursor(b); *o = b->playing ? (1u | (b->looping ? 4u : 0u)) : 0; return 0; }
DSB(Lock) {
  DSBuf *b = THIS_B; uint32_t off = (uint32_t)A(1), n = (uint32_t)A(2), flags = (uint32_t)A(7); uint32_t *p1 = P(3), *b1 = P(4), *p2 = P(5), *b2 = P(6);
  if (flags & 1) { uint32_t pl = ds_cursor(b); off = (pl + b->freq * b->align / 50) % b->size / b->align * b->align; }
  if (flags & 2) { off = 0; n = b->size; }
  if (!p1 || !b1 || off >= b->size || n > b->size) return DS_E_PARAM;
  uint32_t first = n < b->size - off ? n : b->size - off; *p1 = (uint32_t)(uintptr_t)(b->data + off); *b1 = first;
  if (p2) *p2 = n > first ? (uint32_t)(uintptr_t)b->data : 0; if (b2) *b2 = n - first; return 0;
}
DSB(Play) {
  DSBuf *b = THIS_B; static int note; note_once(&note, "DirectSound: audio is mixed/timed in software but NOT output (no Android audio backend yet)");
  ds_cursor(b); b->looping = (A(3) & 1) != 0; if (!b->playing) { b->playing = 1; b->t0 = ds_now(); } return 0;
}
DSB(SetCurrentPosition) { DSBuf *b = THIS_B; uint32_t p = (uint32_t)A(1); if (p >= b->size) return DS_E_PARAM; b->pos = p; b->t0 = ds_now(); return 0; }
DSB(SetFormat) { DSBuf *b = THIS_B; const uint8_t *f = P(1); if (!f) return DS_E_PARAM; memcpy(b->wfx, f, 16); b->freq = *(uint32_t *)(b->wfx + 4); b->align = *(uint16_t *)(b->wfx + 12); if (!b->freq) b->freq = 44100; if (!b->align) b->align = 4; return 0; }
DSB(SetVolume) { THIS_B->vol = (int32_t)A(1); return 0; }
DSB(SetPan) { THIS_B->pan = (int32_t)A(1); return 0; }
DSB(SetFrequency) { DSBuf *b = THIS_B; uint32_t f = (uint32_t)A(1); b->pos = ds_cursor(b); b->t0 = ds_now(); b->freq = f ? f : *(uint32_t *)(b->wfx + 4); return 0; }
DSB(Stop) { DSBuf *b = THIS_B; b->pos = ds_cursor(b); b->playing = 0; return 0; }
DSB(Ok) { return 0; }
DSB(Unsupported) { return DS_E_UNSUPPORTED; }
typedef struct { int idx; const char *name; uint64_t (*fn)(CPU *); int argc; } DSSlot;
static uint32_t ds_build(const char *iface, int n, const DSSlot *s) {
  uint32_t *vt = aot_gcalloc(n + 64, 4); char nm[96]; if (!vt) return 0;
  for (int i = 0; i < n + 64; i++) { snprintf(nm, sizeof nm, "%s::vtbl[%d] (not implemented)", iface, i); vt[i] = (uint32_t)aot_register_thunk(NULL, nm); }
  for (; s->fn; s++) { snprintf(nm, sizeof nm, "%s::%s", iface, s->name); uint64_t t = aot_register_thunk((aot_host_fn)s->fn, nm); aot_thunk_set_pop(t, 4 * s->argc); vt[s->idx] = (uint32_t)t; }
  return (uint32_t)(uintptr_t)vt;
}
static void ds_init_tables(void) {
  static const DSSlot dev[] = { {0,"QueryInterface",dsd_QueryInterface,3}, {1,"AddRef",dsd_AddRef,1}, {2,"Release",dsd_Release,1}, {3,"CreateSoundBuffer",dsd_CreateSoundBuffer,4}, {4,"GetCaps",dsd_GetCaps,2},
    {5,"DuplicateSoundBuffer",dsd_DuplicateSoundBuffer,3}, {6,"SetCooperativeLevel",dsd_Ok,3}, {7,"Compact",dsd_Ok,1}, {8,"GetSpeakerConfig",dsd_GetSpeakerConfig,2}, {9,"SetSpeakerConfig",dsd_Ok,2},
    {10,"Initialize",dsd_Ok,2}, {11,"VerifyCertification",dsd_Unsupported,2}, {0,0,0,0} };
  static const DSSlot buf[] = { {0,"QueryInterface",dsb_QueryInterface,3}, {1,"AddRef",dsd_AddRef,1}, {2,"Release",dsb_Release,1}, {3,"GetCaps",dsb_GetCaps,2}, {4,"GetCurrentPosition",dsb_GetCurrentPosition,3},
    {5,"GetFormat",dsb_GetFormat,4}, {6,"GetVolume",dsb_GetVolume,2}, {7,"GetPan",dsb_GetPan,2}, {8,"GetFrequency",dsb_GetFrequency,2}, {9,"GetStatus",dsb_GetStatus,2}, {10,"Initialize",dsb_Ok,3},
    {11,"Lock",dsb_Lock,8}, {12,"Play",dsb_Play,4}, {13,"SetCurrentPosition",dsb_SetCurrentPosition,2}, {14,"SetFormat",dsb_SetFormat,2}, {15,"SetVolume",dsb_SetVolume,2}, {16,"SetPan",dsb_SetPan,2},
    {17,"SetFrequency",dsb_SetFrequency,2}, {18,"Stop",dsb_Stop,1}, {19,"Unlock",dsb_Ok,5}, {20,"Restore",dsb_Ok,1}, {21,"SetFX",dsb_Unsupported,4}, {22,"AcquireResources",dsb_Unsupported,4}, {23,"GetObjectInPath",dsb_Unsupported,5}, {0,0,0,0} };
  static const DSSlot lis[] = { {0,"QueryInterface",dsd_LisQI,3}, {1,"AddRef",dsd_AddRef,1}, {2,"Release",dsd_Release,1}, {3,"GetAllParameters",dsd_Unsupported,2}, {4,"GetDistanceFactor",dsd_Unsupported,2},
    {5,"GetDopplerFactor",dsd_Unsupported,2}, {6,"GetOrientation",dsd_Unsupported,3}, {7,"GetPosition",dsd_Unsupported,2}, {8,"GetRolloffFactor",dsd_Unsupported,2}, {9,"GetVelocity",dsd_Unsupported,2},
    {10,"SetAllParameters",dsd_Ok,3}, {11,"SetDistanceFactor",dsd_Ok,3}, {12,"SetDopplerFactor",dsd_Ok,3}, {13,"SetOrientation",dsd_Ok,8}, {14,"SetPosition",dsd_Ok,5}, {15,"SetRolloffFactor",dsd_Ok,3},
    {16,"SetVelocity",dsd_Ok,5}, {17,"CommitDeferredSettings",dsd_Ok,1}, {0,0,0,0} };
  static const DSSlot db3[] = { {0,"QueryInterface",dsd_DbQI,3}, {1,"AddRef",dsd_AddRef,1}, {2,"Release",dsd_Release,1}, {3,"GetAllParameters",dsd_Unsupported,2}, {4,"GetConeAngles",dsd_Unsupported,3},
    {5,"GetConeOrientation",dsd_Unsupported,2}, {6,"GetConeOutsideVolume",dsd_Unsupported,2}, {7,"GetMaxDistance",dsd_Unsupported,2}, {8,"GetMinDistance",dsd_Unsupported,2}, {9,"GetMode",dsd_Unsupported,2},
    {10,"GetPosition",dsd_Unsupported,2}, {11,"GetVelocity",dsd_Unsupported,2}, {12,"SetAllParameters",dsd_Ok,3}, {13,"SetConeAngles",dsd_Ok,4}, {14,"SetConeOrientation",dsd_Ok,5}, {15,"SetConeOutsideVolume",dsd_Ok,3},
    {16,"SetMaxDistance",dsd_Ok,3}, {17,"SetMinDistance",dsd_Ok,3}, {18,"SetMode",dsd_Ok,3}, {19,"SetPosition",dsd_Ok,5}, {20,"SetVelocity",dsd_Ok,5}, {0,0,0,0} };
  if (!ds_vt_dev) { ds_vt_3db = ds_build("IDirectSound3DBuffer", 21, db3); ds_vt_lis = ds_build("IDirectSound3DListener", 18, lis); ds_vt_dev = ds_build("IDirectSound8", 12, dev); ds_vt_buf = ds_build("IDirectSoundBuffer8", 24, buf); }
}
S(dsound_enum) {   /* DirectSoundEnumerateA(callback, ctx): one default device, as on a normal system */
  static char *desc, *mod; if (!desc) { desc = aot_gmalloc(64); mod = aot_gmalloc(16); strcpy(desc, "Primary Sound Driver"); strcpy(mod, ""); }
  if (A(0)) aot_call(c, A(0), 4, (uint64_t)0, (uint64_t)(uintptr_t)desc, (uint64_t)(uintptr_t)mod, A(1)); return 0;
}
S(dsound_create) {   /* DirectSoundCreate8 / EAXDirectSoundCreate (guid, IDirectSound**, outer) */
  uint32_t *out = P(1); if (!out) return DS_E_PARAM; ds_init_tables(); DSObj *d = aot_gcalloc(1, sizeof *d); if (!d || !ds_vt_dev) return 0x8007000E;
  d->vt = ds_vt_dev; d->ref = 1; *out = (uint32_t)(uintptr_t)d; return 0;
}

/* ---- DirectInput8: objects open successfully (the game requires keyboard+mouse devices to finish creating its render device) but
 * report NO input: GetDeviceState returns zeroes, GetDeviceData returns 0 events. Android touch/key input is not wired to them yet. ---- */
static uint32_t di_vt, di_vt_dev;
#define DIO(n) S(di_##n)
DIO(QI) { uint32_t *o = P(2); if (!o) return 0x80004003u; *o = (uint32_t)A(0); ((DSObj *)P(0))->ref++; return 0; }
DIO(AddRef) { return (uint32_t)++((DSObj *)P(0))->ref; }
DIO(Release) { DSObj *o = P(0); return (uint32_t)(o->ref > 0 ? --o->ref : 0); }
DIO(Ok) { return 0; }
DIO(State) { static int n; note_once(&n, "dinput8: devices created; they report no input (touch/keys not mapped to DirectInput yet)"); if (P(2)) memset(P(2), 0, (uint32_t)A(1)); return 0; }
DIO(Data) { if (P(3)) *(uint32_t *)P(3) = 0; return 0; }
DIO(CreateDevice) {
  uint32_t *out = P(2); if (!out) return 0x80070057u; DSObj *d = aot_gcalloc(1, sizeof *d); if (!d) return 0x8007000Eu;
  d->vt = di_vt_dev; d->ref = 1; *out = (uint32_t)(uintptr_t)d; return 0; }
static void di_init(void) {
  if (di_vt) return;
  static const DSSlot dev[] = { {0,"QueryInterface",u_di_QI,3}, {1,"AddRef",u_di_AddRef,1}, {2,"Release",u_di_Release,1}, {3,"GetCapabilities",u_di_Ok,2}, {4,"EnumObjects",u_di_Ok,4},
    {5,"GetProperty",u_di_Ok,3}, {6,"SetProperty",u_di_Ok,3}, {7,"Acquire",u_di_Ok,1}, {8,"Unacquire",u_di_Ok,1}, {9,"GetDeviceState",u_di_State,3}, {10,"GetDeviceData",u_di_Data,5},
    {11,"SetDataFormat",u_di_Ok,2}, {12,"SetEventNotification",u_di_Ok,2}, {13,"SetCooperativeLevel",u_di_Ok,3}, {14,"GetObjectInfo",u_di_Ok,4}, {15,"GetDeviceInfo",u_di_Ok,2},
    {16,"RunControlPanel",u_di_Ok,3}, {17,"Initialize",u_di_Ok,4}, {25,"Poll",u_di_Ok,1}, {26,"SendDeviceData",u_di_Ok,5}, {0,0,0,0} };
  static const DSSlot idi[] = { {0,"QueryInterface",u_di_QI,3}, {1,"AddRef",u_di_AddRef,1}, {2,"Release",u_di_Release,1}, {3,"CreateDevice",u_di_CreateDevice,4},
    {4,"EnumDevices",u_di_Ok,5}, {5,"GetDeviceStatus",u_di_Ok,2}, {6,"RunControlPanel",u_di_Ok,3}, {7,"Initialize",u_di_Ok,3}, {0,0,0,0} };
  di_vt_dev = ds_build("IDirectInputDevice8", 32, dev); di_vt = ds_build("IDirectInput8", 11, idi);
}
S(DirectInput8Create) {   /* (hinst, version, riid, void** ppv, outer) */
  uint32_t *out = P(3); if (!out) return 0x80070057u; di_init(); DSObj *d = aot_gcalloc(1, sizeof *d); if (!d || !di_vt) return 0x8007000Eu;
  d->vt = di_vt; d->ref = 1; *out = (uint32_t)(uintptr_t)d; return 0; }
S(xinput_none) { return 1167; }                                       /* ERROR_DEVICE_NOT_CONNECTED */
S(D3DX10UnsetAllDeviceObjects) { return 0; }                          /* teardown helper: nothing is cached on our side */
S(bink_unavail) { static int n; note_once(&n, "binkw32: Bink video is not available; "); return 0; }
/* Bink videos are NOT decoded. The game's video player does not cope with BinkOpen failing (it still uploads a frame into textures
 * that only exist after a successful open), so BinkOpen returns a stub 64x64 clip that is already at its last frame: the player
 * sees "finished", no pixels are produced and intro/cut-scene movies are effectively skipped. */
S(bink_open) { static int n; note_once(&n, "binkw32: Bink video is NOT decoded; movies open as an empty finished 64x64 clip (skipped)");
  uint32_t *h = aot_gcalloc(1, 256); if (!h) return 0; h[0] = 64; h[1] = 64; h[2] = 1; h[3] = 1; h[4] = 1; h[5] = 30; h[6] = 1; return (uint32_t)(uintptr_t)h; }
S(bink_fbinfo) { uint32_t *f = P(1); if (!f) return 0; memset(f, 0, 0x60); f[0] = 1; f[1] = 64; f[2] = 64; f[3] = 32; f[4] = 32; return 0; }   /* BINKFRAMEBUFFERS: 1 frame, YA 64x64, cRcB 32x32, no plane memory */
S(bink_next) { uint32_t *h = P(0); if (h && h[3] < h[2]) h[3]++; return 0; }

/* ---- ws2_32 (BSD sockets underneath; Winsock constants/layouts translated) ---- */
static __thread int wsa_err;
static int wsa_map(int e) {
  switch (e) { case EWOULDBLOCK: case EINPROGRESS: return 10035; case EINTR: return 10004; case EBADF: return 10009; case EACCES: return 10013; case EFAULT: return 10014; case EINVAL: return 10022;
    case EMFILE: return 10024; case EADDRINUSE: return 10048; case EADDRNOTAVAIL: return 10049; case ENETUNREACH: return 10051; case ECONNABORTED: return 10053; case ECONNRESET: return 10054;
    case ENOTCONN: return 10057; case ETIMEDOUT: return 10060; case ECONNREFUSED: return 10061; case ENOTSOCK: return 10038; case EPROTONOSUPPORT: return 10043; case EAFNOSUPPORT: return 10047; case EISCONN: return 10056; case EMSGSIZE: return 10040; default: return 10000 + e; }
}
static uint64_t sret(long r) { if (r < 0) { wsa_err = wsa_map(errno); return 0xFFFFFFFFu; } return (uint32_t)r; }
S(WSAStartup) { uint8_t *d = P(1); if (d) { memset(d, 0, 400); ST(uint16_t, d, 0x0202); ST(uint16_t, d + 2, 0x0202); memcpy(d + 4, "AOT Winsock", 12); ST(uint16_t, d + 390, 32767); } return 0; }
S(WSACleanup) { return 0; }
S(WSAGetLastError) { return (uint32_t)wsa_err; }
S(WSAAsyncSelect) { static int n; note_once(&n, "WSAAsyncSelect: window-message socket notification is not supported; returning SOCKET_ERROR"); wsa_err = 10045; return 0xFFFFFFFFu; }
S(socket) { return sret(socket((int)A(0), (int)A(1), (int)A(2))); }
S(closesocket) { return sret(close((int)A(0))); }
S(bind) { return sret(bind((int)A(0), P(1), (socklen_t)A(2))); }
S(connect) { return sret(connect((int)A(0), P(1), (socklen_t)A(2))); }
S(listen) { return sret(listen((int)A(0), (int)A(1))); }
S(accept) { socklen_t l = 0, *pl = NULL; if (P(2)) { l = LD(int32_t, A(2)); pl = &l; } long r = accept((int)A(0), P(1), pl); if (pl && r >= 0) ST(int32_t, A(2), (int32_t)l); return sret(r); }
S(send) { return sret(send((int)A(0), P(1), (size_t)A(2), (int)A(3))); }
S(recv) { return sret(recv((int)A(0), P(1), (size_t)A(2), (int)A(3))); }
S(sendto) { return sret(sendto((int)A(0), P(1), (size_t)A(2), (int)A(3), P(4), (socklen_t)A(5))); }
S(recvfrom) { socklen_t l = 0, *pl = NULL; if (P(5)) { l = LD(int32_t, A(5)); pl = &l; } long r = recvfrom((int)A(0), P(1), (size_t)A(2), (int)A(3), P(4), pl); if (pl && r >= 0) ST(int32_t, A(5), (int32_t)l); return sret(r); }
S(getpeername) { socklen_t l = LD(int32_t, A(2)); long r = getpeername((int)A(0), P(1), &l); if (r >= 0) ST(int32_t, A(2), (int32_t)l); return sret(r); }
S(getsockname) { socklen_t l = LD(int32_t, A(2)); long r = getsockname((int)A(0), P(1), &l); if (r >= 0) ST(int32_t, A(2), (int32_t)l); return sret(r); }
static int sockopt(int level, int opt, int *L, int *O) {   /* Winsock -> Linux level/option */
  if (level == 0xffff) { *L = SOL_SOCKET; switch (opt) { case 4: *O = SO_REUSEADDR; return 0; case 8: *O = SO_KEEPALIVE; return 0; case 0x20: *O = SO_BROADCAST; return 0; case 0x80: *O = SO_LINGER; return 0;
    case 0x1001: *O = SO_SNDBUF; return 0; case 0x1002: *O = SO_RCVBUF; return 0; case 0x1007: *O = SO_ERROR; return 0; case 0x1008: *O = SO_TYPE; return 0; default: return -1; } }
  if (level == 6 && opt == 1) { *L = IPPROTO_TCP; *O = TCP_NODELAY; return 0; }
  return -1;
}
S(setsockopt) { int L, O; if (sockopt((int)A(1), (int)A(2), &L, &O)) { wsa_err = 10042; return 0xFFFFFFFFu; } return sret(setsockopt((int)A(0), L, O, P(3), (socklen_t)A(4))); }
S(getsockopt) { int L, O; if (sockopt((int)A(1), (int)A(2), &L, &O)) { wsa_err = 10042; return 0xFFFFFFFFu; } socklen_t l = LD(int32_t, A(4)); long r = getsockopt((int)A(0), L, O, P(3), &l); if (r >= 0) ST(int32_t, A(4), (int32_t)l); return sret(r); }
S(ioctlsocket) {
  uint32_t cmd = (uint32_t)A(1); int fd = (int)A(0);
  if (cmd == 0x8004667Eu) { int fl = fcntl(fd, F_GETFL); if (fl < 0) return sret(-1); return sret(fcntl(fd, F_SETFL, LD(uint32_t, A(2)) ? fl | O_NONBLOCK : fl & ~O_NONBLOCK)); }   /* FIONBIO */
  if (cmd == 0x4004667Fu) { int n = 0; long r = ioctl(fd, FIONREAD, &n); if (r >= 0) ST(uint32_t, A(2), (uint32_t)n); return sret(r); }   /* FIONREAD */
  wsa_err = 10022; return 0xFFFFFFFFu;
}
/* Winsock fd_set: u_int count + SOCKET[64]; timeval = two int32 */
static void ws_in(const uint8_t *w, fd_set *o, int *mx) { FD_ZERO(o); if (!w) return; uint32_t n = LD(uint32_t, (uintptr_t)w); for (uint32_t i = 0; i < n && i < 64; i++) { int fd = (int)LD(uint32_t, (uintptr_t)w + 4 + 4 * i); if (fd >= 0 && fd < FD_SETSIZE) { FD_SET(fd, o); if (fd > *mx) *mx = fd; } } }
static void ws_out(uint8_t *w, const fd_set *s) { if (!w) return; uint32_t n = LD(uint32_t, (uintptr_t)w), k = 0; for (uint32_t i = 0; i < n && i < 64; i++) { uint32_t fd = LD(uint32_t, (uintptr_t)w + 4 + 4 * i); if (fd < FD_SETSIZE && FD_ISSET((int)fd, s)) ST(uint32_t, (uintptr_t)w + 4 + 4 * k++, fd); } ST(uint32_t, (uintptr_t)w, k); }
S(select) {
  fd_set r, w, e; int mx = -1; ws_in(P(1), &r, &mx); ws_in(P(2), &w, &mx); ws_in(P(3), &e, &mx);
  struct timeval tv, *ptv = NULL; if (P(4)) { tv.tv_sec = LD(int32_t, A(4)); tv.tv_usec = LD(int32_t, A(4) + 4); ptv = &tv; }
  long n = select(mx + 1, P(1) ? &r : NULL, P(2) ? &w : NULL, P(3) ? &e : NULL, ptv);
  if (n >= 0) { ws_out(P(1), &r); ws_out(P(2), &w); ws_out(P(3), &e); } return sret(n);
}
S(__WSAFDIsSet) { uint32_t fd = (uint32_t)A(0), n = LD(uint32_t, A(1)); for (uint32_t i = 0; i < n && i < 64; i++) if (LD(uint32_t, A(1) + 4 + 4 * i) == fd) return 1; return 0; }
S(htonl) { return __builtin_bswap32((uint32_t)A(0)); }
S(ntohl) { return __builtin_bswap32((uint32_t)A(0)); }
S(htons) { return __builtin_bswap16((uint16_t)A(0)); }
S(ntohs) { return __builtin_bswap16((uint16_t)A(0)); }
S(inet_addr) { struct in_addr a; if (!inet_aton((const char *)P(0), &a)) return 0xFFFFFFFFu; return a.s_addr; }
S(inet_ntoa) { static __thread char *buf; if (!buf) buf = aot_gmalloc(32); struct in_addr a; a.s_addr = (uint32_t)A(0); snprintf(buf, 32, "%s", inet_ntoa(a)); return (uint32_t)(uintptr_t)buf; }
S(gethostname) { char *b = P(0); int n = (int)A(1); if (gethostname(b, (size_t)n)) return sret(-1); b[n - 1] = 0; return 0; }
static uint64_t hostent32(struct hostent *h) {   /* 32-bit HOSTENT in low memory: name, aliases, addrtype(2), length(2), addr_list */
  static __thread uint8_t *blk; if (!blk) blk = aot_gmalloc(512); if (!h || h->h_addrtype != AF_INET) { wsa_err = 11001; return 0; }
  memset(blk, 0, 512); uint8_t *p = blk + 16; ST(uint32_t, blk + 4, 0); snprintf((char *)p, 128, "%s", h->h_name); ST(uint32_t, blk, (uint32_t)(uintptr_t)p); p += 128;
  uint8_t *lst = p; p += 4 * 9; int n = 0;
  for (; h->h_addr_list[n] && n < 8; n++) { memcpy(p, h->h_addr_list[n], 4); ST(uint32_t, lst + 4 * n, (uint32_t)(uintptr_t)p); p += 4; }
  ST(uint16_t, blk + 8, AF_INET); ST(uint16_t, blk + 10, 4); ST(uint32_t, blk + 12, (uint32_t)(uintptr_t)lst); return (uint32_t)(uintptr_t)blk;
}
S(gethostbyname) { return hostent32(gethostbyname((const char *)P(0))); }
S(gethostbyaddr) { return hostent32(gethostbyaddr(P(0), (socklen_t)A(1), (int)A(2))); }

#define U(dll, n, b) { dll, #n, b, u_##n }
#define UO(dll, n, b, f) { dll, n, b, u_##f }
#define R(dll, n, b) { dll, #n, b, NULL }
typedef struct { const char *dll, *name; int pop; shim_fn fn; } DllApi;
static const DllApi tab[] = {
  R("user32.dll", AdjustWindowRect, 12), R("user32.dll", ClipCursor, 4), R("user32.dll", CreateWindowExA, 48), R("user32.dll", DefWindowProcA, 16), R("user32.dll", DestroyWindow, 4),
  R("user32.dll", DispatchMessageA, 4), R("user32.dll", GetClientRect, 8), R("user32.dll", GetCursorPos, 4), R("user32.dll", GetDesktopWindow, 0), R("user32.dll", GetKeyboardState, 4),
  R("user32.dll", GetSystemMetrics, 4), R("user32.dll", GetWindowRect, 8), R("user32.dll", IsIconic, 4), R("user32.dll", IsWindow, 4), R("user32.dll", IsWindowVisible, 4),
  R("user32.dll", LoadCursorA, 8), R("user32.dll", MessageBoxA, 16), R("user32.dll", MoveWindow, 24), R("user32.dll", PeekMessageA, 20), R("user32.dll", PostQuitMessage, 4),
  R("user32.dll", RegisterClassExA, 4), R("user32.dll", ScreenToClient, 8), R("user32.dll", ShowCursor, 4), R("user32.dll", ShowWindow, 8), R("user32.dll", TranslateMessage, 4), R("user32.dll", UpdateWindow, 4),
  U("user32.dll", SetWindowsHookExA, 16), U("user32.dll", UnhookWindowsHookEx, 4), U("user32.dll", CallNextHookEx, 16), U("user32.dll", LoadStringA, 16), U("user32.dll", SystemParametersInfoA, 16), U("user32.dll", ToUnicode, 24),
  U("advapi32.dll", CryptAcquireContextA, 20), U("advapi32.dll", CryptGenRandom, 12), U("advapi32.dll", CryptReleaseContext, 8), U("advapi32.dll", GetUserNameA, 8),
  U("advapi32.dll", RegOpenKeyExA, 20), U("advapi32.dll", RegQueryValueExA, 24), U("advapi32.dll", RegCloseKey, 4),
  U("ole32.dll", CoInitialize, 4), U("ole32.dll", CoUninitialize, 0), U("ole32.dll", CoCreateInstance, 20), U("oleaut32.dll", SysAllocString, 4),
  U("shell32.dll", SHCreateDirectoryExA, 12), U("shell32.dll", SHGetSpecialFolderPathA, 16),
  U("iphlpapi.dll", GetAdaptersInfo, 8), U("wininet.dll", InternetGetCookieW, 16), U("wininet.dll", InternetSetCookieW, 12), U("winmm.dll", waveOutGetDevCapsA, 12),
  U("dinput8.dll", DirectInput8Create, 20), U("d3dx10_36.dll", D3DX10UnsetAllDeviceObjects, 4),
  UO("dsound.dll", "#2", 8, dsound_enum), UO("dsound.dll", "#11", 12, dsound_create), UO("eax.dll", "#6", 12, dsound_create),
  UO("xinput1_3.dll", "#2", 8, xinput_none), UO("xinput1_3.dll", "#3", 8, xinput_none), UO("xinput1_3.dll", "#4", 12, xinput_none),
  UO("binkw32.dll", "_BinkOpen", 8, bink_open), UO("binkw32.dll", "_BinkClose", 4, bink_unavail), UO("binkw32.dll", "_BinkCopyToBuffer", 28, bink_unavail), UO("binkw32.dll", "_BinkDoFrame", 4, bink_unavail),
  UO("binkw32.dll", "_BinkGetFrameBuffersInfo", 8, bink_fbinfo), UO("binkw32.dll", "_BinkNextFrame", 4, bink_next), UO("binkw32.dll", "_BinkOpenDirectSound", 4, bink_unavail), UO("binkw32.dll", "_BinkPause", 8, bink_unavail),
  UO("binkw32.dll", "_BinkSetMemory", 8, bink_unavail), UO("binkw32.dll", "_BinkSetSoundSystem", 8, bink_unavail), UO("binkw32.dll", "_BinkSetSoundTrack", 8, bink_unavail), UO("binkw32.dll", "_BinkSetVolume", 12, bink_unavail),
  UO("binkw32.dll", "_BinkShouldSkip", 4, bink_unavail), UO("binkw32.dll", "_BinkWait", 4, bink_unavail),
  U("ws2_32.dll", WSAAsyncSelect, 16), U("ws2_32.dll", WSACleanup, 0), U("ws2_32.dll", WSAGetLastError, 0), U("ws2_32.dll", WSAStartup, 8), U("ws2_32.dll", __WSAFDIsSet, 8),
  U("ws2_32.dll", accept, 12), U("ws2_32.dll", bind, 12), U("ws2_32.dll", closesocket, 4), U("ws2_32.dll", connect, 12), U("ws2_32.dll", gethostbyaddr, 12), U("ws2_32.dll", gethostbyname, 4),
  U("ws2_32.dll", gethostname, 8), U("ws2_32.dll", getpeername, 12), U("ws2_32.dll", getsockname, 12), U("ws2_32.dll", getsockopt, 20), U("ws2_32.dll", htonl, 4), U("ws2_32.dll", htons, 4),
  U("ws2_32.dll", inet_addr, 4), U("ws2_32.dll", inet_ntoa, 4), U("ws2_32.dll", ioctlsocket, 12), U("ws2_32.dll", listen, 8), U("ws2_32.dll", ntohl, 4), U("ws2_32.dll", ntohs, 4),
  U("ws2_32.dll", recv, 16), U("ws2_32.dll", recvfrom, 24), U("ws2_32.dll", select, 20), U("ws2_32.dll", send, 16), U("ws2_32.dll", sendto, 24), U("ws2_32.dll", setsockopt, 20), U("ws2_32.dll", socket, 12),
  /* ws2_32 by ordinal (GetProcAddress(MAKEINTRESOURCE(n))) */
  UO("ws2_32.dll", "#1", 12, accept),
  UO("ws2_32.dll", "#2", 12, bind),
  UO("ws2_32.dll", "#3", 4, closesocket),
  UO("ws2_32.dll", "#4", 12, connect),
  UO("ws2_32.dll", "#5", 12, getpeername),
  UO("ws2_32.dll", "#6", 12, getsockname),
  UO("ws2_32.dll", "#7", 20, getsockopt),
  UO("ws2_32.dll", "#8", 4, htonl),
  UO("ws2_32.dll", "#9", 4, htons),
  UO("ws2_32.dll", "#10", 12, ioctlsocket),
  UO("ws2_32.dll", "#11", 4, inet_addr),
  UO("ws2_32.dll", "#12", 4, inet_ntoa),
  UO("ws2_32.dll", "#13", 8, listen),
  UO("ws2_32.dll", "#14", 4, ntohl),
  UO("ws2_32.dll", "#15", 4, ntohs),
  UO("ws2_32.dll", "#16", 16, recv),
  UO("ws2_32.dll", "#17", 24, recvfrom),
  UO("ws2_32.dll", "#18", 20, select),
  UO("ws2_32.dll", "#19", 16, send),
  UO("ws2_32.dll", "#20", 24, sendto),
  UO("ws2_32.dll", "#21", 20, setsockopt),
  UO("ws2_32.dll", "#23", 12, socket),
  UO("ws2_32.dll", "#51", 12, gethostbyaddr),
  UO("ws2_32.dll", "#52", 4, gethostbyname),
  UO("ws2_32.dll", "#57", 8, gethostname),
  UO("ws2_32.dll", "#101", 16, WSAAsyncSelect),
  UO("ws2_32.dll", "#111", 0, WSAGetLastError),
  UO("ws2_32.dll", "#115", 8, WSAStartup),
  UO("ws2_32.dll", "#116", 0, WSACleanup),
  UO("ws2_32.dll", "#151", 8, __WSAFDIsSet),
  { NULL, NULL, 0, NULL }
};
shim_fn aot_x86_dll_resolve(const char *dll, const char *name, int *pop) {
  for (const DllApi *a = tab; a->name; a++)
    if (!strcasecmp(a->dll, dll) && !strcmp(a->name, name)) { *pop = a->pop; return a->fn ? a->fn : aot_resolve_import(dll, name); }
  return NULL;
}
