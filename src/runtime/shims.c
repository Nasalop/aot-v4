#include <pthread.h>
/* Win64 API / CRT shims. Each takes the guest CPU (Win64 ABI: rcx,rdx,r8,r9, then stack). Add more here. */
#define _GNU_SOURCE
#include "shims.h"
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/time.h>
#define malloc aot_gmalloc
#define calloc aot_gcalloc
#define realloc aot_grealloc
#define free aot_gfree

#define A0 (c->r[1])
#define A1 (c->r[2])
#define A2 (c->r[8])
#define A3 (c->r[9])
static inline uint64_t argn(CPU *c, int n) { return n == 0 ? c->r[1] : n == 1 ? c->r[2] : n == 2 ? c->r[8] : n == 3 ? c->r[9] : LD(uint64_t, c->r[4] + 8 + 8 * n); }
#define ARG(n) argn(c, (n))
#define SH(n) static uint64_t sh_##n(CPU *c)
#define STR(p) ((const char *)(uintptr_t)(p))
#define FD(h) ((int)((h) - 0x10))
#define DBL(i) (c->x[i].g[0])
#define FLT(i) (c->x[i].f[0])

static int g_argc; static char **g_argv; __thread uint32_t aot_last_error;
static char g_iob[3 * 0x58]; static __thread uint64_t g_tls[1088];   /* per-thread slots: Win32 TLS is per thread (CRT _ptiddata lives here) */ static int g_tls_n;
static uint64_t g_heap = 0x7000;
static uint64_t g_std_handles[3] = { 0x10, 0x11, 0x12 };
static char *g_env_w;
extern char **environ;

void shims_init(int argc, char **argv) { g_argc = argc; g_argv = argv; aot_win_file_init(); }

/* ---- printf engine ------------------------------------------------------ */
typedef struct { CPU *c; const uint64_t *arr; int i; } Va;
static uint64_t va_next(Va *v) { if (v->arr) return v->arr[v->i++]; CPU *c = v->c; return ARG(v->i++); }
static int wfmt(char *out, size_t cap, const char *f, Va *v) {
  size_t n = 0; char tmp[512];
#define PUT(s, l) do { size_t _l = (l); for (size_t _k = 0; _k < _l; _k++) { if (n + 1 < cap) out[n] = (s)[_k]; n++; } } while (0)
  for (; *f; f++) {
    if (*f != '%') { PUT(f, 1); continue; }
    char spec[64]; int sp = 0; spec[sp++] = '%'; f++;
    while (strchr("-+ #0", *f) && *f) spec[sp++] = *f++;
    if (*f == '*') { sp += snprintf(spec + sp, 20, "%d", (int)va_next(v)); f++; } else while (*f >= '0' && *f <= '9') spec[sp++] = *f++;
    if (*f == '.') { spec[sp++] = *f++; if (*f == '*') { sp += snprintf(spec + sp, 20, "%d", (int)va_next(v)); f++; } else while (*f >= '0' && *f <= '9') spec[sp++] = *f++; }
    int big = 0, half = 0;
    for (;; f++) { if (*f == 'l' && f[1] == 'l') { big = 1; f++; } else if (*f == 'l' || *f == 'L') big = 0; else if (*f == 'I' && f[1] == '6' && f[2] == '4') { big = 1; f += 2; } else if (*f == 'z' || *f == 'j' || *f == 't') big = 1; else if (*f == 'h') half++; else if (*f == 'w') {} else break; }
    char cv = *f; if (!cv) break; int l;
    switch (cv) {
      case '%': PUT("%", 1); continue;
      case 'd': case 'i': { spec[sp++] = 'l'; spec[sp++] = 'l'; spec[sp++] = 'd'; spec[sp] = 0; uint64_t a = va_next(v); long long x = big ? (long long)a : half == 1 ? (short)a : half ? (signed char)a : (int)a; l = snprintf(tmp, sizeof tmp, spec, x); break; }
      case 'u': case 'x': case 'X': case 'o': { spec[sp++] = 'l'; spec[sp++] = 'l'; spec[sp++] = cv; spec[sp] = 0; uint64_t a = va_next(v); unsigned long long x = big ? a : half == 1 ? (unsigned short)a : half ? (unsigned char)a : (unsigned)a; l = snprintf(tmp, sizeof tmp, spec, x); break; }
      case 'p': l = snprintf(tmp, sizeof tmp, "%016llX", (unsigned long long)va_next(v)); break;
      case 'c': { char ch = (char)va_next(v); PUT(&ch, 1); continue; }
      case 's': { spec[sp++] = 's'; spec[sp] = 0; const char *s = (const char *)(uintptr_t)va_next(v); if (!s) s = "(null)"; size_t sl = strlen(s); char *b = malloc(sl + 512); l = snprintf(b, sl + 512, spec, s); PUT(b, (size_t)l); free(b); continue; }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': { spec[sp++] = cv; spec[sp] = 0; uint64_t a = va_next(v); double d; memcpy(&d, &a, 8); l = snprintf(tmp, sizeof tmp, spec, d); break; }
      default: PUT(f, 1); continue;
    }
    PUT(tmp, (size_t)l);
  }
  if (cap) out[n < cap ? n : cap - 1] = 0;
  return (int)n;
}
static int fd_of(uint64_t f) { return f >= (uint64_t)(uintptr_t)g_iob && f < (uint64_t)(uintptr_t)g_iob + sizeof g_iob ? (int)((f - (uint64_t)(uintptr_t)g_iob) / 0x58) : 1; }
static int out_fmt(int fd, const char *fmt, Va *v) { char *b = malloc(65536); int n = wfmt(b, 65536, fmt, v); if (n > 65535) n = 65535; fflush(stdout); write(fd, b, n); free(b); return n; }

SH(printf) { Va v = {c, 0, 1}; return out_fmt(1, STR(A0), &v); }
SH(fprintf) { Va v = {c, 0, 2}; return out_fmt(fd_of(A0), STR(A1), &v); }
SH(sprintf) { Va v = {c, 0, 2}; return wfmt((char *)A0, (size_t)-1 >> 1, STR(A1), &v); }
SH(snprintf) { Va v = {c, 0, 3}; return wfmt((char *)A0, A1, STR(A2), &v); }
SH(vfprintf_ucrt) { Va v = {c, (const uint64_t *)(uintptr_t)ARG(4), 0}; return out_fmt(fd_of(A1), STR(A2), &v); }
SH(vsprintf_ucrt) { Va v = {c, (const uint64_t *)(uintptr_t)ARG(5), 0}; return wfmt((char *)A1, A2 ? A2 : (size_t)-1 >> 1, STR(A3), &v); }
SH(vprintf) { Va v = {c, (const uint64_t *)(uintptr_t)A1, 0}; return out_fmt(1, STR(A0), &v); }
SH(puts) { fflush(stdout); write(1, STR(A0), strlen(STR(A0))); write(1, "\n", 1); return 1; }
SH(putchar) { char ch = (char)A0; write(1, &ch, 1); return A0 & 0xff; }
SH(fputs) { write(fd_of(A1), STR(A0), strlen(STR(A0))); return 1; }
SH(fwrite) { fflush(stdout); write(fd_of(A3), STR(A0), A1 * A2); return A2; }
SH(fflush) { fflush(stdout); return 0; }
SH(iob_func) { return (uint64_t)(uintptr_t)g_iob + (A0 > 2 ? 0 : A0) * 0x58; }
SH(iob_array) { return (uint64_t)(uintptr_t)g_iob; }

/* ---- memory / string ------------------------------------------------------ */
SH(malloc) { return (uint64_t)malloc(A0 ? A0 : 1); }
SH(calloc) { return (uint64_t)calloc(A0 ? A0 : 1, A1 ? A1 : 1); }
SH(realloc) { return (uint64_t)realloc((void *)A0, A1); }
SH(free) { free((void *)A0); return 0; }
SH(memcpy) { memcpy((void *)A0, (void *)A1, A2); return A0; }
SH(memmove) { memmove((void *)A0, (void *)A1, A2); return A0; }
SH(memset) { memset((void *)A0, (int)A1, A2); return A0; }
SH(memcmp) { return (uint64_t)(int64_t)memcmp((void *)A0, (void *)A1, A2); }
SH(strlen) { return strlen(STR(A0)); }
SH(strcmp) { return (uint64_t)(int64_t)strcmp(STR(A0), STR(A1)); }
SH(strncmp) { return (uint64_t)(int64_t)strncmp(STR(A0), STR(A1), A2); }
SH(strcpy) { return (uint64_t)strcpy((char *)A0, STR(A1)); }
SH(strncpy) { return (uint64_t)strncpy((char *)A0, STR(A1), A2); }
SH(strcat) { return (uint64_t)strcat((char *)A0, STR(A1)); }
SH(strchr) { return (uint64_t)strchr(STR(A0), (int)A1); }
SH(strstr) { return (uint64_t)strstr(STR(A0), STR(A1)); }
SH(atoi) { return (uint64_t)(int64_t)atoi(STR(A0)); }
SH(rand) { return (uint64_t)rand(); }
SH(srand) { srand((unsigned)A0); return 0; }
SH(abs) { int v = (int)A0; return (uint64_t)(uint32_t)(v < 0 ? -v : v); }
SH(exit) { fflush(stdout); exit((int)A0); }
SH(abort) { fflush(stdout); abort(); }
SH(zero) { (void)c; return 0; }
SH(one) { (void)c; return 1; }
SH(initterm) { for (uint64_t *p = (uint64_t *)A0; p < (uint64_t *)A1; p++) if (*p) aot_call(c, *p, 0); return 0; }
SH(initterm_e) { for (uint64_t *p = (uint64_t *)A0; p < (uint64_t *)A1; p++) if (*p) { uint64_t r = aot_call(c, *p, 0); if ((uint32_t)r) return (uint32_t)r; } return 0; }
SH(getmainargs) { static int ac; static char *av[66], *ev[1]; ac = g_argc < 64 ? g_argc : 64; for (int i = 0; i < ac; i++) av[i] = g_argv[i]; av[ac] = 0;
  ST(int32_t, A0, ac); ST(uint64_t, A1, av); ST(uint64_t, A2, ev); return 0; }
SH(p_argc) { static int ac; ac = g_argc; return (uint64_t)&ac; }
SH(p_argv) { static char *av[66]; for (int i = 0; i < g_argc && i < 64; i++) av[i] = g_argv[i]; static char **p = av; return (uint64_t)&p; }
SH(p_fmode) { static int v; return (uint64_t)&v; }
SH(get_narrow_env) { static char *ev[1]; return (uint64_t)ev; }
SH(acmdln) { static char *p; p = g_cmdline; return (uint64_t)&p; }
SH(sqrt) { DBL(0) = sqrt(DBL(0)); return 0; }
SH(sin) { DBL(0) = sin(DBL(0)); return 0; }
SH(cos) { DBL(0) = cos(DBL(0)); return 0; }
SH(tan) { DBL(0) = tan(DBL(0)); return 0; }
SH(atan) { DBL(0) = atan(DBL(0)); return 0; }
SH(atan2) { DBL(0) = atan2(DBL(0), DBL(1)); return 0; }
SH(exp) { DBL(0) = exp(DBL(0)); return 0; }
SH(log) { DBL(0) = log(DBL(0)); return 0; }
SH(pow) { DBL(0) = pow(DBL(0), DBL(1)); return 0; }
SH(floor) { DBL(0) = floor(DBL(0)); return 0; }
SH(ceil) { DBL(0) = ceil(DBL(0)); return 0; }
SH(fabs) { DBL(0) = fabs(DBL(0)); return 0; }
SH(fmod) { DBL(0) = fmod(DBL(0), DBL(1)); return 0; }
SH(sqrtf) { FLT(0) = sqrtf(FLT(0)); return 0; }
SH(sinf) { FLT(0) = sinf(FLT(0)); return 0; }
SH(cosf) { FLT(0) = cosf(FLT(0)); return 0; }
SH(powf) { FLT(0) = powf(FLT(0), FLT(1)); return 0; }
SH(floorf) { FLT(0) = floorf(FLT(0)); return 0; }
SH(atan2f) { FLT(0) = atan2f(FLT(0), FLT(1)); return 0; }

/* ---- kernel32 ---------------------------------------------------------------- */
SH(ExitProcess) { fflush(stdout); exit((int)A0); }
SH(GetStdHandle) { int n = (int)A0; return n >= -12 && n <= -10 ? g_std_handles[-n - 10] : (uint64_t)-1; }
SH(SetStdHandle) { int n = (int)A0; if (n < -12 || n > -10) return 0; g_std_handles[-n - 10] = A1; return 1; }
SH(GetACP) { (void)c; return 1252; }
SH(GetConsoleMode) { if (A1) ST(uint32_t, A1, 0); return 1; }
SH(GetProcessHeap) { return (uint64_t)&g_heap; }
/* The Heap* family must hand the guest addresses it can dereference: a host malloc pointer is 48-bit and would be truncated by a PE32 thunk, and
 * HeapSize cannot decode a class byte the guest allocator never wrote. All four share one 16-byte header (size + magic) in low memory. */
#define GHDR 16
#define GMAGIC 0x48454150u
static void *gheap_alloc(size_t n) { char *b = aot_gmalloc(n + GHDR); if (!b) return NULL; ((size_t *)b)[0] = n; ((size_t *)b)[1] = GMAGIC; return b + GHDR; }
SH(HeapAlloc) { void *p = gheap_alloc(A2 ? (size_t)A2 : 1); if (p && (A1 & 8)) memset(p, 0, (size_t)A2); if (getenv("AOT_TRACE_VA")) fprintf(stderr, "[ha] size=%llx -> %p\n", (unsigned long long)A2, p); return (uint64_t)(uintptr_t)p; }
SH(HeapReAlloc) { char *b = A2 ? (char *)(uintptr_t)A2 - GHDR : NULL;
  if (!b) return (uint64_t)(uintptr_t)gheap_alloc(A3 ? (size_t)A3 : 1);
  if (((size_t *)b)[1] != GMAGIC) { aot_last_error = 6; return 0; }   /* not one of ours: never hand back a host pointer */
  size_t old = ((size_t *)b)[0], n = A3 ? (size_t)A3 : 1; char *q = gheap_alloc(n); if (!q) { aot_last_error = 8; return 0; }
  memcpy(q, b + GHDR, old < n ? old : n); aot_gfree(b); return (uint64_t)(uintptr_t)q; }
SH(HeapFree) { char *b = A2 ? (char *)(uintptr_t)A2 - GHDR : NULL; if (b && ((size_t *)b)[1] == GMAGIC) aot_gfree(b); return 1; }
SH(HeapSize) { char *b = A2 ? (char *)(uintptr_t)A2 - GHDR : NULL; if (!b || ((size_t *)b)[1] != GMAGIC) return (uint64_t)-1; return (uint64_t)((size_t *)b)[0]; }
/* VirtualAlloc/VirtualFree with Windows semantics the guest allocators rely on: 64 KB-aligned reservations, a fixed address is honoured or the call
 * fails (never relocated), MEM_RELEASE frees the whole reservation (dwSize must be 0), MEM_DECOMMIT returns zeroed pages on recommit. */
typedef struct { uint64_t raw, rawn, base, n, req; } VRes;   /* req: size the guest asked for (page-rounded) = what Windows reports as RegionSize */
static VRes *vres; static int vres_n, vres_cap; static pthread_mutex_t vres_mu = PTHREAD_MUTEX_INITIALIZER;
static int vres_add(VRes v) {   /* mu held: reuse a released slot, else grow; never records two reservations over the same address */
  for (int i = 0; i < vres_n; i++) if (vres[i].base && v.base < vres[i].base + vres[i].n && vres[i].base < v.base + v.n) {
    static int rep; if (rep++ < 8) fprintf(stderr, "[va] overlapping reservation refused: %#llx+%#llx over %#llx+%#llx\n",
      (unsigned long long)v.base, (unsigned long long)v.n, (unsigned long long)vres[i].base, (unsigned long long)vres[i].n);
    return 0; }
  for (int i = 0; i < vres_n; i++) if (!vres[i].base) { vres[i] = v; return 1; }
  if (vres_n == vres_cap) { int nc = vres_cap ? vres_cap * 2 : 4096; VRes *nv = realloc(vres, (size_t)nc * sizeof *nv); if (!nv) return 0; vres = nv; vres_cap = nc; }
  vres[vres_n++] = v; return 1;
}
static VRes *vres_find(uint64_t a) { for (int i = 0; i < vres_n; i++) if (vres[i].base && a >= vres[i].base && a < vres[i].base + vres[i].n) return &vres[i]; return NULL; }
/* Released reservations stay mapped PROT_NONE ("quarantine") so their addresses are never handed out again behind the allocator's back: a stale free-list node
 * inside memory that VirtualAlloc re-issued is how the game's heap ended up with live blocks overlapping free chunks. Windows would fault on the stale access. */
/* Released space stays reserved PROT_NONE for good. Unmapping it (eviction, or a failed PROT_NONE map) is what puts a hole underneath a chunk
 * the guest allocator has already coalesced into a live block, and the next copy write faults there one page past the surviving mapping. */
#define QMAX 4096
typedef struct { uint64_t a, n; } QEnt; static QEnt qlist[QMAX]; static int qcount;
static int va_quarantine(uint64_t a, uint64_t n) {   /* 0: it could not be kept reserved, so the release must fail rather than leave a hole */
  void *p = mmap((void *)a, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
  if (p != (void *)a) { if (p != MAP_FAILED) munmap(p, n); return 0; }
  if (qcount == QMAX) { static int rep; if (rep++ < 4) fprintf(stderr, "[va] quarantine table full: %#llx+%#llx stays reserved but unreusable\n", (unsigned long long)a, (unsigned long long)n); return 1; }
  qlist[qcount++] = (QEnt){ a, n }; return 1;
}
static int va_quarantine_take(uint64_t b, uint64_t n) {   /* hinted VirtualAlloc over released space: hand it back as read/write, keep the leftovers reserved */
  for (int i = 0; i < qcount; i++) { QEnt old = qlist[i];
    if (b < old.a || b + n > old.a + old.n) continue;
    if (mprotect((void *)b, n, PROT_READ | PROT_WRITE) != 0) return 0;
    madvise((void *)b, n, MADV_DONTNEED);   /* fresh zero pages */
    qlist[i] = qlist[--qcount];
    if (b > old.a && qcount < QMAX) qlist[qcount++] = (QEnt){ old.a, b - old.a };
    if (b + n < old.a + old.n && qcount < QMAX) qlist[qcount++] = (QEnt){ b + n, old.a + old.n - (b + n) };
    return 1; }
  return 0;
}
static void va_trace(const char *what, uint64_t addr, uint64_t size, uint64_t type, uint64_t out) {
  static int n; if (!aot_is_pe32) return;
  if (n >= 700 && (strcmp(what, "alloc") != 0 || size < (256u << 10))) return;   /* keep the per-call budget, but never lose a heap-segment allocation */
  n++;
  fprintf(stderr, "[va] %s addr=%#llx size=%#llx type=%#llx -> %#llx\n", what, (unsigned long long)addr, (unsigned long long)size, (unsigned long long)type, (unsigned long long)out);
}
SH(VirtualAlloc) {
  uint64_t addr = A0, type = A2; size_t sz = (A1 + 0xFFF) & ~0xFFFULL; if (!sz) return 0;
  pthread_mutex_lock(&vres_mu); uint64_t out = 0;
  if (addr) {
    VRes *r = vres_find(addr);
    if (r) { if (addr + sz <= r->base + r->n) out = addr; }                  /* commit inside our reservation: pages are already there (zero or kept) */
    else {
      /* Windows rounds a reservation hint down to the 64 KB granularity and returns that base; record exactly what is returned, so a
         later VirtualFree(returned) succeeds and VirtualQuery reports the right base and RegionSize. */
      uint64_t b = addr & ~0xFFFFULL; size_t n = (sz + (size_t)(addr - b) + 0xFFFF) & ~0xFFFFULL;
      if (va_quarantine_take(b, n)) { if (vres_add((VRes){ b, n, b, n, n })) out = b; }
      else { void *p = mmap((void *)b, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
      if (p == (void *)b && vres_add((VRes){ b, n, b, n, n })) out = b;
      else if (p != MAP_FAILED) munmap(p, n); }                               /* address taken, or out of memory: fail honestly */
    }
  } else {
    size_t n = (sz + 0xFFFF) & ~0xFFFFULL; void *raw = aot_map_anon(n);   /* exact size, 64K-aligned: no hidden slack pages past the region end (allocators probe/merge by adjacency and trust RegionSize) */
    if (raw && ((uint64_t)raw & 0xFFFF)) { munmap(raw, n); raw = aot_map_anon(n + 0x10000); if (raw) { uint64_t b2 = ((uint64_t)raw + 0xFFFF) & ~0xFFFFULL; if (vres_add((VRes){ (uint64_t)raw, n + 0x10000, b2, n, n })) out = b2; else munmap(raw, n + 0x10000); } }
    else if (raw) { if (vres_add((VRes){ (uint64_t)raw, n, (uint64_t)raw, n, n })) out = (uint64_t)raw; else munmap(raw, n); }
  }
  if (!out) { static int nf; if (nf++ < 20) fprintf(stderr, "[aot] VirtualAlloc failed: addr=%llx size=%llx type=%llx (reservations=%d)\n", (unsigned long long)addr, (unsigned long long)A1, (unsigned long long)type, vres_n); }
  if (getenv("AOT_TRACE_VA")) fprintf(stderr, "[va] addr=%llx size=%llx type=%llx -> %llx\n", (unsigned long long)addr, (unsigned long long)A1, (unsigned long long)type, (unsigned long long)out);
  va_trace("alloc", addr, A1, type, out);
  pthread_mutex_unlock(&vres_mu); (void)type; return out; }
int aot_vres_lookup(uint64_t a, uint64_t *base, uint64_t *size) {   /* VirtualQuery support: the VirtualAlloc reservation containing a */
  pthread_mutex_lock(&vres_mu); VRes *r = vres_find(a); int ok = r != NULL; if (ok) { *base = r->base; *size = r->req ? r->req : r->n; } pthread_mutex_unlock(&vres_mu); return ok;
}
int aot_vres_peek(uint64_t a, uint64_t *base, uint64_t *size) {   /* crash-handler safe: never blocks on a lock the faulting thread may hold */
  if (pthread_mutex_trylock(&vres_mu)) return 0;
  VRes *r = vres_find(a); int ok = r != NULL; if (ok) { *base = r->base; *size = r->req ? r->req : r->n; } pthread_mutex_unlock(&vres_mu); return ok;
}
SH(VirtualFree) {
  uint64_t addr = A0, size = A1, type = A2; int ok = 1; pthread_mutex_lock(&vres_mu); VRes *r = vres_find(addr);
  if ((type & 0x8000) && r && r->base == addr) {
    if (va_quarantine(r->raw, r->rawn)) { va_trace("release", addr, r->n, type, 1); memset(r, 0, sizeof *r); }
    else ok = 0;   /* keeping it mapped is mandatory: an unmapped release is the hole a coalesced chunk later writes into */
  }
  else if ((type & 0x4000) && r) { va_trace("decommit", addr, size, type, 1); size_t n = ((size ? size : r->n) + 0xFFF) & ~0xFFFULL; madvise((void *)(addr & ~0xFFFULL), n, MADV_DONTNEED); }   /* anonymous private: pages come back zeroed */
  else if (type & 0x8000) ok = 0;
  pthread_mutex_unlock(&vres_mu); return ok; }
SH(VirtualProtect) { if (A3) ST(uint32_t, A3, 0x40); return 1; }
SH(GetCommandLineA) {
  if (!aot_is_pe32) return (uint64_t)g_cmdline;
  static char *guest; if (!guest) { guest = aot_gmalloc(strlen(g_cmdline) + 1); if (!guest) return 0; strcpy(guest, g_cmdline); }
  return (uint64_t)(uintptr_t)guest;
}
SH(GetCurrentProcess) { return (uint64_t)-1; }
SH(GetCurrentThread) { return (uint64_t)-2; }
SH(GetCurrentProcessId) { return (uint64_t)getpid(); }
SH(GetCurrentThreadId) { return 1; }
SH(GetTickCount) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
SH(QueryPerformanceCounter) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); ST(uint64_t, A0, (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec); return 1; }
SH(QueryPerformanceFrequency) { ST(uint64_t, A0, 1000000000ULL); return 1; }
SH(GetSystemTimeAsFileTime) { struct timeval tv; gettimeofday(&tv, 0); ST(uint64_t, A0, ((uint64_t)tv.tv_sec + 11644473600ULL) * 10000000ULL + tv.tv_usec * 10); return 0; }
SH(Sleep) { usleep((useconds_t)A0 * 1000); return 0; }
SH(GetStartupInfoA) { size_t n = aot_is_pe32 ? 68 : 104; if (!A0) return 0; memset((void *)A0, 0, n); ST(uint32_t, A0, (uint32_t)n); return 0; }
SH(GetEnvironmentStringsW) {
  if (g_env_w) return (uint64_t)(uintptr_t)g_env_w;
  size_t units = 1; for (char **e = environ; e && *e; e++) units += strlen(*e) + 1;
  uint16_t *block = aot_gcalloc(units, 2), *out = block; if (!block) { aot_last_error = 8; return 0; }
  for (char **e = environ; e && *e; e++) { const unsigned char *s = (const unsigned char *)*e; while (*s) *out++ = *s++; *out++ = 0; }
  *out = 0; g_env_w = (char *)block; return (uint64_t)(uintptr_t)block;
}
SH(FreeEnvironmentStringsW) { void *p = (void *)(uintptr_t)A0; if (p == g_env_w) g_env_w = NULL; aot_gfree(p); return 1; }
SH(TlsAlloc) { return (uint64_t)__atomic_fetch_add(&g_tls_n, 1, __ATOMIC_RELAXED); }
SH(TlsGetValue) { return A0 < 1088 ? g_tls[A0] : 0; }
SH(TlsSetValue) { if (A0 < 1088) g_tls[A0] = A1; return 1; }
SH(FlsAlloc) { return (uint64_t)__atomic_fetch_add(&g_tls_n, 1, __ATOMIC_RELAXED); }
static int copy_module_name_w(char *out, size_t cap, const uint16_t *wide) {
  if (!wide || !cap) return 0;
  size_t i = 0;
  for (; i + 1 < cap && wide[i]; i++) {
    if (wide[i] > 0x7f) return 0;
    out[i] = (char)wide[i];
  }
  if (wide[i]) return 0;
  out[i] = 0;
  return 1;
}
SH(LoadLibraryA) { const char *name = A0 ? STR(A0) : "(null)"; uint64_t h = aot_module_load(A0 ? STR(A0) : 0); if (!h) aot_last_error = 126; if (getenv("AOT_VERBOSE")) fprintf(stderr, "[aot] dynamic LoadLibraryA %s: %s\n", h ? "resolved" : "unresolved", name); return h; }
SH(LoadLibraryW) { char dll[260]; if (!copy_module_name_w(dll, sizeof dll, (const uint16_t *)(uintptr_t)A0)) { aot_last_error = 126; if (getenv("AOT_VERBOSE")) fprintf(stderr, "[aot] dynamic LoadLibraryW unresolved: invalid wide module name\n"); return 0; } uint64_t h = aot_module_load(dll); if (!h) aot_last_error = 126; if (getenv("AOT_VERBOSE")) fprintf(stderr, "[aot] dynamic LoadLibraryW %s: %s\n", h ? "resolved" : "unresolved", dll); return h; }
SH(LoadLibraryExA) { const char *name = A0 ? STR(A0) : "(null)"; uint64_t h = aot_module_load(A0 ? STR(A0) : 0); if (!h) aot_last_error = 126; if (getenv("AOT_VERBOSE")) fprintf(stderr, "[aot] dynamic LoadLibraryExA %s: %s\n", h ? "resolved" : "unresolved", name); return h; }
SH(LoadLibraryExW) { char dll[260]; if (!copy_module_name_w(dll, sizeof dll, (const uint16_t *)(uintptr_t)A0)) { aot_last_error = 126; if (getenv("AOT_VERBOSE")) fprintf(stderr, "[aot] dynamic LoadLibraryExW unresolved: invalid wide module name\n"); return 0; } uint64_t h = aot_module_load(dll); if (!h) aot_last_error = 126; if (getenv("AOT_VERBOSE")) fprintf(stderr, "[aot] dynamic LoadLibraryExW %s: %s\n", h ? "resolved" : "unresolved", dll); return h; }
SH(GetModuleHandleA) { return A0 ? aot_module_get_handle(STR(A0)) : g_image_base; }
SH(GetModuleHandleW) { char dll[260]; if (!A0) return g_image_base; return copy_module_name_w(dll, sizeof dll, (const uint16_t *)(uintptr_t)A0) ? aot_module_get_handle(dll) : 0; }
SH(GetProcAddress) { uint64_t p = aot_module_get_proc(A0, A1); if (!p) aot_last_error = 127; return p; }
SH(GetSystemInfo) {
  if (!A0) return 0; size_t n = aot_is_pe32 ? 36 : 48; memset((void *)A0, 0, n);
  long cpus = sysconf(_SC_NPROCESSORS_ONLN); if (cpus < 1) cpus = 1;
  ST(uint16_t, A0, aot_is_pe32 ? 0 : 9); ST(uint32_t, A0 + 4, 4096);
  if (aot_is_pe32) {
    ST(uint32_t, A0 + 8, 0x10000); ST(uint32_t, A0 + 12, 0x7ffeffffu); ST(uint32_t, A0 + 16, 0xff);
    ST(uint32_t, A0 + 20, (uint32_t)cpus); ST(uint32_t, A0 + 24, 386); ST(uint32_t, A0 + 28, 0x10000);
  } else {
    ST(uint64_t, A0 + 8, 0x10000); ST(uint64_t, A0 + 16, 0x7ffffffeffffULL); ST(uint64_t, A0 + 24, 0xff);
    ST(uint32_t, A0 + 32, (uint32_t)cpus); ST(uint32_t, A0 + 36, 8664); ST(uint32_t, A0 + 40, 0x10000);
  } return 0;
}
SH(MultiByteToWideChar) {
  if (!A2 || !(int)A3 || (int)A3 < -1 || (ARG(5) && !ARG(4))) { aot_last_error = 87; return 0; }
  int n = (int)A3 < 0 ? (int)strlen(STR(A2)) + 1 : (int)A3;
  if (!ARG(5)) return n; if ((int)ARG(5) < n) { aot_last_error = 122; return 0; }
  uint16_t *o = (uint16_t *)(uintptr_t)ARG(4); for (int i = 0; i < n; i++) o[i] = (unsigned char)STR(A2)[i]; return n;
}

#define S(n) {#n, sh_##n}
#define SA(nm, fn) {nm, sh_##fn}
const ShimEnt aot_shim_table_crt[] = {
  S(printf), S(fprintf), S(sprintf), S(snprintf), S(vprintf), S(puts), S(putchar), S(fputs), S(fwrite), S(fflush),
  SA("_snprintf", snprintf), SA("__stdio_common_vfprintf", vfprintf_ucrt), SA("__stdio_common_vsprintf", vsprintf_ucrt), SA("__stdio_common_vsnprintf_s", vsprintf_ucrt),
  SA("__acrt_iob_func", iob_func), SA("__iob_func", iob_array), SA("_vfprintf", vfprintf_ucrt),
  S(malloc), S(calloc), S(realloc), S(free), S(memcpy), S(memmove), S(memset), S(memcmp), S(strlen), S(strcmp), S(strncmp), S(strcpy), S(strncpy), S(strcat), S(strchr), S(strstr),
  S(atoi), S(rand), S(srand), S(abs), S(exit), S(abort), SA("_exit", exit), SA("_cexit", zero), SA("_Exit", exit),
  SA("_initterm", initterm), SA("_initterm_e", initterm_e), SA("__getmainargs", getmainargs), SA("__p___argc", p_argc), SA("__p___argv", p_argv), SA("__p__fmode", p_fmode), SA("__p__commode", p_fmode),
  SA("_get_initial_narrow_environment", get_narrow_env), SA("__p__acmdln", acmdln), SA("_acmdln", acmdln),
  S(sqrt), S(sin), S(cos), S(tan), S(atan), S(atan2), S(exp), S(log), S(pow), S(floor), S(ceil), S(fabs), S(fmod), S(sqrtf), S(sinf), S(cosf), S(powf), S(floorf), S(atan2f),
  SA("__set_app_type", zero), SA("_set_app_type", zero), SA("_setmode", zero), SA("signal", zero), SA("atexit", zero), SA("_onexit", zero), SA("_crt_atexit", zero), SA("__setusermatherr", zero),
  SA("_set_invalid_parameter_handler", zero), SA("_controlfp_s", zero), SA("_configthreadlocale", zero), SA("_set_new_mode", zero), SA("_set_fmode", zero), SA("_configure_narrow_argv", zero),
  SA("_initialize_narrow_environment", zero), SA("_register_onexit_function", zero), SA("_initialize_onexit_table", zero), SA("_seh_filter_exe", zero),
  SA("_amsg_exit", exit), SA("_XcptFilter", zero), SA("_fpreset", zero), SA("__mingw_init_ehandler", zero), SA("_lock", zero), SA("_unlock", zero), SA("__lock", zero), SA("__unlock", zero),
  {0, 0}
};
const ShimEnt aot_shim_table_win32[] = {
  S(ExitProcess), S(GetStdHandle), S(GetConsoleMode), S(GetProcessHeap), S(HeapAlloc), S(HeapReAlloc), S(HeapFree), S(HeapSize),
  S(VirtualAlloc), S(VirtualFree), S(VirtualProtect), S(GetModuleHandleA), S(GetModuleHandleW), S(GetCommandLineA), S(SetStdHandle), S(GetACP),
  S(GetCurrentProcess), S(GetCurrentThread), S(GetCurrentProcessId), S(GetCurrentThreadId), S(GetTickCount), SA("GetTickCount64", GetTickCount), S(QueryPerformanceCounter), S(QueryPerformanceFrequency),
  S(GetSystemTimeAsFileTime), S(Sleep), S(GetStartupInfoA), SA("GetStartupInfoW", GetStartupInfoA), S(GetEnvironmentStringsW), S(FreeEnvironmentStringsW), S(TlsAlloc), S(TlsGetValue), S(TlsSetValue),
  S(FlsAlloc), SA("FlsGetValue", TlsGetValue), SA("FlsSetValue", TlsSetValue), SA("FlsFree", one), SA("TlsFree", one), S(LoadLibraryA), S(LoadLibraryW), S(LoadLibraryExA), S(LoadLibraryExW), S(GetProcAddress), S(GetSystemInfo), S(MultiByteToWideChar),
  SA("SetUnhandledExceptionFilter", zero), SA("UnhandledExceptionFilter", zero), SA("IsDebuggerPresent", zero), SA("IsProcessorFeaturePresent", zero), SA("InitializeCriticalSection", zero),
  SA("InitializeCriticalSectionAndSpinCount", one), SA("InitializeCriticalSectionEx", one), SA("EnterCriticalSection", zero), SA("LeaveCriticalSection", zero), SA("DeleteCriticalSection", zero),
  SA("TryEnterCriticalSection", one), SA("SetConsoleCtrlHandler", one), SA("SetConsoleOutputCP", one), SA("SetErrorMode", zero),
  {0, 0}
};
const ShimEnt aot_shim_table_ntdll[] = {
  SA("__C_specific_handler", zero), SA("RtlCaptureContext", zero),
  SA("RtlLookupFunctionEntry", zero), SA("RtlVirtualUnwind", zero),
  {0, 0}
};
int aot_shim_is_placeholder(shim_fn fn) {
  return fn == sh_zero || fn == sh_one;
}
