#include <time.h>
#include <unistd.h>
/* PE32 guest -> 64-bit DXVK D3D10/DXGI proxy. Guest COM objects are PX wrappers whose vtable thunks (generated from the DXVK headers,
   d3d10_proxy_gen.inc) convert the guest stdcall frame (32-bit values, 32-bit pointers, 32-bit-layout structs) into a call on the real
   host COM object loaded from libdxvk_*.so. Guest memory is identity-mapped below 4 GB, so POD pointers pass through unchanged. */
#include "shims.h"
#include "aot_gpu.h"   /* ?system? or a bundled Turnip driver, chosen in the menu that follows the folder picker */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef uint64_t (*fn_t)(CPU *);
#define ARGN(c, n) ((n) == 0 ? (c)->r[1] : (n) == 1 ? (c)->r[2] : (n) == 2 ? (c)->r[8] : (n) == 3 ? (c)->r[9] : LD(uint64_t, (c)->r[4] + 8 + 8 * (n)))
#define A(n) ((uint32_t)ARGN(c, (n)))
#define G32(a, v) ST(uint32_t, (uintptr_t)(a), (uint32_t)(uintptr_t)(v))
void aot_thunk_set_pop(uint64_t addr, int pop);
typedef struct { const char *name; uint8_t n; char k[17]; uint8_t ci[16], ty[16]; uint8_t ret; } PM;
typedef struct { const char *name; int nm; const PM *m; const fn_t *wf; const uint8_t *guid; } PI;
static uint64_t px_call(CPU *c, const PI *pi, int slot);
static void *g_hdev; static int g_flush_slot = -1;
volatile uint64_t aot_frames;   /* presented frames, for the FPS counter */
volatile uint64_t aot_ns[4];    /* wall time the game thread spent blocked: [0] inside Present, [1] waiting for an event/timestamp query */
extern void aot_present_tick(void);   /* android_main.c: frame pacing hint for the Android scheduler */
static inline uint64_t px_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
#include "d3d10_proxy_gen.inc"
#define NPI ((int)(sizeof PIS / sizeof PIS[0]))
/* ---- where the game thread's time goes: time inside translated code vs time in the D3D proxy ---- */
static uint64_t pf_proxy_ticks, pf_calls, pf_if_calls[64], pf_if_ticks[64];
static void pf_report(void) {
  static uint64_t last_ns, last_g, last_p, last_c, last_f; static int started;
  uint64_t now = px_now_ns();
  if (!started) { started = 1; last_ns = now; last_g = aot_perf_guest; last_p = pf_proxy_ticks; last_c = pf_calls; last_f = aot_frames; return; }
  if (now - last_ns < 1000000000ull) return;
  double to_ms = 1000.0 / (double)aot_perf_hz(), win = (double)(now - last_ns) / 1e6;
  uint64_t dg = aot_perf_guest - last_g, dp = pf_proxy_ticks - last_p, dc = pf_calls - last_c, df = aot_frames - last_f;
  double gms = (double)dg * to_ms, pms = (double)dp * to_ms, n = df ? (double)df : 1.0;
  fprintf(stderr, "[perf] %.0f ms, %llu frames (%.1f fps) | translated code %.2f ms/frame (%.0f%% of wall, D3D inside)"
                  " | D3D proxy %.2f ms/frame, %llu calls (%.0f ns/call)\n",
          win, (unsigned long long)df, 1000.0 * (double)df / win, gms / n, win > 0 ? gms / win * 100.0 : 0.0,
          pms / n, (unsigned long long)dc, dc ? pms * 1e6 / (double)dc : 0.0);
  int top[3] = { -1, -1, -1 };
  for (int i = 0; i < 64; i++) {
    uint64_t t = pf_if_ticks[i]; if (!t) continue;
    if (top[0] < 0 || t > pf_if_ticks[top[0]]) { top[2] = top[1]; top[1] = top[0]; top[0] = i; }
    else if (top[1] < 0 || t > pf_if_ticks[top[1]]) { top[2] = top[1]; top[1] = i; }
    else if (top[2] < 0 || t > pf_if_ticks[top[2]]) top[2] = i;
  }
  if (top[0] >= 0) {
    fprintf(stderr, "[perf]   hottest interfaces:");
    for (int k = 0; k < 3; k++) { int i = top[k]; if (i < 0) continue;
      fprintf(stderr, " %s %.2f ms %llu calls;", PIS[i].name, (double)pf_if_ticks[i] * to_ms / n, (unsigned long long)pf_if_calls[i]); }
    fprintf(stderr, "\n");
  }
  memset(pf_if_calls, 0, sizeof pf_if_calls); memset(pf_if_ticks, 0, sizeof pf_if_ticks);
  last_ns = now; last_g = aot_perf_guest; last_p = pf_proxy_ticks; last_c = pf_calls; last_f = aot_frames;
}
static uint64_t px_call_inner(CPU *c, const PI *pi, int slot);
/* overlay: cumulative nanoseconds in translated regions (k=2) and inside the D3D proxy (k=3) */
uint64_t aot_perf_ns(int k) {
  uint64_t t = k == 2 ? aot_perf_guest : k == 3 ? pf_proxy_ticks : 0, hz = aot_perf_hz();
  return (uint64_t)((double)t * 1e9 / (double)hz);
}
static uint64_t px_call(CPU *c, const PI *pi, int slot) {   /* timed wrapper around every guest->D3D call */
  uint64_t t0 = aot_perf_ticks();
  uint64_t r = px_call_inner(c, pi, slot);
  uint64_t dt = aot_perf_ticks() - t0;
  pf_proxy_ticks += dt; pf_calls++;
  int ti = (int)(pi - PIS); if (ti >= 0 && ti < 64) { pf_if_ticks[ti] += dt; pf_if_calls[ti]++; }
  if ((pf_calls & 255) == 0) pf_report();   /* the clock read inside costs about as much as a proxy call */
  return r;
}
#define E_NOTIMPL_ 0x80004001u
#define E_NOINTF_ 0x80004002u
#define MAGIC 0x50583130u
typedef struct { void *g, *h; size_t sz; uint32_t sub, type; void *spare; size_t spcap, gcap; int sv; } Shadow;   /* sv: the shadow still mirrors the host for its subresource */   /* spare: a retained low-memory block reused by the next Map (no allocator round trip per Map/Unmap) */
/* The guest-visible object is only {vtable, magic, id}; everything the host side relies on (host pointer, interface, map shadows) lives in host memory,
   so guest code that scribbles on a COM object (intrusive refcounts at +4/+8, ...) cannot corrupt it. */
typedef struct { void **vt; uint32_t magic, id; } GO;
typedef struct PX { uint32_t gaddr; void *host; const PI *pi; Shadow sh[4]; int qtype; } PX;   /* qtype: D3D10_QUERY + 1 for queries (0 = not a query) */
#define NPX (1 << 16)
static PX *pxs[NPX]; static uint32_t npx;
#define GN (1 << 17)
static PX *gt[GN];   /* guest address -> PX, kept host-side so guest writes to the object cannot break lookup */
static PX *px_of(uint32_t g) {
  if (!g) return NULL;
  for (unsigned i = (g >> 4) * 2654435761u >> 15 & (GN - 1), n = 0; n < GN && gt[i]; n++, i = (i + 1) & (GN - 1)) if (gt[i]->gaddr == g) return gt[i];
  return NULL;
}
static void gt_add(PX *x) { unsigned i = (x->gaddr >> 4) * 2654435761u >> 15 & (GN - 1); while (gt[i]) i = (i + 1) & (GN - 1); gt[i] = x; }
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void **vts[64];
static void **px_vt(int ti) {
  pthread_mutex_lock(&mu);
  if (!vts[ti]) {
    const PI *pi = &PIS[ti]; uint32_t *v = aot_gcalloc(pi->nm + 64, 4); char nm[96];
    if (!v) { fprintf(stderr, "[dxvk] out of 32-bit guest memory building %s vtable\n", pi->name); pthread_mutex_unlock(&mu); return NULL; }
    for (int i = 0; i < pi->nm + 64; i++) {
      if (i < pi->nm) { snprintf(nm, sizeof nm, "%s::%s", pi->name, pi->m[i].name); uint64_t t = aot_register_thunk(pi->wf[i], nm); aot_thunk_set_pop(t, 4 * (pi->m[i].n + 1)); v[i] = (uint32_t)t; }
      else { snprintf(nm, sizeof nm, "%s::vtbl[%d] (not implemented)", pi->name, i); v[i] = (uint32_t)aot_register_thunk(NULL, nm); }
    }
    vts[ti] = (void **)v;
  }
  pthread_mutex_unlock(&mu); return vts[ti];
}
#define TN (1 << 15)
static struct { void *h; int ti; GO *px; } tab[TN];
static GO *wrap(void *h, int ti) {
  if (!h) return NULL;
  if (ti < 0 || ti >= NPI) { fprintf(stderr, "[dxvk] cannot wrap host object %p: interface not in proxy table\n", h); return NULL; }
  void **vt = px_vt(ti); if (!vt) return NULL; pthread_mutex_lock(&mu);
  unsigned i = (unsigned)((((uintptr_t)h >> 4) ^ (unsigned)ti * 0x9E37u)) & (TN - 1), tomb = TN;
  for (unsigned n = 0; n < TN; n++, i = (i + 1) & (TN - 1)) {
    if (tab[i].h == h && tab[i].ti == ti) { GO *p = tab[i].px; pthread_mutex_unlock(&mu); return p; }
    if (!tab[i].h) { if (tomb == TN) tomb = i; break; }
    if (tab[i].h == (void *)1 && tomb == TN) tomb = i;
  }
  GO *p = aot_gcalloc(1, sizeof *p); PX *x = calloc(1, sizeof *x);
  if (!p || !x || npx >= NPX) { pthread_mutex_unlock(&mu); fprintf(stderr, "[dxvk] out of memory while wrapping a host object\n"); return NULL; }
  x->gaddr = (uint32_t)(uintptr_t)p; x->host = h; x->pi = &PIS[ti]; p->vt = vt; p->magic = MAGIC; p->id = npx; pxs[npx++] = x; gt_add(x);
  if (tomb == TN) { pthread_mutex_unlock(&mu); fprintf(stderr, "[dxvk] proxy table full\n"); return NULL; }
  tab[tomb].h = h; tab[tomb].ti = ti; tab[tomb].px = p; pthread_mutex_unlock(&mu); return p;
}
static void unwrap_dead(PX *p) {
  pthread_mutex_lock(&mu);
  unsigned i = (unsigned)((((uintptr_t)p->host >> 4) ^ (unsigned)(p->pi - PIS) * 0x9E37u)) & (TN - 1);
  for (unsigned n = 0; n < TN && tab[i].h; n++, i = (i + 1) & (TN - 1)) if ((uint32_t)(uintptr_t)tab[i].px == p->gaddr) { tab[i].h = (void *)1; break; }
  pthread_mutex_unlock(&mu);
}
static int ti_by_guid(const uint8_t *g) { for (int i = 0; i < NPI; i++) if (!memcmp(PIS[i].guid, g, 16)) return i; return -1; }
static int ti_by_name(const char *n) { for (int i = 0; i < NPI; i++) if (!strcmp(PIS[i].name, n)) return i; return -1; }
static int slot_by_name(const PI *pi, const char *n) { for (int i = 0; i < pi->nm; i++) if (!strcmp(pi->m[i].name, n)) return i; return -1; }
static void *hostof(uint32_t g) {
  if (!g) return NULL; PX *p = px_of(g);
  if (!p) { fprintf(stderr, "[dxvk] guest passed %#x that is not a DXVK proxy object\n", g); return NULL; }
  return p->host;
}
typedef uint64_t (*GF)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, float, float, float, float,
                       uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t hcall(void *o, int slot, const uint64_t *iv, int ni, const float *fv) {   /* iv[0] is the object */
  /* Only the argument slots the callee reads matter; zeroing all 256 bytes on every D3D call was dead work. */
  uint64_t a[24]; memcpy(a, iv, (size_t)ni * 8); if (ni < 24) memset(a + ni, 0, (size_t)(24 - ni) * 8); (void)o;
  void **hvt = *(void ***)iv[0];
  if (!hvt) { fprintf(stderr, "[dxvk] host object %#llx has no vtable (slot %d)\n", (unsigned long long)iv[0], slot); return 0x80004005u; }
  GF f = (GF)hvt[slot];
  return f(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], fv[0], fv[1], fv[2], fv[3], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15], a[16], a[17], a[18], a[19], a[20], a[21], a[22], a[23]);
}
static void conv_scd_in(const uint8_t *g, uint8_t *h) { memset(h, 0, 72); memcpy(h, g, 44); uint64_t w = LD(uint32_t, g + 44); memcpy(h + 48, &w, 8); memcpy(h + 56, g + 48, 4); memcpy(h + 60, g + 52, 4); memcpy(h + 64, g + 56, 4); }
static void conv_wstr(uint8_t *g, const uint8_t *h, int n) { for (int i = 0; i < n; i++) ST(uint16_t, g + 2 * i, LD(uint32_t, h + 4 * i)); }
static uint32_t sat(uint64_t v) { return v > 0x7fffffffu ? 0x7fffffffu : (uint32_t)v; }
typedef struct { uint8_t kind; uint8_t ti; uint32_t g; void *tmp; uint32_t n, sub, type; } Post;
/* Shadow-copy traffic, so the cost of Map/Unmap is a number instead of a guess. */
static uint64_t m_up, m_down; static uint32_t m_n; static int m_logged;
static uint64_t px_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }
static void m_report(void) {
  static uint64_t t0; uint64_t t = px_ms();
  if (!t0) { t0 = t; return; }
  if (t - t0 < 1000) return;
  fprintf(stderr, "[dxvk] maps/s=%u  up=%.1f MB/s  down=%.1f MB/s\n", (unsigned)(m_n * 1000u / (uint32_t)(t - t0)),
          (double)m_up / 1048576.0 * 1000.0 / (double)(t - t0), (double)m_down / 1048576.0 * 1000.0 / (double)(t - t0));
  t0 = t; m_n = 0; m_up = m_down = 0;
}
static uint64_t px_call_inner(CPU *c, const PI *pi, int slot) {
  /* IDXGIOutput::GetDisplayModeList (slot 8) and FindClosestMatchingMode (slot 9) are answered here instead of
     being forwarded. The Vulkan surface reports only its own size, so the in-game resolution setting had exactly
     one choice; advertising a normal mode list lets a lower resolution be picked, and DXVK scales that backbuffer
     up to the surface when presenting. Any other slot still goes to the real DXGI. */
  if (pi == &PIS[6] && (slot == 8 || slot == 9)) {
    static const uint16_t dm[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 }, { 1366, 768 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 } };
    if (slot == 9) {   /* FindClosestMatchingMode(want, *got, unused): echo the requested mode, so a size that is not the surface's is still accepted */
      const uint8_t *want = (const uint8_t *)(uintptr_t)A(1); uint8_t *got = (uint8_t *)(uintptr_t)A(2);
      if (want && got) { memcpy(got, want, 28); if (!LD(uint32_t, got + 8)) { ST(uint32_t, got + 8, 60); ST(uint32_t, got + 12, 1); } }
      return 0;
    }
    uint32_t fmt = A(1), *cnt = (uint32_t *)(uintptr_t)A(3), out = A(4);
    if (!cnt) return 0x80070057u;   /* E_INVALIDARG */
    if (fmt != 28 && fmt != 87 && fmt != 24 && fmt != 10) { *cnt = 0; return 0; }   /* only 32-bit RGB scanout formats */
    uint32_t n = (uint32_t)(sizeof dm / sizeof dm[0]);
    if (!out) { *cnt = n; return 0; }
    uint32_t k = *cnt < n ? *cnt : n;
    for (uint32_t i = 0; i < k; i++) {   /* DXGI_MODE_DESC: 28 bytes */
      uint8_t *e = (uint8_t *)(uintptr_t)(out + 28 * i);
      ST(uint32_t, e, dm[i][0]); ST(uint32_t, e + 4, dm[i][1]);
      ST(uint32_t, e + 8, 60); ST(uint32_t, e + 12, 1);   /* RefreshRate 60/1 */
      ST(uint32_t, e + 16, fmt); ST(uint32_t, e + 20, 0); ST(uint32_t, e + 24, 0);
    }
    *cnt = k;
    return k < n ? 0x887A0003u : 0;   /* DXGI_ERROR_MORE_DATA when the buffer was too small */
  }
  static int ntr; PX *o = px_of(A(0));
  if (slot < 0 || slot >= pi->nm) { fprintf(stderr, "[dxvk] %s: bad slot %d (table has %d)\n", pi->name, slot, pi->nm); return E_NOTIMPL_; }
  const PM *m = &pi->m[slot];
  static uint8_t seen[64][256]; int ti_ = (int)(pi - PIS); int first = ti_ < 64 && slot < 256 && !seen[ti_][slot]++;
  if (first || ntr < 20) { ntr++; fprintf(stderr, "[dxvk] #%d %s::%s this=%#x host=%p\n", ntr, pi->name, m->name, (unsigned)A(0), o ? o->host : NULL); }
  if (!o) { const uint32_t *w = (const uint32_t *)(uintptr_t)A(0); fprintf(stderr, "[dxvk] %s::%s called on a non-proxy object %#x [%#x %#x %#x %#x]\n", pi->name, m->name, (unsigned)A(0), w?w[0]:0, w?w[1]:0, w?w[2]:0, w?w[3]:0); return E_NOTIMPL_; }
  uint8_t ar[8192]; size_t ap = 0; uint64_t iv[32]; float fv[4] = { 0 }; int ni = 0, nf = 0, np = 0; void *ot[8]; int nt = 0; Post post[8];
  /* Anything other than Map/Unmap may make the host contents of this resource differ from the shadow
     (copies into it, UpdateSubresource, GPU use), so the shadow stops being a usable mirror. */
  if (!(m->name[0] == 'M' && !strcmp(m->name, "Map")) && !(m->name[0] == 'U' && !strcmp(m->name, "Unmap")))
    for (int i = 0; i < 4; i++) o->sh[i].sv = 0;
  if ((m->name[0] == 'U' && !strcmp(m->name, "UpdateSubresource")) || (m->name[0] == 'C' && !strncmp(m->name, "Copy", 4)) || (m->name[0] == 'R' && !strcmp(m->name, "ResolveSubresource")))
    for (uint32_t pi_ = 0; pi_ < npx; pi_++) for (int k_ = 0; k_ < 4; k_++) pxs[pi_]->sh[k_].sv = 0;   /* these write into OTHER resources behind their shadows' backs */
#define ALLOC(sz) ({ size_t _s = ((sz) + 15) & ~15u; if (ap + _s > sizeof ar) { fprintf(stderr, "[dxvk] %s::%s: argument too large for marshalling arena\n", pi->name, m->name); return E_NOTIMPL_; } void *_p = ar + ap; ap += _s; memset(_p, 0, _s); _p; })
  if (m->name[0] == 'U' && !strcmp(m->name, "Unmap")) {   /* flush a low-memory shadow back into the host mapping */
    uint32_t sub = m->n ? A(1) : 0;
    for (int i = 0; i < 4; i++) if (o->sh[i].g && o->sh[i].sub == sub) { Shadow *s = &o->sh[i];
      if (s->type != 1) { memcpy(s->h, s->g, s->sz); m_down += s->sz; }   /* READ maps wrote nothing back */
      if (!s->spare && s->gcap <= (256u << 10)) { s->spare = s->g; s->spcap = s->gcap; } else { aot_gfree(s->g); s->sv = 0; } s->g = NULL; m_n++; m_report(); break; }
  }
  if (m->name[0] == 'C' && !strcmp(m->name, "CreateBuffer") && A(1) && LD(uint32_t, A(1)) == 0) {
    /* The game asks for a zero-size buffer (a shader without constant buffers); native D3D10 drivers tolerated this, DXVK rejects it. Give it the smallest valid buffer instead of a NULL the game does not check. */
    static int warned; if (!warned++) fprintf(stderr, "[dxvk] CreateBuffer with ByteWidth 0 (usage=%u bind=%#x): substituting a 16-byte buffer\n", LD(uint32_t, A(1) + 4), LD(uint32_t, A(1) + 8));
    ST(uint32_t, A(1), 16);
  }
  int new_qtype = 0; if (m->name[0] == 'C' && (!strcmp(m->name, "CreateQuery") || !strcmp(m->name, "CreatePredicate")) && A(1)) new_qtype = (int)LD(uint32_t, A(1)) + 1;
  iv[ni++] = (uint64_t)(uintptr_t)o->host;
  for (int i = 0; i < m->n; i++) {
    uint32_t v = A(i + 1); char k = m->k[i];
    switch (k) {
    case 'u': case 'v': iv[ni++] = v; break;
    case 'f': memcpy(&fv[nf++], &v, 4); break;
    case 'o': iv[ni++] = (uint64_t)(uintptr_t)hostof(v); break;
    case 'O': if (!v) { iv[ni++] = 0; break; } ot[nt] = NULL; iv[ni++] = (uint64_t)(uintptr_t)&ot[nt]; post[np++] = (Post){ 'O', m->ty[i], v, &ot[nt], 0, 0, 0 }; nt++; break;
    case 'R': { if (!v) { iv[ni++] = 0; break; } ot[nt] = NULL; iv[ni++] = (uint64_t)(uintptr_t)&ot[nt]; post[np++] = (Post){ 'R', 0, v, &ot[nt], A(m->ci[i] + 1), 0, 0 }; nt++; break; }
    case 'A': { uint32_t n = m->ci[i] == 255 ? 0 : A(m->ci[i] + 1); if (!v || !n) { iv[ni++] = 0; break; } uint64_t *h = ALLOC(n * 8); for (uint32_t j = 0; j < n; j++) h[j] = (uint64_t)(uintptr_t)hostof(LD(uint32_t, v + 4 * j)); iv[ni++] = (uint64_t)(uintptr_t)h; break; }
    case 'B': { uint32_t n = A(m->ci[i] + 1); if (!v || !n) { iv[ni++] = 0; break; } void *h = ALLOC(n * 8); iv[ni++] = (uint64_t)(uintptr_t)h; post[np++] = (Post){ 'B', m->ty[i], v, h, n, 0, 0 }; break; }
    case 'd': { uint32_t n = 1; const char *mn = m->name; uint32_t d = A(1);
      if (!strcmp(mn, "CreateTexture1D")) n = LD(uint32_t, d + 4) * LD(uint32_t, d + 8); else if (!strcmp(mn, "CreateTexture2D")) n = LD(uint32_t, d + 8) * LD(uint32_t, d + 12); else if (!strcmp(mn, "CreateTexture3D")) n = LD(uint32_t, d + 12);
      if (!v) { iv[ni++] = 0; break; } uint8_t *h = ALLOC(n * 16); for (uint32_t j = 0; j < n; j++) { uint64_t p = LD(uint32_t, v + 12 * j); memcpy(h + 16 * j, &p, 8); memcpy(h + 16 * j + 8, (void *)(uintptr_t)(v + 12 * j + 4), 8); } iv[ni++] = (uint64_t)(uintptr_t)h; break; }
    case 'e': { uint32_t n = A(m->ci[i] + 1); if (!v) { iv[ni++] = 0; break; } uint8_t *h = ALLOC(n * 32); for (uint32_t j = 0; j < n; j++) { uint64_t p = LD(uint32_t, v + 28 * j); memcpy(h + 32 * j, &p, 8); memcpy(h + 32 * j + 8, (void *)(uintptr_t)(v + 28 * j + 4), 24); } iv[ni++] = (uint64_t)(uintptr_t)h; break; }
    case 's': { uint32_t n = A(m->ci[i] + 1); if (!v) { iv[ni++] = 0; break; } uint8_t *h = ALLOC(n * 16); for (uint32_t j = 0; j < n; j++) { uint64_t p = LD(uint32_t, v + 12 * j); memcpy(h + 16 * j, &p, 8); memcpy(h + 16 * j + 8, (void *)(uintptr_t)(v + 12 * j + 4), 8); } iv[ni++] = (uint64_t)(uintptr_t)h; break; }
    case 'c': { if (!v) { iv[ni++] = 0; break; } uint8_t *h = ALLOC(72); conv_scd_in((const uint8_t *)(uintptr_t)v, h); iv[ni++] = (uint64_t)(uintptr_t)h; break; }
    case 'C': case 'a': case 'b': case 'w': { if (!v) { iv[ni++] = 0; break; } void *h = ALLOC(640); iv[ni++] = (uint64_t)(uintptr_t)h; post[np++] = (Post){ k, 0, v, h, 0, 0, 0 }; break; }
    case 'M': case 'T': case 'U': { if (!v) { iv[ni++] = 0; break; } void *h = ALLOC(32); iv[ni++] = (uint64_t)(uintptr_t)h; post[np++] = (Post){ k, 0, v, h, 0, k == 'M' ? 0 : A(1), A(k == 'M' ? 1 : 2) }; break; }
    default: fprintf(stderr, "[dxvk] %s::%s is not supported by the 32->64 proxy\n", pi->name, m->name); return m->ret ? E_NOTIMPL_ : 0;
    }
  }
  int is_gd = m->name[0] == 'G' && !strcmp(m->name, "GetData") && ni >= 4;
  if (is_gd) iv[3] &= ~1ull;   /* D3D10_ASYNC_GETDATA_DONOTFLUSH: native drivers still make progress on their own, DXVK only submits work on a flush, so the game polls forever */
  int is_present = m->name[0] == 'P' && !strcmp(m->name, "Present") && ni >= 2;
  if (is_present) iv[1] = 0;   /* no vsync wait: SyncInterval 0 (the game asks for 1, which makes every frame a multiple of the refresh period) */
  uint64_t pt0 = is_present ? px_now_ns() : 0;
  uint64_t r = hcall(o->host, slot, iv, ni, fv); uint32_t hr = (uint32_t)r;
  if (is_present) { __atomic_add_fetch(&aot_ns[0], px_now_ns() - pt0, __ATOMIC_RELAXED); __atomic_add_fetch(&aot_frames, 1, __ATOMIC_RELAXED); aot_present_tick(); }
  int occl = is_gd && (o->qtype == 2 || o->qtype == 6 || o->qtype == 8);   /* OCCLUSION, OCCLUSION_PREDICATE, SO_OVERFLOW_PREDICATE: culling hints, never worth stalling the frame on the GPU */
  if (occl && hr == 1) {   /* not ready: answer "visible" now. Culling is only an optimisation; waiting for it serialised CPU and GPU every frame */
    uint64_t pd = iv[1]; uint32_t sz = (uint32_t)iv[2]; int pred = o->qtype != 2;   /* predicate: BOOL FALSE = not occluded; occlusion: a large sample count */
    if (pd && sz >= 8) *(uint64_t *)(uintptr_t)pd = pred ? 0 : 1000000; else if (pd && sz >= 4) *(uint32_t *)(uintptr_t)pd = pred ? 0 : 1000000;
    hr = 0; r = 0; }
  if (is_gd && hr == 1 && g_hdev && g_flush_slot >= 0) {   /* S_FALSE on an event/timestamp query: submit once, then wait for the GPU HERE (short host sleeps) instead of bouncing back to the guest's SwitchToThread spin */
    uint64_t fi[1] = { (uint64_t)(uintptr_t)g_hdev }; float z[4] = { 0 }; hcall(g_hdev, g_flush_slot, fi, 1, z);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) { r = hcall(o->host, slot, iv, ni, fv); hr = (uint32_t)r; if (hr != 1) break;
      clock_gettime(CLOCK_MONOTONIC, &t1); if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 >= 40) break;
      usleep(100); }
    __atomic_add_fetch(&aot_ns[1], px_now_ns() - ((uint64_t)t0.tv_sec * 1000000000ull + (uint64_t)t0.tv_nsec), __ATOMIC_RELAXED); }
  if (is_gd && hr == 1) {   /* still not ready after 40 ms: report it finished so the game cannot stall forever (BOOL/UINT64 = 1) */
    uint64_t pd = iv[1]; uint32_t sz = (uint32_t)iv[2]; if (pd && sz >= 8) *(uint64_t *)(uintptr_t)pd = 1; else if (pd && sz >= 4) *(uint32_t *)(uintptr_t)pd = 1;
    static int warned; if (!warned++) fprintf(stderr, "[dxvk] GPU query still not ready after 40 ms: reporting it as finished (result 1)\n");
    hr = 0; r = 0; } if (m->ret && (int32_t)hr < 0) {
    fprintf(stderr, "[dxvk] %s::%s FAILED hr=%#x args:", pi->name, m->name, hr);
    for (int i = 0; i < m->n && i < 6; i++) { uint32_t a = A(i + 1); fprintf(stderr, " %#x", a);
      if (a > 0x10000 && a < 0x80000000u) { const uint32_t *w = (const uint32_t *)(uintptr_t)a; fprintf(stderr, "[%x %x %x %x %x %x]", w[0], w[1], w[2], w[3], w[4], w[5]); } }
    fprintf(stderr, "\n");
  } int ok = !m->ret || (int32_t)hr >= 0 || m->name[0] != 'C';
  for (int p = 0; p < np; p++) {
    Post *q = &post[p]; const uint8_t *h = q->tmp;
    switch (q->kind) {
    case 'O': { void *hp = *(void **)q->tmp; if (hp) { GO *x = wrap(hp, q->ti); G32(q->g, x); if (new_qtype && x) { PX *qx = px_of((uint32_t)(uintptr_t)x); if (qx) qx->qtype = new_qtype; } } else G32(q->g, 0); break; }
    case 'R': { void *hp = *(void **)q->tmp; uint8_t id[16]; memcpy(id, (void *)(uintptr_t)q->n, 16); static const uint8_t unk[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46 };
      int ti = !memcmp(id, unk, 16) ? (int)(pi - PIS) : ti_by_guid(id);
      if (!hp) { G32(q->g, 0); break; }
      if (ti < 0) { fprintf(stderr, "[dxvk] %s::%s: guest asked for an interface that is not in the proxy table (%02x%02x%02x%02x-...)\n", pi->name, m->name, id[3], id[2], id[1], id[0]); void **vt = *(void ***)hp; ((uint32_t (*)(void *))vt[2])(hp); G32(q->g, 0); hr = E_NOINTF_; r = hr; break; }
      G32(q->g, wrap(hp, ti)); break; }
    case 'B': { for (uint32_t j = 0; j < q->n; j++) { void *hp = ((void **)q->tmp)[j]; G32(q->g + 4 * j, hp ? wrap(hp, q->ti) : NULL); } break; }
    case 'C': { uint8_t *g = (uint8_t *)(uintptr_t)q->g; memcpy(g, h, 44); memcpy(g + 44, h + 48, 4); memcpy(g + 48, h + 56, 4); memcpy(g + 52, h + 60, 4); memcpy(g + 56, h + 64, 4); break; }
    case 'a': case 'b': { uint8_t *g = (uint8_t *)(uintptr_t)q->g; conv_wstr(g, h, 128); memcpy(g + 256, h + 512, 16); ST(uint32_t, g + 272, sat(LD(uint64_t, h + 528))); ST(uint32_t, g + 276, sat(LD(uint64_t, h + 536))); ST(uint32_t, g + 280, sat(LD(uint64_t, h + 544))); memcpy(g + 284, h + 552, 8); if (q->kind == 'b') memcpy(g + 292, h + 560, 4); break; }
    case 'w': { uint8_t *g = (uint8_t *)(uintptr_t)q->g; conv_wstr(g, h, 32); memcpy(g + 64, h + 128, 16); memcpy(g + 80, h + 144, 8); ST(uint32_t, g + 88, LD(uint64_t, h + 152)); break; }
    case 'M': case 'T': case 'U': {
      if ((int32_t)hr < 0) break;
      void *hp = *(void **)h; size_t sz = 0; uint8_t d[160]; memset(d, 0, sizeof d);
      static int16_t gd_of[64]; static uint8_t gd_known[64];   /* GetDesc slot: was a string scan per Map */
      int ti_ = (int)(pi - PIS); if (ti_ >= 0 && ti_ < 64 && !gd_known[ti_]) { gd_of[ti_] = (int16_t)slot_by_name(pi, "GetDesc"); gd_known[ti_] = 1; }
      int gd = (ti_ >= 0 && ti_ < 64) ? gd_of[ti_] : slot_by_name(pi, "GetDesc");
      uint64_t gi[2] = { (uint64_t)(uintptr_t)o->host, (uint64_t)(uintptr_t)d }; float z[4] = { 0 };
      if (gd >= 0) hcall(o->host, gd, gi, 2, z);
      if (q->kind == 'M') sz = LD(uint32_t, d);
      else { uint32_t w = LD(uint32_t, d), ht = LD(uint32_t, d + 4), dp = q->kind == 'U' ? LD(uint32_t, d + 8) : 1, ml = LD(uint32_t, d + (q->kind == 'U' ? 12 : 8)), fmt = LD(uint32_t, d + 16), mip = ml ? q->sub % ml : 0; (void)w;
        ht = ht >> mip; if (!ht) ht = 1; if ((fmt >= 70 && fmt <= 84) || (fmt >= 94 && fmt <= 99)) ht = (ht + 3) / 4; dp = dp >> mip; if (!dp) dp = 1;
        uint32_t pitch = LD(uint32_t, h + 8); sz = (size_t)pitch * ht; if (q->kind == 'U') sz = (size_t)LD(uint32_t, h + 12) * dp; }
      if (!hp || !sz) { fprintf(stderr, "[dxvk] %s::Map: cannot size the mapped region (sz=%zu)\n", pi->name, sz); break; }
      int si = -1; for (int i = 0; i < 4; i++) if (!o->sh[i].g) { si = i; break; }
      if (si < 0) { fprintf(stderr, "[dxvk] too many concurrent maps on one resource\n"); break; }
      Shadow *sp_ = &o->sh[si]; void *g = NULL; size_t gcap = sz; int reused = 0; if (sp_->spare && sp_->spcap >= sz) { reused = 1; g = sp_->spare; gcap = sp_->spcap; sp_->spare = NULL; sp_->spcap = 0; } else { if (sp_->spare) { aot_gfree(sp_->spare); sp_->spare = NULL; sp_->spcap = 0; } g = aot_gmalloc(sz); } if (!g) { fprintf(stderr, "[dxvk] Map: out of low memory for %zu bytes\n", sz); break; }
      /* What has to be copied in is decided by the map mode, so only the modes that need it pay:
           DISCARD (4)  - the contents are undefined, the app writes what it uses: no copy, no zeroing
                          (zeroing the whole subresource per map was pure traffic for nothing);
           NO_OVERWRITE (5) - the app cannot read the old contents, and the shadow still mirrors the host
                          unless something else touched the resource in between (sv);
           READ/WRITE/READ_WRITE - the app may read, so the current contents must be there. */
      if (q->type == 4) { }
      else if (q->type == 5 && reused && sp_->sv && sp_->sub == q->sub) { }   /* only a retained block still holds the host contents; a fresh one is garbage */
      else { memcpy(g, hp, sz); m_up += sz; }
      if (!m_logged) { m_logged = 1; fprintf(stderr, "[dxvk] Map: DISCARD maps are not zero-filled; NO_OVERWRITE maps skip the up-copy while the shadow is current\n"); }
      sp_->g = g; sp_->h = hp; sp_->sz = sz; sp_->sub = q->sub; sp_->type = q->type; sp_->gcap = gcap; sp_->sv = 1;
      uint32_t *gs = (uint32_t *)(uintptr_t)q->g; if (q->kind == 'M') gs[0] = (uint32_t)(uintptr_t)g; else { gs[0] = (uint32_t)(uintptr_t)g; gs[1] = LD(uint32_t, h + 8); if (q->kind == 'U') gs[2] = LD(uint32_t, h + 12); }
      break; }
    }
  }
  (void)ok;
  if (m->name[0] == 'R' && !strcmp(m->name, "Release") && (uint32_t)r == 0) unwrap_dead(o);
  return m->ret ? (uint32_t)r : 0;
}
/* ---- creation: D3D10CreateDeviceAndSwapChain -> real DXVK ---- */
static void *hd[3]; static int inited; static void *g_fac; static int g_tier = -1;
extern void *aot_present_window(void);
static void *sym(const char *n) { for (int i = 0; i < 3; i++) if (hd[i]) { void *p = dlsym(hd[i], n); if (p) return p; } return NULL; }
/* DXVK tiers, newest first. Tier 0 keeps the original library names; the others carry a suffix so all three sets can sit side by side in the APK
   (build.sh renames them and rewrites their DT_NEEDED entries). A tier is taken only if a real probe works on this phone's Vulkan driver:
   instance + adapter + D3D10 device. Otherwise the next, older tier is tried; if none works the caller falls back to the built-in emulation. */
#define NTIER 3
static const char *const tier_sfx[NTIER] = { "", "_t1", "_t2" };
static const char *const tier_ver[NTIER] = { "3.1.1", "2.4.1", "1.9.2 (native 1.x line)" };
static void *g_win;
/* Load one tier's three libraries and hand them the window. No device is created here: tier 0 then runs exactly the path it always did, and a
   newer tier is only given up on if the real factory/adapter/device creation below fails on this phone's driver. */
static int load_tier(int t) {
  static const char *const base[3] = { "libdxvk_dxgi", "libdxvk_d3d11", "libdxvk_d3d10core" };
  void *h[3];
  for (int i = 0; i < 3; i++) {
    char n[64]; snprintf(n, sizeof n, "%s%s.so", base[i], tier_sfx[t]);
    h[i] = dlopen(n, RTLD_NOW | (t == 0 ? RTLD_GLOBAL : RTLD_LOCAL));   /* older tiers stay local so their symbols never bind into another tier */
    if (!h[i]) { fprintf(stderr, "[dxvk] tier %d (DXVK %s): %s unavailable: %s\n", t, tier_ver[t], n, dlerror()); return -1; }
  }
  for (int i = 0; i < 3; i++) { hd[i] = h[i]; void (*sw)(void *) = dlsym(hd[i], "dxvk_android_set_window"); if (sw) sw(g_win); }
  g_fac = NULL; g_tier = t; return 0;
}
static int next_tier(void) {   /* the current tier failed to produce a device: move to the next older one that is present */
  for (int t = g_tier + 1; t < NTIER; t++) if (load_tier(t) == 0) { fprintf(stderr, "[dxvk] falling back to DXVK %s (tier %d)\n", tier_ver[t], t); return 1; }
  return 0;
}
static int init_dxvk(void) {
  if (inited) return inited > 0 ? 0 : -1; inited = -1;
  if (!aot_is_pe32) return -1;
  setenv("DXVK_LOG_PATH", "none", 0);
  /* The state cache is what carries compiled pipelines from one run to the next; with it off every pipeline is
     compiled again, which is the dxvk-shader-<n> CPU seen in the profile. The game data folder is this process's
     working directory (android_main.c and win_file.c chdir into it before any D3D object exists), so name it
     explicitly instead of inheriting whatever cwd we were started with. overwrite=0 keeps a user override. */
  { char cd[512]; if (getcwd(cd, sizeof cd)) setenv("DXVK_STATE_CACHE_PATH", cd, 0); }
  setenv("DXVK_STATE_CACHE", "1", 0);
  /* Which Vulkan driver this run uses has to be decided before DXVK creates its instance: the system driver leaves the
     environment alone, a bundled Turnip driver points DXVK at libaotvkicd.so and falls back to the system driver if
     it does not come up on this phone. */
  aot_gpu_apply();
  g_win = aot_present_window(); if (!g_win) { fprintf(stderr, "[dxvk] no Android window yet\n"); return -1; }
  { char b[32]; snprintf(b, sizeof b, "%llx", (unsigned long long)(uintptr_t)g_win); setenv("AOT_ANDROID_WINDOW", b, 1); }
  int t0 = 0; { const char *e = getenv("AOT_DXVK_TIER"); if (e && e[0] >= '0' && e[0] < '0' + NTIER) t0 = e[0] - '0'; }   /* testing: start at an older tier */
  g_tier = t0 - 1;
  if (!next_tier()) { fprintf(stderr, "[dxvk] no DXVK tier is available (using the built-in D3D10 emulation)\n"); return -1; }
  fprintf(stderr, "[dxvk] using DXVK %s (tier %d)\n", tier_ver[g_tier], g_tier);
  inited = 1; return 0;
}
static void *fac_get(void) {
  if (g_fac || init_dxvk() != 0) return g_fac;
  uint32_t (*cf)(const void *, void **) = (uint32_t (*)(const void *, void **))sym("CreateDXGIFactory1"); int tf = ti_by_name("IDXGIFactory1");
  if (!cf || tf < 0) return NULL;
  uint32_t hr = cf(PIS[tf].guid, &g_fac); if ((int32_t)hr < 0) { fprintf(stderr, "[dxvk] CreateDXGIFactory1 failed %#x\n", hr); g_fac = NULL; }
  return g_fac;
}
/* the game's own CreateDXGIFactory must return a DXVK factory, otherwise its swapchain is the old emulated one */
uint64_t aot_dxvk_factory(uint64_t out) {
  void *f = fac_get(); if (!f) return (uint64_t)-1;
  G32(out, wrap(f, ti_by_name("IDXGIFactory1"))); return 0;
}
uint64_t aot_dxvk_d3d10_create(uint64_t dev_out, uint64_t sc_out, uint64_t sc_desc) {
  if (init_dxvk() != 0) return (uint64_t)-1;
  typedef uint32_t (*CF)(const void *, void **); typedef uint32_t (*CD)(void *, void *, uint32_t, uint32_t, void **);
  int tf = ti_by_name("IDXGIFactory1"), tdev = ti_by_name("ID3D10Device"), tsc = ti_by_name("IDXGISwapChain");
  void *fac = NULL, *ad = NULL, *dev = NULL, *sc = NULL; uint32_t hr = 0;
  for (;;) {   /* one pass per tier: the first tier whose driver support is good enough to create the device wins */
    CF cf = (CF)sym("CreateDXGIFactory1"); CD cd = (CD)sym("D3D10CoreCreateDevice");
    ad = NULL; dev = NULL; hr = (uint32_t)-1;
    if (!cf || !cd) fprintf(stderr, "[dxvk] missing entry points in tier %d\n", g_tier);
    else if (!(fac = fac_get())) fprintf(stderr, "[dxvk] tier %d: no factory\n", g_tier);
    else {
      { uint64_t iv[3] = { (uint64_t)(uintptr_t)fac, 0, (uint64_t)(uintptr_t)&ad }; float z[4] = { 0 }; hr = (uint32_t)hcall(fac, slot_by_name(&PIS[tf], "EnumAdapters"), iv, 3, z); }
      if ((int32_t)hr < 0) fprintf(stderr, "[dxvk] EnumAdapters failed %#x\n", hr);
      else { hr = cd(fac, ad, 0, 0xA000, &dev); if ((int32_t)hr < 0) fprintf(stderr, "[dxvk] D3D10CoreCreateDevice failed %#x\n", hr); }
    }
    if ((int32_t)hr >= 0 && dev) break;
    if (!next_tier()) return (int32_t)hr < 0 ? hr : (uint64_t)-1;
  }
  if (sc_out) {
    uint8_t d[72]; conv_scd_in((const uint8_t *)(uintptr_t)sc_desc, d);
    uint64_t iv[4] = { (uint64_t)(uintptr_t)fac, (uint64_t)(uintptr_t)dev, (uint64_t)(uintptr_t)d, (uint64_t)(uintptr_t)&sc }; float z[4] = { 0 };
    hr = (uint32_t)hcall(fac, slot_by_name(&PIS[tf], "CreateSwapChain"), iv, 4, z);
    if ((int32_t)hr < 0) { fprintf(stderr, "[dxvk] CreateSwapChain failed %#x\n", hr); return hr; }
    G32(sc_out, wrap(sc, tsc));
  }
  fprintf(stderr, "[dxvk] host ID3D10Device=%p vtbl=%p\n", dev, dev ? *(void **)dev : NULL);
  g_hdev = dev; g_flush_slot = slot_by_name(&PIS[tdev], "Flush");
  G32(dev_out, wrap(dev, tdev)); fprintf(stderr, "[dxvk] D3D10 device + swapchain created through DXVK\n"); return 0;
}
