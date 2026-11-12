/* PE image mapper + import binder + dispatch trampoline for recompiled code. */
#define _GNU_SOURCE
#include "aot_runtime.h"
#include "shims.h"
#include <stdarg.h>
#include <sys/mman.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#define THUNK_BASE (aot_is_pe32 ? 0x6ff00000ULL : 0x6fff00000000ULL)
#define THUNK_SZ 16
#define EXIT_IDX 0
#define RET_IDX 1
#define MAXTHUNK 8192

typedef struct { shim_fn fn; char name[160]; int pop, fp; } Thunk;
static Thunk thunks[MAXTHUNK]; static int nthunks = 2;
static uint64_t g_stack_top;
const char *g_exe_path; char g_cmdline[1024] = "game.exe";
uint64_t g_image_base;
__thread uint64_t aot_last_pc;
uint64_t g_peb_addr, g_tls_s0; size_t g_tls_n, g_tls_total;
__attribute__((weak)) const uint64_t aot_image_hash = 0;

static uint16_t u16(const uint8_t *p, size_t o) { uint16_t v; memcpy(&v, p + o, 2); return v; }
static uint32_t u32(const uint8_t *p, size_t o) { uint32_t v; memcpy(&v, p + o, 4); return v; }
static uint64_t u64(const uint8_t *p, size_t o) { uint64_t v; memcpy(&v, p + o, 8); return v; }

static uint64_t image_fingerprint(const uint8_t *image, size_t size) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < size; i++) {
    hash ^= image[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

uint64_t aot_rdtsc(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 3000000000ULL + (uint64_t)t.tv_nsec * 3; }

void aot_unimpl(uint64_t addr, const char *text) {
  fflush(stdout);
  fprintf(stderr, "\n[aot] untranslated/trapping instruction at 0x%llx: %s\n", (unsigned long long)addr, text);
  exit(70);
}

void aot_thunk_set_pop(uint64_t addr, int pop) {   /* PE32: bytes the callee pops (stdcall COM methods) */
  uint64_t i = (addr - THUNK_BASE) / THUNK_SZ; if (i < (uint64_t)nthunks) thunks[i].pop = pop;
}

void aot_thunk_set_fp(uint64_t addr, int fp) {
  uint64_t i = (addr - THUNK_BASE) / THUNK_SZ; if (i < (uint64_t)nthunks) thunks[i].fp = fp;
}

uint64_t aot_register_thunk(aot_host_fn fn, const char *name) {
  if (nthunks >= MAXTHUNK) return 0;
  thunks[nthunks].fn = (shim_fn)fn;
  snprintf(thunks[nthunks].name, sizeof thunks[nthunks].name, "%s", name ? name : "host method");
  return THUNK_BASE + (uint64_t)nthunks++ * THUNK_SZ;
}

static void bind_thunks(uint8_t *img, const char *dll, uint32_t oft,
                        uint32_t ft, int verbose) {
  const int ps = aot_is_pe32 ? 4 : 8;
  for (int k = 0;; k++) {
    uint64_t e = aot_is_pe32 ? u32(img, oft + 4 * k) : u64(img, oft + 8 * k);
    if (!e) break;
    char nm[96];
    if (e >> (8 * ps - 1))
      snprintf(nm, sizeof nm, "#%u", (unsigned)(e & 0xFFFF));
    else
      snprintf(nm, sizeof nm, "%.80s",
               (const char *)img + (e & 0x7FFFFFFF) + 2);
    if (aot_is_pe32) { char *at = strrchr(nm, '@'); if (at && at[1] && strspn(at + 1, "0123456789") == strlen(at + 1)) *at = 0; }   /* tolerate stdcall-decorated import names */
    if (nthunks >= MAXTHUNK) {
      fprintf(stderr, "[aot] too many imports\n");
      exit(71);
    }
    Thunk *t = &thunks[nthunks];
    t->pop = t->fp = 0;
    t->fn = aot_is_pe32 ? aot_x86_resolve(dll, nm, &t->pop, &t->fp) : aot_resolve_import(dll, nm);
    snprintf(t->name, sizeof t->name, "%.72s!%.80s", dll, nm);
    if (!t->fn && verbose)
      fprintf(stderr, "[aot] missing import %s\n", t->name);
    else if (t->fn && verbose && aot_import_is_placeholder(dll, nm))
      fprintf(stderr, "[aot] import uses placeholder shim %s\n", t->name);
    uint64_t address = THUNK_BASE + (uint64_t)nthunks * THUNK_SZ;
    nthunks++;
    memcpy(img + ft + ps * k, &address, ps);
  }
}

static void bind_imports(uint8_t *img, uint32_t rva, int verbose) {
  if (!rva) return;
  for (const uint8_t *d = img + rva; u32(d, 12); d += 20) {
    const char *dll = (const char *)img + u32(d, 12);
    uint32_t oft = u32(d, 0) ? u32(d, 0) : u32(d, 16), ft = u32(d, 16);
    (void)aot_module_load(dll);
    bind_thunks(img, dll, oft, ft, verbose);
  }
}

static uint32_t delay_field_rva(uint32_t value, uint32_t attrs,
                                uint64_t image_base, uint32_t image_size) {
  if (!value) return 0;
  uint64_t rva = (attrs & 1) ? value :
      ((uint64_t)value >= image_base ? (uint64_t)value - image_base : UINT64_MAX);
  return rva < image_size ? (uint32_t)rva : 0;
}

static void bind_delay_imports(uint8_t *img, uint32_t rva, uint32_t size,
                               uint64_t image_base, uint32_t image_size,
                               int verbose) {
  if (!rva || !size) return;
  for (uint32_t offset = 0; offset + 32 <= size; offset += 32) {
    const uint8_t *descriptor = img + rva + offset;
    uint32_t attrs = u32(descriptor, 0);
    uint32_t name_rva = delay_field_rva(u32(descriptor, 4), attrs,
                                        image_base, image_size);
    uint32_t iat_rva = delay_field_rva(u32(descriptor, 12), attrs,
                                       image_base, image_size);
    uint32_t int_rva = delay_field_rva(u32(descriptor, 16), attrs,
                                       image_base, image_size);
    if (!attrs && !u32(descriptor, 4) && !u32(descriptor, 8) &&
        !u32(descriptor, 12) && !u32(descriptor, 16) &&
        !u32(descriptor, 20) && !u32(descriptor, 24) &&
        !u32(descriptor, 28))
      break;
    if (!name_rva || !iat_rva) {
      if (verbose)
        fprintf(stderr, "[aot] invalid delay-import descriptor at RVA 0x%x\n",
                rva + offset);
      continue;
    }
    const char *dll = (const char *)img + name_rva;
    (void)aot_module_load(dll);
    bind_thunks(img, dll, int_rva ? int_rva : iat_rva, iat_rva, verbose);
  }
}

/* last imports the guest called (for crash reports) */
static __thread int recent[32]; static __thread unsigned recent_n; static __thread uint64_t recent_pc[16]; static __thread unsigned recent_pc_n;   /* per thread: shared globals were written on every dispatch by all threads (cache-line ping-pong) */   /* last guest region entries */
uint64_t g_image_size;
/* crash report helper (called from the signal handler): recent imports + the original x86 bytes of the block being executed, so the faulting code can be disassembled offline */
static __thread CPU *cur_cpu;   /* guest CPU of this thread, for crash reports */
static void dump_context(CPU *c);
/* File-I/O failures are reported (bounded), never silent: a failed read/seek/lock is exactly what makes a streaming game wait forever. */
static void io_fail_note(const char *name, uint32_t eax) {
  static const char *const bools[] = { "ReadFile", "GetOverlappedResult", "LockFileEx", "UnlockFileEx", "GetFileSizeEx", "SetFilePointerEx", "WriteFile", "SetEndOfFile", NULL };
  static const char *const dwords[] = { "SetFilePointer", "GetFileSize", NULL }; static int cnt[16];
  const char *dot = strchr(name, '!'); const char *n = dot ? dot + 1 : name; int k = 0, hit = 0;
  for (; bools[k]; k++) if (!strcmp(n, bools[k])) { hit = (eax == 0) && !(aot_last_error == 997 && (!strcmp(n, "ReadFile") || !strcmp(n, "WriteFile") || !strcmp(n, "GetOverlappedResult"))); goto done; }   /* ERROR_IO_PENDING (997) is the normal answer of an overlapped read */
  for (int j = 0; dwords[j]; j++, k++) if (!strcmp(n, dwords[j])) { hit = (eax == 0xffffffffu); k += 0; goto done; }
  return;
done:
  if (hit && cnt[k % 16]++ < 10) fprintf(stderr, "[aot] %s failed (eax=%u, last error %u)\n", n, (unsigned)eax, (unsigned)aot_last_error);
}
typedef struct { volatile uint64_t pc; volatile unsigned n; volatile int imp[12]; volatile unsigned in; volatile uint64_t wh, set; volatile uint32_t wms; volatile int waiting; CPU *cpu; } __attribute__((aligned(128))) WdThread;
static WdThread wd_t[16]; static int wd_cnt; static __thread WdThread *wd_me;
static WdThread *wd_self(void);
void aot_wd_wait(uint64_t h, uint32_t ms, int on) { WdThread *w = wd_self(); w->wh = h; w->wms = ms; w->waiting = on; }
void aot_wd_set(uint64_t h) { wd_self()->set = h; }
static WdThread *wd_self(void) { if (!wd_me) { int k = __atomic_fetch_add(&wd_cnt, 1, __ATOMIC_RELAXED); wd_me = &wd_t[k % 16]; } return wd_me; }
/* Watchdog: every 10 s report guest progress (region transitions, imports, recent entries) so a hang/spin is located without a debugger. */
static void *watchdog(void *arg) {
  (void)arg; for (int k = 0; k < 6; k++) {
    static uint64_t hp[16][512]; static uint32_t hc[16][512]; memset(hp, 0, sizeof hp); memset(hc, 0, sizeof hc);
    for (int i = 0; i < 5000; i++) { usleep(2000);   /* 2 ms pc sampler: where do running guest threads spend their time? */
      for (int t = 0; t < wd_cnt && t < 16; t++) { if (wd_t[t].waiting) continue; uint64_t pc = wd_t[t].pc; unsigned h = (unsigned)((pc * 2654435761u) >> 7) & 511; for (int pr = 0; pr < 8; pr++, h = (h + 1) & 511) if (hp[t][h] == pc || !hc[t][h]) { hp[t][h] = pc; hc[t][h]++; break; } } }
    fprintf(stderr, "[aot] watchdog %ds, per guest thread (pc, region entries, last non-sync imports):\n", 10 * (k + 1));
    for (int t = 0; t < wd_cnt && t < 16; t++) {
      WdThread *w = &wd_t[t]; fprintf(stderr, "  T%d pc=%llx n=%u %s h=%llx ms=%x lastSet=%llx:", t, (unsigned long long)w->pc, w->n, w->waiting ? "WAITING" : "running", (unsigned long long)w->wh, w->wms, (unsigned long long)w->set); unsigned in = w->in; int shown = 0;
      for (unsigned i = 0; i < 12 && shown < 5; i++) { int idx = w->imp[(in + 11 - i) % 12]; if (idx > 0 && idx < nthunks) { const char *nm = thunks[idx].name; if (strstr(nm, "CriticalSection") || strstr(nm, "Tls") || strstr(nm, "Fls") || strstr(nm, "QueryPerf")) continue; fprintf(stderr, " %s", nm); shown++; } }
      fprintf(stderr, "\n");
      for (int top = 0; top < 6; top++) { int bi = -1; for (int h = 0; h < 512; h++) if (hc[t][h] && (bi < 0 || hc[t][h] > hc[t][bi])) bi = h; if (bi < 0 || hc[t][bi] < 25) break; fprintf(stderr, "    hot T%d pc=%llx samples=%u\n", t, (unsigned long long)hp[t][bi], hc[t][bi]); hc[t][bi] = 0; }
      if (!wd_t[t].waiting && wd_t[t].cpu) { CPU *cc = wd_t[t].cpu; fprintf(stderr, "    regs T%d eax=%llx ecx=%llx edx=%llx ebx=%llx esp=%llx ebp=%llx esi=%llx edi=%llx ftop=%d fsw=%04x st0=%g st1=%g st2=%g st3=%g\n", t,
        (unsigned long long)cc->r[0], (unsigned long long)cc->r[1], (unsigned long long)cc->r[2], (unsigned long long)cc->r[3], (unsigned long long)cc->r[4], (unsigned long long)cc->r[5], (unsigned long long)cc->r[6], (unsigned long long)cc->r[7],
        cc->ftop, cc->fsw, (double)cc->fst[cc->ftop & 7], (double)cc->fst[(cc->ftop + 1) & 7], (double)cc->fst[(cc->ftop + 2) & 7], (double)cc->fst[(cc->ftop + 3) & 7]); } }
  } return NULL;
}
void aot_dump_crash_context(void) {
  if (cur_cpu) dump_context(cur_cpu);
  dprintf(2, "[aot] last imports called (oldest first):\n");
  for (unsigned i = 0; i < 32; i++) { int idx = recent[(recent_n + i) % 32]; if (idx > 0 && idx < nthunks) dprintf(2, "  %s\n", thunks[idx].name); }
  if (g_image_base && aot_last_pc >= g_image_base && aot_last_pc + 160 <= g_image_base + g_image_size) {
    dprintf(2, "[aot] code at 0x%llx:", (unsigned long long)aot_last_pc);
    for (int i = 0; i < 160; i++) dprintf(2, "%s%02x", i % 32 ? "" : "\n  ", ((const uint8_t *)(uintptr_t)aot_last_pc)[i]);
    dprintf(2, "\n");
  }
}
static void dump_code(uint64_t pc, int before, int after) {
  if (!g_image_base || pc < g_image_base + (uint64_t)before || pc + after > g_image_base + g_image_size) return;
  fprintf(stderr, "[aot] image bytes 0x%llx..0x%llx (target at +%d):", (unsigned long long)(pc - before), (unsigned long long)(pc + after), before);
  for (int i = -before; i < after; i++) fprintf(stderr, "%s%02x", (i + before) % 32 ? "" : "\n  ", ((const uint8_t *)(uintptr_t)pc)[i]);
  fprintf(stderr, "\n");
}
/* crash helper: if the faulting block directly follows `[mov ecx,[abs]] [push imm32] call rel32` (a lookup that returned NULL), dump the callee, the pushed string and the object ecx came from */
static int in_img(uint64_t a, size_t n) { return g_image_base && a >= g_image_base && a + n <= g_image_base + g_image_size; }
static void dump_hex(const char *tag, uint64_t a, size_t n) {
  if (!in_img(a, n)) return;
  fprintf(stderr, "[aot] %s 0x%llx:", tag, (unsigned long long)a);
  for (size_t i = 0; i < n; i++) fprintf(stderr, "%s%02x", i % 32 ? "" : "\n  ", ((const uint8_t *)(uintptr_t)a)[i]);
  fprintf(stderr, "\n");
}
static int safe_read(uint64_t a, void *buf, size_t n) {   /* readable-memory probe: write() to a pipe fails with EFAULT instead of faulting */
  int fd[2]; if (pipe(fd)) return 0; int ok = write(fd[1], (void *)(uintptr_t)a, n) == (ssize_t)n && read(fd[0], buf, n) == (ssize_t)n; close(fd[0]); close(fd[1]); return ok;
}
static void dump_heap(const char *tag, uint32_t a, size_t n) {
  uint8_t b[256]; if (n > sizeof b || !safe_read(a, b, n)) return;
  fprintf(stderr, "[aot] %s 0x%x:", tag, a); for (size_t i = 0; i < n; i++) fprintf(stderr, "%s%02x", i % 32 ? "" : "\n  ", b[i]); fprintf(stderr, "\n");
}
static void dump_call_site(void) {
  uint64_t p = aot_last_pc; if (!aot_is_pe32 || !in_img(p - 32, 32)) return;
  const uint8_t *q = (const uint8_t *)(uintptr_t)p;
  if (q[-5] != 0xE8) return;
  int32_t rel; memcpy(&rel, q - 4, 4); uint32_t callee = (uint32_t)(p + rel);
  dump_hex("call target", callee, 96);
  { const uint8_t *cb = (const uint8_t *)(uintptr_t)callee;   /* the callee's own global object (first `mov ecx,[abs]`) */
    for (int k = 0; k < 24; k++) if (cb[k] == 0x8B && cb[k + 1] == 0x0D) { uint32_t g, obj; memcpy(&g, cb + k + 2, 4); if (in_img(g, 4)) { memcpy(&obj, (void *)(uintptr_t)g, 4); fprintf(stderr, "[aot] callee global [0x%x] = 0x%x\n", g, obj); dump_heap("callee object", obj, 192); } break; } }
  for (int k = 6; k <= 20; k++) {
    uint32_t v; memcpy(&v, q - k + 1, 4);
    if (q[-k] == 0x68 && in_img(v, 64)) { dump_hex("pushed arg (string?)", v, 64); break; }
  }
  for (int k = 7; k <= 24; k++) {
    uint32_t v; if (q[-k] == 0x8B && q[-k + 1] == 0x0D) { memcpy(&v, q - k + 2, 4);
      if (in_img(v, 4)) { uint32_t obj; memcpy(&obj, (void *)(uintptr_t)v, 4); fprintf(stderr, "[aot] ecx global [0x%x] = 0x%x\n", v, obj); if (in_img(obj, 64)) dump_hex("object", obj, 64); break; } }
  }
}
static void dump_context(CPU *c) {
  fprintf(stderr, "[aot] last region entries (oldest first):");
  for (unsigned i = 0; i < 16; i++) { uint64_t v = recent_pc[(recent_pc_n + i) % 16]; if (v) fprintf(stderr, " %llx", (unsigned long long)v); }
  fprintf(stderr, "\n");
  if (aot_is_pe32) for (unsigned i = 2; i <= 4; i++) { uint64_t v = recent_pc[(recent_pc_n + 16 - i) % 16]; if (v && in_img(v - 16, 144)) dump_hex("caller code (entry-16)", (uint32_t)(v - 16), 144); }
  dump_code(aot_last_pc, 64, 96);
  fprintf(stderr, "[aot] last guest region entry: 0x%llx\n[aot] last imports called (oldest first):\n", (unsigned long long)aot_last_pc);
  for (unsigned i = 0; i < 32; i++) { int idx = recent[(recent_n + i) % 32]; if (idx > 0 && idx < nthunks) fprintf(stderr, "  %s\n", thunks[idx].name); }
  dump_call_site();
  if (aot_is_pe32 && c) { uint32_t sp = (uint32_t)c->r[4]; fprintf(stderr, "[aot] eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n[aot] stack:", (unsigned)c->r[0], (unsigned)c->r[1], (unsigned)c->r[2], (unsigned)c->r[3], sp, (unsigned)c->r[5], (unsigned)c->r[6], (unsigned)c->r[7]);
    /* Probe, never assume: when esp itself is the faulting address, a raw load here kills the dump and the log ends with no post-mortem. */
    uint32_t w[8];
    if (sp && safe_read(sp, w, sizeof w)) { for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", w[i]); }
    else { uint64_t b = 0, s = 0; int have = aot_vres_peek(sp, &b, &s);
      fprintf(stderr, " <unreadable: esp %#x %s %#llx+%#llx>", sp, have ? "in region" : "in no reservation,", (unsigned long long)b, (unsigned long long)s); }
    fprintf(stderr, "\n"); }
}

/* ---- allocator-corruption diagnostics (game dlmalloc object at 0x199dd6c, tree bins at 0x199dec8) ---- */
__attribute__((noinline)) int aot_stguard(uint64_t addr, uint64_t val, unsigned size) {
  static int n;
  if (!aot_is_pe32) return 1;
  uint32_t v = (uint32_t)val;
  int bad = size == 4 && v && v < 0x10000u;
  if (!bad && n > 8) return 1;
  if (n++ < 40) {
    uintptr_t ret = (uintptr_t)__builtin_return_address(0); int k = -1; uintptr_t best = 0;
    for (int m = 0; m < aot_nregions; m++) { uintptr_t f = (uintptr_t)aot_region_fn[m]; if (f <= ret && f > best) { best = f; k = m; } }
    fprintf(stderr, "[heap] %s store size=%u val=%#llx to tree-bin slot %#llx (bin %d); writer region #%d guest %#llx..%#llx +%#lx; last entry %#llx\n",
        bad ? "BAD (dropped)" : "ok", size, (unsigned long long)val, (unsigned long long)addr, (int)((addr - 0x199dec8) / 4), k,
        k >= 0 ? (unsigned long long)aot_region_lo[k] : 0ull, k >= 0 ? (unsigned long long)aot_region_hi[k] : 0ull, (unsigned long)(ret - best), (unsigned long long)aot_last_pc);
    if (bad && n < 12) dump_context(cur_cpu);
  }
  return 1;   /* report only: dropping the store rewrites the guest allocator's state and hides the real fault */
}
/* Called when the guest takes/releases the allocator lock: a tree-bin slot holding a small non-pointer is the
   exact state that later crashes tmalloc_large.  Report who ran before it and repair the slot. */
uint8_t aot_nodemap[8192];
static uint32_t hc_nodes[8192]; static volatile unsigned hc_nn; static __thread int hc_in_lock;
static __thread uint32_t hc_tmp[8192]; static __thread unsigned hc_tmp_n;
/* last stores that landed on a live free-tree node (the chunk header writer, not the victim) */
static struct { uint32_t node, addr, val; } hwr[16]; static volatile unsigned hwn;
/* The host no longer unlinks nodes from the guest's free trees: a diagnostic must not edit the structure it is measuring. */
__attribute__((noinline)) int aot_nodehit(uint64_t addr, uint64_t val, unsigned size) {
  static int n;
  if (hc_in_lock || !aot_is_pe32) return 1;
  unsigned cnt = hc_nn; uint32_t hit = 0;
  for (unsigned i = 0; i < cnt && i < 8192; i++) { uint32_t nd = hc_nodes[i]; if (addr + size > nd + 4 && addr < nd + 0x1c) { hit = nd; break; } }
  if (!hit) return 1;
  { unsigned q = __atomic_fetch_add(&hwn, 1, __ATOMIC_RELAXED) & 15; hwr[q].node = hit; hwr[q].addr = (uint32_t)addr; hwr[q].val = (uint32_t)val; }
  if (n++ < 30) {
    uintptr_t ret = (uintptr_t)__builtin_return_address(0); int k = -1; uintptr_t best = 0;
    for (int m = 0; m < aot_nregions; m++) { uintptr_t f = (uintptr_t)aot_region_fn[m]; if (f <= ret && f > best) { best = f; k = m; } }
    fprintf(stderr, "[heap] WRITE INTO FREE NODE (dropped): addr %#llx (node %#x +%#x) size %u val %#llx; writer region #%d guest %#llx..%#llx +%#lx; last entry %#llx\n",
        (unsigned long long)addr, hit, (unsigned)(addr - hit), size, (unsigned long long)val, k, k >= 0 ? (unsigned long long)aot_region_lo[k] : 0ull,
        k >= 0 ? (unsigned long long)aot_region_hi[k] : 0ull, (unsigned long)(ret - best), (unsigned long long)aot_last_pc);
    if (n <= 3) dump_context(cur_cpu);
  }
  /* Report only. A live block written over a node that is still in the free trees means the allocator coalesced that memory into the block
     without unlinking the node; repairing it here would change the guest's allocator while it holds its own lock, and the next validation
     would then call the edited tree clean. */
  return 1;
}
static int heap_ptr_ok(uint32_t p) { return !p || (!(p & 7) && ((p >= 0x400000u && p < 0x2a00000u) || (p >= 0x10000000u && p < 0xF0000000u))); }
/* A free chunk is only usable if every byte of it lies inside one live VirtualAlloc reservation. The free trees can be perfectly consistent
   while a chunk reaches across a released region: that is the memcpy that faults one 64 KB page into a 400 KB copy. */
static int heap_chunk_mapped(uint32_t a, uint32_t n) {
  uint64_t base = 0, size = 0;
  if (!n || a + (uint64_t)n > 0x100000000ull) return 0;
  return aot_vres_peek(a, &base, &size) && (uint64_t)a + n <= base + size;
}
static __thread uint64_t hc_last_leave_pc; static __thread unsigned hc_n;
static volatile int hc_reports; static volatile uint64_t hc_checks, hc_nodes_max;
/* Walk all free trees of the game allocator. Returns the number of problems found; crash=1 reports everything with parent dumps and no repair. */
static int heap_validate(const char *where, int crash) {
  static __thread uint32_t st[4096][2]; int problems = 0; uint64_t total = 0;
  uint32_t map = LD(uint32_t, 0x199dda0u); hc_tmp_n = 0;
  for (int bin = 0; bin < 32; bin++) {
    uint32_t root = LD(uint32_t, 0x199dec8u + 4 * bin); int sp = 0;
    if (crash && (!!root != !!(map & (1u << bin)))) fprintf(stderr, "[heap] treemap bit %d = %d but slot = %#x\n", bin, !!(map & (1u << bin)), root);
    if (!root) continue;
    st[sp][0] = root; st[sp++][1] = 0x199dec8u + 4 * bin; unsigned visited = 0;
    while (sp > 0 && visited++ < 20000) {
      uint32_t n = st[--sp][0], from = st[sp][1]; int bad = 0; const char *what = ""; total++;
      uint32_t ch[2] = {0, 0}, par = 0, head = 0;
      if (!n || !heap_ptr_ok(n)) { bad = 1; what = "node pointer"; }
      else { head = LD(uint32_t, n + 4); ch[0] = LD(uint32_t, n + 0x10); ch[1] = LD(uint32_t, n + 0x14); par = LD(uint32_t, n + 0x18);
        if (!heap_ptr_ok(ch[0]) || !heap_ptr_ok(ch[1])) { bad = 2; what = "child link"; }
        else if (!(head & ~3u) || (head & ~3u) > 0x40000000u) { bad = 3; what = "chunk size"; }
        else if (crash && !heap_chunk_mapped(n, head & ~3u)) { bad = 4; what = "chunk reaches past its reservation"; } }
      if (bad) {
        problems++;
        if (hc_reports++ < 12 || crash) {
          fprintf(stderr, "[heap] TREE CORRUPT at %s: bin %d, %s (node %#x head %#x children %#x %#x parent %#x) reached from %#x; op %u, last leave pc %#llx, last entry %#llx\n",
              where, bin, what, n, head, ch[0], ch[1], par, from, hc_n, (unsigned long long)hc_last_leave_pc, (unsigned long long)aot_last_pc);
          if (bad == 4) { uint64_t b = 0, s = 0; int have = aot_vres_peek(n, &b, &s);
            fprintf(stderr, "[heap]   chunk %#x size %#x: %s %#llx+%#llx\n", n, head & ~3u, have ? "region" : "no region at", (unsigned long long)b, (unsigned long long)s); }
          if (n && heap_ptr_ok(n)) dump_hex("bad node", n, 64);
          if (from > 0x199df48u && heap_ptr_ok(from)) dump_hex("referrer", from, 64);
          if (!crash) dump_context(cur_cpu);
        }
        if (!crash) {
          if (bad == 2) { if (!heap_ptr_ok(ch[0])) ST(uint32_t, n + 0x10, 0); if (!heap_ptr_ok(ch[1])) ST(uint32_t, n + 0x14, 0); }
          else if (from >= 0x199dec8u && from < 0x199df48u) ST(uint32_t, from, 0);
          else if (heap_ptr_ok(from)) { if (LD(uint32_t, from + 0x10) == n) ST(uint32_t, from + 0x10, 0); if (LD(uint32_t, from + 0x14) == n) ST(uint32_t, from + 0x14, 0); }
          return problems; }
        continue;
      }
      if (hc_tmp_n < 8192) hc_tmp[hc_tmp_n++] = n;
      if (sp + 2 < 4096) { if (ch[0]) { st[sp][0] = ch[0]; st[sp++][1] = n; } if (ch[1]) { st[sp][0] = ch[1]; st[sp++][1] = n; } }
    }
  }
  if (total > hc_nodes_max) hc_nodes_max = total;
  return problems;
}
static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
/* Two free chunks that overlap are what later hands one memory region to two owners. Find the operation that creates the overlap. */
static void heap_overlap_check(const char *where) {
  static int rep; if (rep >= 16 || hc_tmp_n < 2) return;
  static __thread uint32_t s[8192]; unsigned n = hc_tmp_n; memcpy(s, hc_tmp, n * 4); qsort(s, n, 4, cmp_u32);
  for (unsigned i = 0; i + 1 < n; i++) {
    uint32_t a = s[i], b = s[i + 1], sa = LD(uint32_t, a + 4) & ~3u;
    if (a + sa > b) {
      rep++;
      fprintf(stderr, "[heap] OVERLAPPING FREE CHUNKS at %s: %#x (size %#x, ends %#x) and %#x (size %#x); op %u, last leave pc %#llx, last entry %#llx\n",
          where, a, sa, a + sa, b, LD(uint32_t, b + 4) & ~3u, hc_n, (unsigned long long)hc_last_leave_pc, (unsigned long long)aot_last_pc);
      fprintf(stderr, "[heap]   words at a: "); for (int k = 0; k < 8; k++) fprintf(stderr, "%x ", LD(uint32_t, a + 4 * k));
      fprintf(stderr, "\n[heap]   words at b-8: "); for (int k = 0; k < 8; k++) fprintf(stderr, "%x ", LD(uint32_t, b - 8 + 4 * k)); fprintf(stderr, "\n");
      { unsigned q = __atomic_load_n(&hwn, __ATOMIC_RELAXED); if (q > 16) q = 16; fprintf(stderr, "[heap]   last node stores (newest first):");
        for (unsigned k = 0; k < q; k++) { unsigned j = (q - 1 - k) & 15; fprintf(stderr, " [node %#x <- %#x at %#x]", hwr[j].node, hwr[j].val, hwr[j].addr); } fprintf(stderr, "\n"); }
      dump_context(cur_cpu); return; }
  }
}
void aot_heap_check(const char *where) {
  if (!aot_is_pe32 || !in_img(0x199dec8u, 0x80)) return;
  uint64_t k = __atomic_add_fetch(&hc_checks, 1, __ATOMIC_RELAXED);
  if (k == 1000 || k == 100000 || k == 1000000) fprintf(stderr, "[heap] validator alive: %llu checks, largest tree set so far %llu nodes\n", (unsigned long long)k, (unsigned long long)hc_nodes_max);
  int bad = heap_validate(where, 0);
  if (!bad) heap_overlap_check(where);
  hc_n++;
  if (where[3] == 'l') {   /* leave: publish the verified node set, then the lock is gone */
    hc_last_leave_pc = aot_last_pc;
    if (!bad) { memset(aot_nodemap, 0, sizeof aot_nodemap); memcpy(hc_nodes, hc_tmp, hc_tmp_n * 4); hc_nn = hc_tmp_n;
      for (unsigned i = 0; i < hc_tmp_n; i++) { uint32_t a = hc_tmp[i] + 4, b = hc_tmp[i] + 0x1b; aot_nodemap[(a >> 8) & 0x1FFF] |= 1u << ((a >> 5) & 7); aot_nodemap[(b >> 8) & 0x1FFF] |= 1u << ((b >> 5) & 7); } }
    hc_in_lock = 0;
  } else hc_in_lock = 1;
}
/* Called from the fatal-signal handler: report the allocator state exactly as it is at the crash. */
void aot_heap_crash_dump(void) {
  if (!aot_is_pe32 || !in_img(0x199dec8u, 0x80)) return;
  fprintf(stderr, "[heap] state at crash (checks so far %llu, treemap %#x):\n", (unsigned long long)hc_checks, LD(uint32_t, 0x199dda0u));
  for (int i = 0; i < 32; i++) { uint32_t s = LD(uint32_t, 0x199dec8u + 4 * i); if (s) fprintf(stderr, "  bin %2d slot %#x\n", i, s); }
  int p = heap_validate("crash", 1); fprintf(stderr, "[heap] crash-time validation: %d problem(s)\n", p);
  heap_overlap_check("crash");
}

static uint64_t run(CPU *c, uint64_t pc, int stop_idx) {
  CPU *prev_cpu = cur_cpu; cur_cpu = c;
  for (;;) {
    if (pc >= THUNK_BASE && pc < THUNK_BASE + (uint64_t)MAXTHUNK * THUNK_SZ) {
      int idx = (int)((pc - THUNK_BASE) / THUNK_SZ);
      if (idx == EXIT_IDX) { fflush(stdout); exit((int)c->r[0]); }
      if (idx == RET_IDX) { cur_cpu = prev_cpu; return c->r[0]; }
      Thunk *t = &thunks[idx]; recent[recent_n++ % 32] = idx; { WdThread *w = wd_self(); w->imp[w->in++ % 12] = idx; } { static int tr = -1; if (tr < 0) tr = getenv("AOT_TRACE_IMPORTS") != NULL; if (tr) fprintf(stderr, "[imp] %s\n", t->name); }
      if (!t->fn) { fflush(stdout); fprintf(stderr, "\n[aot] call to unimplemented import: %s (rcx=0x%llx rdx=0x%llx)\n", t->name, (unsigned long long)c->r[1], (unsigned long long)c->r[2]); dump_context(c); exit(72); }
      if (aot_is_pe32) { pc = aot_x86_thunk(c, t->fn, t->pop, t->fp); { static int8_t iocls[MAXTHUNK]; int8_t k_ = iocls[idx]; if (!k_) { const char *dt = strchr(t->name, '!'); const char *nn = dt ? dt + 1 : t->name; static const char *const io_names[] = { "ReadFile", "GetOverlappedResult", "LockFileEx", "UnlockFileEx", "GetFileSizeEx", "SetFilePointerEx", "WriteFile", "SetEndOfFile", "SetFilePointer", "GetFileSize", NULL }; k_ = 1; for (int q = 0; io_names[q]; q++) if (!strcmp(nn, io_names[q])) k_ = 2; iocls[idx] = k_; } if (k_ == 2) io_fail_note(t->name, (uint32_t)c->r[0]); } continue; }
      c->r[0] = t->fn(c);
      pc = LD(uint64_t, c->r[4]); c->r[4] += 8; continue;
    }
    static __thread struct { uint64_t pc; int k; } rcache[4096]; unsigned rh = (unsigned)((pc * 0x9E3779B1u) >> 8) & 4095; int lo = 0, hi = aot_nregions - 1, k = -1;
    if (rcache[rh].pc == pc && rcache[rh].k > 0) k = rcache[rh].k - 1; else
    while (lo <= hi) { int m = (lo + hi) / 2; if (pc < aot_region_lo[m]) hi = m - 1; else if (pc >= aot_region_hi[m]) lo = m + 1; else { k = m; break; } }
    if (k >= 0) { rcache[rh].pc = pc; rcache[rh].k = k + 1; }
    if (k < 0) { fflush(stdout); fprintf(stderr, "\n[aot] jump to untranslated address 0x%llx (indirect target not discovered, or non-code)\n", (unsigned long long)pc); dump_context(c); exit(73); }
    aot_last_pc = pc; recent_pc[recent_pc_n++ % 16] = pc; { WdThread *w = wd_self(); w->pc = pc; w->n++; w->cpu = c; }
    uint64_t np = aot_region_fn[k](c, pc);
    if (np == pc) {   /* regions overlap: the binary search may land on one that does not enter at pc, so try every region covering pc */
      for (int m = 0; m < aot_nregions && np == pc; m++) if (m != k && pc >= aot_region_lo[m] && pc < aot_region_hi[m]) np = aot_region_fn[m](c, pc);
    }
    if (np == pc) { fflush(stdout); fprintf(stderr, "\n[aot] address 0x%llx is inside translated code but is not a known entry point (undiscovered indirect jump target)\n", (unsigned long long)pc);
      fprintf(stderr, "[aot] regions containing pc (index lo hi):"); for (int m = 0; m < aot_nregions; m++) if (pc >= aot_region_lo[m] && pc < aot_region_hi[m]) fprintf(stderr, " #%d 0x%llx-0x%llx", m, (unsigned long long)aot_region_lo[m], (unsigned long long)aot_region_hi[m]);
      fprintf(stderr, "\n[aot] searched region #%d\n", k); dump_context(c); exit(73); }
    pc = np;
  }
  (void)stop_idx;
}

uint64_t aot_call(CPU *c, uint64_t fn, int nargs, ...) {
  va_list ap; va_start(ap, nargs);
  /* Nested guest calls may use vector registers and leave flags/DF live. */
  CPU save = *c;
  if (aot_is_pe32) {
    extern __thread uint32_t aot_x86_gesp;   /* inside a shim, r[4] is the host arg frame: nest below the guest esp instead */
    uint32_t esp = (aot_x86_gesp ? aot_x86_gesp - 64u : (uint32_t)c->r[4]) - 4u * (uint32_t)(nargs + 1);
    ST(uint32_t, esp, (uint32_t)(THUNK_BASE + RET_IDX * THUNK_SZ));
    for (int i = 0; i < nargs; i++) ST(uint32_t, esp + 4 + 4 * i, (uint32_t)va_arg(ap, uint64_t));
    c->r[4] = esp;
  } else {
    uint64_t rsp = c->r[4] - 0x20 - 8 * (nargs > 4 ? nargs - 4 : 0); rsp &= ~15ULL;
    for (int i = 0; i < nargs; i++) { uint64_t v = va_arg(ap, uint64_t); if (i == 0) c->r[1] = v; else if (i == 1) c->r[2] = v; else if (i == 2) c->r[8] = v; else if (i == 3) c->r[9] = v; else ST(uint64_t, rsp + 0x20 + 8 * (i - 4), v); }
    rsp -= 8; ST(uint64_t, rsp, THUNK_BASE + RET_IDX * THUNK_SZ); c->r[4] = rsp;
  }
  va_end(ap);
  uint64_t r = run(c, fn, RET_IDX);
  *c = save;
  return r;
}

#ifdef AOT_ANDROID_APP
#define main aot_main
#endif
/* Run guest code `fn(param)` on a fresh guest CPU/TEB/stack (used by CreateThread). Returns eax. */
#include <sys/syscall.h>
/* x86 guests rely on total store order; ARM reorders plain loads/stores between cores, so lock-free hand-offs in the game
   (job flags, refcounts, lock-free queues) can see stale data and corrupt its allocator.  Running every guest thread on ONE core
   gives the same ordering guarantees a single-core x86 has, and thread switches are full barriers.  AOT_NOPIN=1 disables it. */
static int g_pin_cpu = -2;
static void aot_pin_guest_thread(void) {
  if (!getenv("AOT_PIN")) return;   /* off by default: pinning did not change the crash */
  if (g_pin_cpu == -2) {
    int best = -1; long bf = -1;
    for (int i = 0; i < 16; i++) { char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
      FILE *f = fopen(p, "r"); if (!f) continue; long v = 0; if (fscanf(f, "%ld", &v) == 1 && v > bf) { bf = v; best = i; } fclose(f); }
    g_pin_cpu = best >= 0 ? best : 7;
    fprintf(stderr, "[aot] guest threads pinned to cpu %d (single-core ordering, x86 TSO emulation)\n", g_pin_cpu);
  }
  uint64_t mask[2] = { g_pin_cpu < 64 ? 1ull << g_pin_cpu : 0, 0 };
  if (syscall(SYS_sched_setaffinity, 0, sizeof mask, mask) != 0) { static int w; if (!w++) fprintf(stderr, "[aot] pinning failed: %s\\n", strerror(errno)); }
}
/* ---- guest stacks: reserved in full, committed on demand (Windows grows a stack inside its reserve) ---- */
#define GST_MAX 64
static struct { uint32_t lo, hi, cl; } gst[GST_MAX]; static volatile int gst_n;
static uint8_t *gstack_new(size_t ssz, size_t commit) {
  uint8_t *p = aot_low_map(ssz, 0); if (!p) return NULL;
  size_t keep = commit < ssz ? commit : ssz; keep = (keep + 0xFFF) & ~(size_t)0xFFF; if (keep > ssz) keep = ssz;
  int i = __atomic_fetch_add(&gst_n, 1, __ATOMIC_RELAXED);
  if (i < GST_MAX) { gst[i].lo = (uint32_t)(uintptr_t)p; gst[i].hi = (uint32_t)(uintptr_t)(p + ssz); gst[i].cl = gst[i].hi - (uint32_t)keep; }
  else __atomic_store_n(&gst_n, GST_MAX, __ATOMIC_RELAXED);
  if (keep < ssz) mprotect(p, ssz - keep, PROT_NONE);   /* the rest commits on first touch, see aot_stack_grow */
  return p;
}
static void gstack_free(uint8_t *p, size_t ssz) {
  int n = __atomic_load_n(&gst_n, __ATOMIC_RELAXED);
  for (int i = 0; i < n && i < GST_MAX; i++) if (gst[i].lo == (uint32_t)(uintptr_t)p) { gst[i] = gst[n - 1]; __atomic_store_n(&gst_n, n - 1, __ATOMIC_RELAXED); break; }
  munmap(p, ssz);
}
/* Fatal-signal handler: a fault on a reserved-but-uncommitted page of a guest stack commits that one page and returns 1, so the faulting access
   retries instead of killing the process. Tracking cl means a repeat fault on an already committed page is NOT swallowed. */
int aot_stack_grow(uintptr_t a, void *uc) {
  (void)uc; int n = __atomic_load_n(&gst_n, __ATOMIC_RELAXED);
  for (int i = 0; i < n && i < GST_MAX; i++)
    if (a >= gst[i].lo && a < gst[i].cl) {
      uintptr_t pg = a & ~(uintptr_t)0xFFF;
      if (pg >= gst[i].lo && mprotect((void *)pg, 0x1000, PROT_READ | PROT_WRITE) == 0) { __atomic_store_n(&gst[i].cl, (uint32_t)pg, __ATOMIC_RELAXED); return 1; }
    }
  return 0;
}
uint64_t aot_thread_run(uint64_t fn, uint64_t param, size_t ssz, uint32_t tid) {
  aot_pin_guest_thread();
  if (ssz < (1u << 20)) ssz = 1u << 20; if (ssz > (64u << 20)) ssz = 64u << 20;
  CPU *c = calloc(1, sizeof(CPU)); if (!c) return 0;
  if (aot_is_pe32) {
    uint8_t *teb = aot_gcalloc(1, 0x2000); uint32_t *slots = aot_gcalloc(64, 4);
    uint8_t *blk = g_tls_total ? aot_gcalloc(1, g_tls_total + 64) : NULL;
    uint8_t *stk = gstack_new(ssz, 256u << 10);
    if (!teb || !slots || !stk || (g_tls_total && !blk)) { if (stk) gstack_free(stk, ssz); free(c); return 0; }
    if (blk) memcpy(blk, (void *)g_tls_s0, g_tls_n); slots[0] = (uint32_t)(uintptr_t)blk;
    ST(uint32_t, teb + 0x00, 0xffffffffu); ST(uint32_t, teb + 0x04, (uint32_t)(uintptr_t)(stk + ssz)); ST(uint32_t, teb + 0x08, (uint32_t)(uintptr_t)stk);
    ST(uint32_t, teb + 0x18, (uint32_t)(uintptr_t)teb); ST(uint32_t, teb + 0x20, tid); ST(uint32_t, teb + 0x2c, (uint32_t)(uintptr_t)slots); ST(uint32_t, teb + 0x30, (uint32_t)g_peb_addr);
    c->gs = (uint64_t)(uintptr_t)teb; c->fcw = 0x37f; c->mxcsr = 0x1f80;
    uint32_t esp = ((uint32_t)(uintptr_t)(stk + ssz - 0x100) & ~15u) - 4;
    ST(uint32_t, esp, (uint32_t)(THUNK_BASE + RET_IDX * THUNK_SZ)); ST(uint32_t, esp + 4, (uint32_t)param); c->r[4] = esp;
    uint64_t r = run(c, fn, RET_IDX);
    gstack_free(stk, ssz); aot_gfree(slots); aot_gfree(blk); aot_gfree(teb); free(c); return r;
  }
  uint8_t *teb = calloc(1, 0x2000); uint64_t *slots = calloc(64, 8);
  uint8_t *blk = g_tls_total ? calloc(1, g_tls_total + 64) : NULL; if (blk) memcpy(blk, (void *)g_tls_s0, g_tls_n); slots[0] = (uint64_t)blk;
  ST(uint64_t, teb + 0x30, teb); ST(uint64_t, teb + 0x60, g_peb_addr); ST(uint64_t, teb + 0x58, slots); ST(uint32_t, teb + 0x48, tid);
  uint8_t *stk = mmap(NULL, ssz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  ST(uint64_t, teb + 0x08, (uint64_t)stk + ssz); ST(uint64_t, teb + 0x10, stk);
  c->gs = (uint64_t)teb; c->fcw = 0x37f; c->mxcsr = 0x1f80; uint64_t rsp = ((uint64_t)stk + ssz - 0x100) & ~15ULL; rsp -= 8; ST(uint64_t, rsp, THUNK_BASE + RET_IDX * THUNK_SZ);
  c->r[4] = rsp; c->r[1] = param;
  uint64_t r = run(c, fn, RET_IDX);
  munmap(stk, ssz); free(slots); free(blk); free(teb); free(c); return r;
}

int main(int argc, char **argv) {
  aot_pin_guest_thread();
  if (argc < 2) { fprintf(stderr, "usage: %s game.exe [args...]\n", argv[0]); return 2; }
  g_exe_path = argv[1];
  { pthread_t wt; if (!pthread_create(&wt, NULL, watchdog, NULL)) pthread_detach(wt); }
  FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
  fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *file = malloc(fsz); if (fread(file, 1, fsz, f) != (size_t)fsz) return 2; fclose(f);
  uint32_t pe = u32(file, 0x3c);
  int p32 = u16(file, pe + 24) == 0x10b;
  if (memcmp(file + pe, "PE\0\0", 4) || (u16(file, pe + 24) != 0x20b && !p32) || p32 != aot_is_pe32) { fprintf(stderr, "executable architecture does not match this build (PE32=%d, build PE32=%d)\n", p32, aot_is_pe32); return 2; }
  uint16_t nsec = u16(file, pe + 6), optsz = u16(file, pe + 20); uint32_t opt = pe + 24;
  uint64_t base = p32 ? u32(file, opt + 28) : u64(file, opt + 24); uint32_t simg = u32(file, opt + 56), shdr = u32(file, opt + 60);
  if (base != aot_image_base) { fprintf(stderr, "image base mismatch: exe 0x%llx, recompiled 0x%llx (wrong exe for this build?)\n", (unsigned long long)base, (unsigned long long)aot_image_base); return 2; }
  uint64_t entry = base + u32(file, opt + 16);
  if (entry != aot_entry) {
    fprintf(stderr, "entry point mismatch: exe 0x%llx, recompiled 0x%llx (wrong exe for this build?)\n",
            (unsigned long long)entry, (unsigned long long)aot_entry);
    return 2;
  }
  if (aot_image_hash && image_fingerprint(file, (size_t)fsz) != aot_image_hash) {
    fprintf(stderr, "executable fingerprint mismatch (wrong exe for this build?)\n");
    return 2;
  }
  /* Android: wrap.sh/aot_reserve.so reserved [lo,hi) before ART started; map over our own reservation (never over foreign memory). */
  const char *rlo = getenv("AOT_RESERVED_LO"), *rhi = getenv("AOT_RESERVED_HI");
  int inres = rlo && rhi && base >= strtoull(rlo, 0, 0) && base + simg <= strtoull(rhi, 0, 0);
  uint8_t *img = mmap((void *)base, simg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | (inres ? MAP_FIXED : MAP_FIXED_NOREPLACE), -1, 0);
  if (img != (void *)base) {
    fprintf(stderr, "cannot map image at 0x%llx (size 0x%x): %s\nmappings overlapping that range (from /proc/self/maps):\n", (unsigned long long)base, simg, strerror(errno));
    FILE *mf = fopen("/proc/self/maps", "r"); char ln[512];
    while (mf && fgets(ln, sizeof ln, mf)) { unsigned long long lo, hi; if (sscanf(ln, "%llx-%llx", &lo, &hi) == 2 && lo < base + simg && hi > base) fputs(ln, stderr); }
    if (mf) fclose(mf);
    return 2;
  }
  g_image_base = base; g_image_size = simg;
  memcpy(img, file, shdr);
  for (int i = 0; i < nsec; i++) {
    const uint8_t *s = file + opt + optsz + 40 * i; uint32_t va = u32(s, 12), raw = u32(s, 16), ptr = u32(s, 20), vs = u32(s, 8);
    uint32_t n = raw < vs || !vs ? raw : vs; if (ptr + n > (uint32_t)fsz) n = fsz > ptr ? fsz - ptr : 0;
    memcpy(img + va, file + ptr, n);
  }
  uint32_t dd = opt + (p32 ? 96 : 112);
  bind_imports(img, u32(file, dd + 8), getenv("AOT_VERBOSE") != NULL);
  if (!p32) bind_delay_imports(img, u32(file, dd + 13 * 8),
                     u32(file, dd + 13 * 8 + 4), base, simg,
                     getenv("AOT_VERBOSE") != NULL);
  /* TLS: copy template, set index 0 */
  static uint64_t tls_slots[64]; uint64_t tls_block = 0;
  uint32_t tls_rva = u32(file, dd + 72);
  if (tls_rva) {
    uint64_t s0 = p32 ? u32(img, tls_rva) : u64(img, tls_rva), e0 = p32 ? u32(img, tls_rva + 4) : u64(img, tls_rva + 8), idxp = p32 ? u32(img, tls_rva + 8) : u64(img, tls_rva + 16);
    uint32_t zf = u32(img, tls_rva + (p32 ? 16 : 32));
    size_t n = e0 - s0; g_tls_s0 = s0; g_tls_n = n; g_tls_total = n + zf; tls_block = (uint64_t)aot_gcalloc(1, n + zf + 64); memcpy((void *)tls_block, (void *)s0, n);
    if (idxp) ST(uint32_t, idxp, 0);
  }
  tls_slots[0] = tls_block;
  CPU *c;
  if (p32) {   /* PE32: TEB/PEB/stack/TLS array all live below 4GB; fs base = TEB */
    uint8_t *teb = aot_gcalloc(1, 0x2000), *peb = aot_gcalloc(1, 0x1000); uint32_t *slots = aot_gcalloc(64, 4); slots[0] = (uint32_t)tls_block;
    size_t ssz = u32(file, opt + 72); if (ssz < (4u << 20)) ssz = 4u << 20; if (ssz > (64u << 20)) ssz = 64u << 20;
    uint8_t *stk = gstack_new(ssz, 256u << 10); if (!stk) { fprintf(stderr, "[aot] cannot allocate 32-bit guest stack\n"); return 2; }
    ST(uint32_t, teb + 0x00, 0xFFFFFFFFu); ST(uint32_t, teb + 0x04, (uint64_t)stk + ssz); ST(uint32_t, teb + 0x08, stk); ST(uint32_t, teb + 0x18, teb);
    ST(uint32_t, teb + 0x20, 1); ST(uint32_t, teb + 0x24, 1); ST(uint32_t, teb + 0x2C, slots); ST(uint32_t, teb + 0x30, peb); ST(uint32_t, peb + 0x08, base);
    c = calloc(1, sizeof(CPU)); c->gs = (uint64_t)teb; c->fcw = 0x37F; c->mxcsr = 0x1f80; g_peb_addr = (uint64_t)peb; g_stack_top = (uint64_t)stk + ssz;
    uint32_t sp = (uint32_t)(((uint64_t)stk + ssz - 0x100) & ~15ULL) - 4; ST(uint32_t, sp, THUNK_BASE + EXIT_IDX * THUNK_SZ); c->r[4] = sp;
  } else {
  /* TEB / PEB */
  uint8_t *teb = calloc(1, 0x2000), *peb = calloc(1, 0x1000);
  ST(uint64_t, teb + 0x30, teb); ST(uint64_t, teb + 0x60, peb); ST(uint64_t, teb + 0x58, tls_slots); ST(uint64_t, peb + 0x10, base);
  ST(uint64_t, teb + 0x08, 0x7fffffff0000ULL);
  /* stack */
  size_t ssz = u64(file, opt + 72); if (ssz < (8u << 20)) ssz = 8u << 20; if (ssz > (256u << 20)) ssz = 256u << 20;
  uint8_t *stk = mmap(NULL, ssz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  g_stack_top = (uint64_t)stk + ssz;
  c = calloc(1, sizeof(CPU)); c->gs = (uint64_t)teb; c->fcw = 0x37F; c->mxcsr = 0x1f80;
  uint64_t rsp = (g_stack_top - 0x100) & ~15ULL; rsp -= 8; ST(uint64_t, rsp, THUNK_BASE + EXIT_IDX * THUNK_SZ);
  c->r[4] = rsp; c->r[1] = (uint64_t)peb; g_peb_addr = (uint64_t)peb;
  }
  snprintf(g_cmdline, sizeof g_cmdline, "%s", argv[1]);
  for (int i = 2; i < argc; i++) { strncat(g_cmdline, " ", sizeof g_cmdline - strlen(g_cmdline) - 1); strncat(g_cmdline, argv[i], sizeof g_cmdline - strlen(g_cmdline) - 1); }
  shims_init(argc - 1, argv + 1);
  aot_sys_init();
  run(c, aot_entry, -1);
  aot_win_audio_cleanup();
  return 0;
}
