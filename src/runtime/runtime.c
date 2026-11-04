/* PE image mapper + import binder + dispatch trampoline for recompiled code. */
#define _GNU_SOURCE
#include "aot_runtime.h"
#include "shims.h"
#include <stdarg.h>
#include <sys/mman.h>
#include <time.h>
#include <errno.h>

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
static int recent[32]; static unsigned recent_n;
uint64_t g_image_size;
/* crash report helper (called from the signal handler): recent imports + the original x86 bytes of the block being executed, so the faulting code can be disassembled offline */
void aot_dump_crash_context(void) {
  dprintf(2, "[aot] last imports called (oldest first):\n");
  for (unsigned i = 0; i < 32; i++) { int idx = recent[(recent_n + i) % 32]; if (idx > 0 && idx < nthunks) dprintf(2, "  %s\n", thunks[idx].name); }
  if (g_image_base && aot_last_pc >= g_image_base && aot_last_pc + 160 <= g_image_base + g_image_size) {
    dprintf(2, "[aot] code at 0x%llx:", (unsigned long long)aot_last_pc);
    for (int i = 0; i < 160; i++) dprintf(2, "%s%02x", i % 32 ? "" : "\n  ", ((const uint8_t *)(uintptr_t)aot_last_pc)[i]);
    dprintf(2, "\n");
  }
}
static void dump_context(CPU *c) {
  fprintf(stderr, "[aot] last guest region entry: 0x%llx\n[aot] last imports called (oldest first):\n", (unsigned long long)aot_last_pc);
  for (unsigned i = 0; i < 32; i++) { int idx = recent[(recent_n + i) % 32]; if (idx > 0 && idx < nthunks) fprintf(stderr, "  %s\n", thunks[idx].name); }
  if (aot_is_pe32 && c) { uint32_t sp = (uint32_t)c->r[4]; fprintf(stderr, "[aot] eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n[aot] stack:", (unsigned)c->r[0], (unsigned)c->r[1], (unsigned)c->r[2], (unsigned)c->r[3], sp, (unsigned)c->r[5], (unsigned)c->r[6], (unsigned)c->r[7]);
    for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", LD(uint32_t, sp + 4 * i)); fprintf(stderr, "\n"); }
}

static uint64_t run(CPU *c, uint64_t pc, int stop_idx) {
  for (;;) {
    if (pc >= THUNK_BASE && pc < THUNK_BASE + (uint64_t)MAXTHUNK * THUNK_SZ) {
      int idx = (int)((pc - THUNK_BASE) / THUNK_SZ);
      if (idx == EXIT_IDX) { fflush(stdout); exit((int)c->r[0]); }
      if (idx == RET_IDX) return c->r[0];
      Thunk *t = &thunks[idx]; recent[recent_n++ % 32] = idx;
      if (!t->fn) { fflush(stdout); fprintf(stderr, "\n[aot] call to unimplemented import: %s (rcx=0x%llx rdx=0x%llx)\n", t->name, (unsigned long long)c->r[1], (unsigned long long)c->r[2]); dump_context(c); exit(72); }
      if (aot_is_pe32) { pc = aot_x86_thunk(c, t->fn, t->pop, t->fp); continue; }
      c->r[0] = t->fn(c);
      pc = LD(uint64_t, c->r[4]); c->r[4] += 8; continue;
    }
    int lo = 0, hi = aot_nregions - 1, k = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (pc < aot_region_lo[m]) hi = m - 1; else if (pc >= aot_region_hi[m]) lo = m + 1; else { k = m; break; } }
    if (k < 0) { fflush(stdout); fprintf(stderr, "\n[aot] jump to untranslated address 0x%llx (indirect target not discovered, or non-code)\n", (unsigned long long)pc); dump_context(c); exit(73); }
    aot_last_pc = pc;
    uint64_t np = aot_region_fn[k](c, pc);
    if (np == pc) { fflush(stdout); fprintf(stderr, "\n[aot] address 0x%llx is inside translated code but is not a known entry point (undiscovered indirect jump target)\n", (unsigned long long)pc); dump_context(c); exit(73); }
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
uint64_t aot_thread_run(uint64_t fn, uint64_t param, size_t ssz, uint32_t tid) {
  if (ssz < (1u << 20)) ssz = 1u << 20; if (ssz > (64u << 20)) ssz = 64u << 20;
  CPU *c = calloc(1, sizeof(CPU)); if (!c) return 0;
  if (aot_is_pe32) {
    uint8_t *teb = aot_gcalloc(1, 0x2000); uint32_t *slots = aot_gcalloc(64, 4);
    uint8_t *blk = g_tls_total ? aot_gcalloc(1, g_tls_total + 64) : NULL;
    uint8_t *stk = aot_low_map(ssz, 0);
    if (!teb || !slots || !stk || (g_tls_total && !blk)) { free(c); return 0; }
    if (blk) memcpy(blk, (void *)g_tls_s0, g_tls_n); slots[0] = (uint32_t)(uintptr_t)blk;
    ST(uint32_t, teb + 0x00, 0xffffffffu); ST(uint32_t, teb + 0x04, (uint32_t)(uintptr_t)(stk + ssz)); ST(uint32_t, teb + 0x08, (uint32_t)(uintptr_t)stk);
    ST(uint32_t, teb + 0x18, (uint32_t)(uintptr_t)teb); ST(uint32_t, teb + 0x20, tid); ST(uint32_t, teb + 0x2c, (uint32_t)(uintptr_t)slots); ST(uint32_t, teb + 0x30, (uint32_t)g_peb_addr);
    c->gs = (uint64_t)(uintptr_t)teb; c->fcw = 0x37f; c->mxcsr = 0x1f80;
    uint32_t esp = ((uint32_t)(uintptr_t)(stk + ssz - 0x100) & ~15u) - 4;
    ST(uint32_t, esp, (uint32_t)(THUNK_BASE + RET_IDX * THUNK_SZ)); ST(uint32_t, esp + 4, (uint32_t)param); c->r[4] = esp;
    uint64_t r = run(c, fn, RET_IDX);
    munmap(stk, ssz); aot_gfree(slots); aot_gfree(blk); aot_gfree(teb); free(c); return r;
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
  if (argc < 2) { fprintf(stderr, "usage: %s game.exe [args...]\n", argv[0]); return 2; }
  g_exe_path = argv[1];
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
    uint8_t *stk = aot_low_map(ssz, 0); if (!stk) { fprintf(stderr, "[aot] cannot allocate 32-bit guest stack\n"); return 2; }
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
