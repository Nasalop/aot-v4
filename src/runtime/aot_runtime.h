/* Runtime support for statically recompiled x86_64 PE code (guest addresses == host addresses). */
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifdef AOT_ANDROID_APP
void aot_exit(int) __attribute__((noreturn));
#define exit(c) aot_exit(c)
#define _exit(c) aot_exit(c)
#endif

typedef union { uint8_t b[16]; uint16_t w[8]; uint32_t d[4]; uint64_t q[2]; float f[4]; double g[2]; } XMM;
typedef union { uint8_t b[8]; uint16_t w[4]; uint32_t d[2]; uint64_t q; } MM;
#if defined(__x86_64__) && !defined(AOT_FPU_DOUBLE)
typedef long double aot_f80; /* exact x87 extended precision on x86-64 hosts */
#define F80(fn) fn##l
#else
typedef double aot_f80;      /* ARM64 hosts: x87 emulated in double precision */
#define F80(fn) fn
#endif
/* x[16], x[17] are scratch registers used by the AVX lowering; xh[] holds the upper 128 bits of ymm0-15 */
typedef struct {
  uint64_t r[16]; XMM x[18]; XMM xh[16]; MM mm[8];
  uint64_t gs; uint32_t mxcsr; uint16_t sregs[6]; /* es, cs, ss, ds, fs, gs */
  uint8_t cf, zf, sf, of, pf, df;
  aot_f80 fst[8]; uint16_t fcw, fsw; uint8_t ftop;
} CPU;
#define FST(i) c->fst[(c->ftop + (i)) & 7]
#define FPUSH(v) do { aot_f80 _v = (v); c->ftop = (c->ftop + 7) & 7; c->fst[c->ftop] = _v; } while (0)
#define FPOP() (c->ftop = (c->ftop + 1) & 7)
static inline aot_f80 aot_ld80(uint64_t a) {
#if defined(__x86_64__) && !defined(AOT_FPU_DOUBLE)
  aot_f80 v = 0; memcpy(&v, (const void *)(uintptr_t)a, 10); return v;
#else
  /* Extended -> double straight out of the IEEE fields. This runs on every 80-bit x87 load in a 32-bit
     game, and ldexp() there was a libm call for what is three shifts and a mask. The rare shapes
     (unnormal, out of range) keep the exact ldexp path. */
  uint64_t m; uint16_t se; memcpy(&m, (const void *)(uintptr_t)a, 8); memcpy(&se, (const void *)(uintptr_t)(a + 8), 2);
  int e = se & 0x7fff; uint64_t sg = (uint64_t)(se >> 15) << 63;
  if (e == 0 && m == 0) { double z; uint64_t b = sg; memcpy(&z, &b, 8); return z; }
  if (e == 0x7fff) { uint64_t b = sg | 0x7ff0000000000000ull; if (m != 0x8000000000000000ull) b |= (m >> 11) | 0x8000000000000ull; double d; memcpy(&d, &b, 8); return d; }
  int ex = e - 16383;
  if ((m >> 63) == 0) { double r = ldexp((double)m, ex - 63); return sg ? -r : r; }   /* unnormal */
  /* The mantissa must round to double, so convert the integer (the hardware rounds to nearest) and then
     rescale by a power of two in the exponent field: exact, and no libm call. */
  double dm = (double)m; uint64_t db; memcpy(&db, &dm, 8);
  int ne = (int)((db >> 52) & 0x7ff) + ex - 63;
  if (ne <= 0 || ne >= 0x7ff) { double r = ldexp(dm, ex - 63); return sg ? -r : r; }   /* subnormal / overflow */
  db = (db & ~(0x7ffull << 52)) | ((uint64_t)ne << 52) | sg;
  { double d; memcpy(&d, &db, 8); return d; }
#endif
}
static inline void aot_st80(uint64_t a, aot_f80 v) {
#if defined(__x86_64__) && !defined(AOT_FPU_DOUBLE)
  memcpy((void *)(uintptr_t)a, &v, 10);
#else
  /* double -> extended without frexp/ldexp, which used to cost four libm calls per x87 store. Only a
     subnormal double needs the exact (and here almost never taken) libm path. */
  double d = (double)v; uint64_t b; memcpy(&b, &d, 8);
  uint64_t sg = (b >> 63) << 15; int se = (int)((b >> 52) & 0x7ff); uint64_t fr = b & 0xfffffffffffffull;
  uint64_t m; uint16_t e80;
  if (se == 0x7ff) { e80 = 0x7fff; m = fr ? 0xC000000000000000ull : 0x8000000000000000ull; }
  else if (se == 0) {
    if (!fr) { e80 = 0; m = 0; }
    else { int e2; double f2 = frexp(d < 0 ? -d : d, &e2); m = (uint64_t)ldexp(f2, 64); e80 = (uint16_t)(e2 - 1 + 16383); }
  } else { m = 0x8000000000000000ull | (fr << 11); e80 = (uint16_t)(se - 1023 + 16383); }
  uint16_t se80 = (uint16_t)(e80 | sg);
  memcpy((void *)(uintptr_t)a, &m, 8); memcpy((void *)(uintptr_t)(a + 8), &se80, 2);
#endif
}
/* NaN guard for x87 stores: the game's grid-walk loops spin forever on NaN. First sites are logged; NaN is stored as 0. */
void aot_nan_m(uint32_t pc, uint64_t a) __attribute__((noinline, cold));   /* runtime.c: logs the first few NaN loads/stores; kept out of line so the x87 fast path stays tiny */
void aot_nan_s(uint32_t pc) __attribute__((noinline, cold));
static inline aot_f80 aot_nanckm(aot_f80 v, uint32_t pc, uint64_t a) { if (__builtin_expect(v != v, 0)) aot_nan_m(pc, a); return v; }
static inline aot_f80 aot_nanck(aot_f80 v, uint32_t pc) { if (__builtin_expect(v != v, 0)) aot_nan_s(pc); return v; }
/* x87 float -> integer with FCW rounding control (rc<0: truncate). Out-of-range / NaN -> integer indefinite. */
static inline int64_t aot_fist(aot_f80 v, int bits, uint16_t fcw, int trunc_) {
  int rc = trunc_ ? 3 : (fcw >> 10) & 3; aot_f80 r;
  if (v != v) goto bad;
  r = rc == 0 ? __builtin_nearbyint(v) : rc == 1 ? __builtin_floor(v) : rc == 2 ? __builtin_ceil(v) : __builtin_trunc(v);
  if (bits == 16) { if (r > 32767 || r < -32768) goto bad; return (int16_t)r; }
  if (bits == 32) { if (r > 2147483647.0 || r < -2147483648.0) goto bad; return (int32_t)r; }
  if (r >= 9223372036854775808.0 || r < -9223372036854775808.0) goto bad; return (int64_t)r;
bad: return bits == 16 ? INT16_MIN : bits == 32 ? INT32_MIN : INT64_MIN;
}
/* FRNDINT: round to integral value per FCW rounding control, staying in floating point (inf/NaN pass through). */
static inline aot_f80 aot_fist_r(aot_f80 v, uint16_t fcw) {
  int rc = (fcw >> 10) & 3;
  return rc == 0 ? __builtin_nearbyint(v) : rc == 1 ? __builtin_floor(v) : rc == 2 ? __builtin_ceil(v) : __builtin_trunc(v);
}
typedef uint64_t (*aot_host_fn)(CPU *);

/* generated tables (aot_tab.c) */
extern const uint64_t aot_image_base, aot_entry, aot_image_hash;
extern const int aot_nregions;
extern const uint64_t aot_region_lo[], aot_region_hi[];
extern uint64_t (*const aot_region_fn[])(CPU *, uint64_t);

/* runtime services */
void aot_unimpl(uint64_t addr, const char *text) __attribute__((noreturn));
uint64_t aot_call(CPU *c, uint64_t fn, int nargs, ...);   /* call guest function from a shim */
uint64_t aot_rdtsc(void);
/* Profiling clock: clock_gettime() costs about as much as the D3D proxy calls being measured, so read the
   hardware counter instead (EL0 access to cntvct/cntfrq is enabled by Linux on ARM64). */
static inline uint64_t aot_perf_ticks(void) {
#if defined(__aarch64__)
  uint64_t v; __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v)); return v;
#elif defined(__x86_64__)
  uint32_t lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo;
#else
  return aot_rdtsc();
#endif
}
static inline uint64_t aot_perf_hz(void) {
#if defined(__aarch64__)
  uint64_t v; __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v)); return v ? v : 24000000ull;
#else
  return 3000000000ull;   /* nominal x86 host TSC */
#endif
}
extern volatile uint64_t aot_perf_guest;   /* ticks spent inside translated code (proxy calls nested) */
void aot_heap_check(const char *where);
void aot_heap_crash_dump(void);
uint64_t aot_register_thunk(aot_host_fn fn, const char *name);

#define LD(T, a) ({ T _v; memcpy(&_v, (const void *)(uintptr_t)(a), sizeof _v); _v; })
int aot_vres_peek(uint64_t a, uint64_t *base, uint64_t *size);   /* crash-safe image of the VirtualAlloc reservation map */
int aot_stack_grow(uintptr_t a, void *uc);   /* fatal-signal handler: commit a reserved guest-stack page and retry the access */
/* Heap-corruption guards (game allocator): stores into the free-tree bin table, or into any live free-tree node while the writer does not hold the
   allocator lock, are reported with the writing region and the store is allowed through (see aot_stguard / aot_nodehit in runtime.c): a diagnostic
   that rewrites or drops guest state changes the very thing it is measuring. */
extern int aot_stguard(uint64_t addr, uint64_t val, unsigned size);   /* always 1: report only */
extern uint8_t aot_nodemap[8192];
extern int aot_nodehit(uint64_t addr, uint64_t val, unsigned size);
#define AOT_NODE_SUSPECT(a, n) (((aot_nodemap[((a) >> 8) & 0x1FFF] >> (((a) >> 5) & 7)) | (aot_nodemap[(((a) + (n) - 1) >> 8) & 0x1FFF] >> ((((a) + (n) - 1) >> 5) & 7))) & 1)
#ifdef AOT_RELEASE
/* Release builds (release.yml defines AOT_RELEASE) drop the heap-corruption guards: the checks below ran on every
   guest store, which is a fixed tax on the busiest instructions in the recompiled image. Debug builds keep them. */
#define ST(T, a, v) do { T _v = (T)(v); memcpy((void *)(uintptr_t)(a), &_v, sizeof _v); } while (0)
#else
#define ST(T, a, v) do { T _v = (T)(v); uint64_t _a = (uint64_t)(uintptr_t)(a); \
  if (__builtin_expect(_a < 0x100000000ull, 1)) { \
    if (__builtin_expect((uint32_t)(_a - 0x199dec8u) < 0x80u, 0) && !aot_stguard(_a, (uint64_t)_v, sizeof _v)) break; \
    if (__builtin_expect(AOT_NODE_SUSPECT(_a, sizeof _v), 0) && !aot_nodehit(_a, (uint64_t)_v, sizeof _v)) break; } \
  memcpy((void *)(uintptr_t)_a, &_v, sizeof _v); } while (0)
#endif
#define LDX(a) ({ XMM _t; memcpy(&_t, (const void *)(uintptr_t)(a), 16); _t; })
#ifdef AOT_RELEASE
#define STX(a, v) do { XMM _t = (v); memcpy((void *)(uintptr_t)(a), &_t, 16); } while (0)
#else
#define STX(a, v) do { XMM _t = (v); uint64_t _a = (uint64_t)(uintptr_t)(a); \
  if (__builtin_expect(_a < 0x100000000ull, 1) && __builtin_expect(AOT_NODE_SUSPECT(_a, 16), 0) && !aot_nodehit(_a, 0, 16)) break; \
  memcpy((void *)(uintptr_t)_a, &_t, 16); } while (0)
#endif
#define PAR(v) ((uint8_t)!__builtin_parity((uint8_t)(v)))
extern const int aot_is_pe32;   /* 1 when the recompiled image is a 32-bit PE32 (guest pointers are 32-bit) */
#define PUSH32(v) do { uint32_t _p = (uint32_t)(v); r4 = (uint32_t)(r4 - 4); ST(uint32_t, r4, _p); } while (0)
#define POP32() ({ uint32_t _p = LD(uint32_t, r4); r4 = (uint32_t)(r4 + 4); _p; })
#define PUSH(v) do { uint64_t _p = (v); r4 -= 8; ST(uint64_t, r4, _p); } while (0)
#define POP() ({ uint64_t _p = LD(uint64_t, r4); r4 += 8; _p; })

#ifdef AOT_M32
/* PE32 images only ever touch eax..edi: r8-r15 do not exist for them, so they are neither loaded at a region entry nor stored at an exit (they were 16 of the 21 loads and
   stores every call, return and region transition paid). They stay declared (0) because a few translator templates still name them. */
#define DECL_STATE \
  uint64_t r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3], r4 = c->r[4], r5 = c->r[5], r6 = c->r[6], r7 = c->r[7], \
           r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0; \
  uint8_t cf = c->cf, zf = c->zf, sf = c->sf, of = c->of, pf = c->pf
#define SAVES() do { c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7; \
  c->cf = cf; c->zf = zf; c->sf = sf; c->of = of; c->pf = pf; (void)r8; (void)r9; (void)r10; (void)r11; (void)r12; (void)r13; (void)r14; (void)r15; } while (0)
#else
#define DECL_STATE \
  uint64_t r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3], r4 = c->r[4], r5 = c->r[5], r6 = c->r[6], r7 = c->r[7], \
           r8 = c->r[8], r9 = c->r[9], r10 = c->r[10], r11 = c->r[11], r12 = c->r[12], r13 = c->r[13], r14 = c->r[14], r15 = c->r[15]; \
  uint8_t cf = c->cf, zf = c->zf, sf = c->sf, of = c->of, pf = c->pf
#define SAVES() do { c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7; \
  c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11; c->r[12] = r12; c->r[13] = r13; c->r[14] = r14; c->r[15] = r15; \
  c->cf = cf; c->zf = zf; c->sf = sf; c->of = of; c->pf = pf; } while (0)
#endif
#define EXIT(a) do { SAVES(); return (a); } while (0)
/* Cross-region transfers. A region that leaves through a direct call/jump/fall-through (JMPREG) or a return/indirect target it does not contain
   (AOT_TAILDISP) tail-calls the next region instead of returning to run(): no region lookup and no per-dispatch bookkeeping in the dispatcher loop, and
   the host stack stays flat because the call is a guaranteed tail call. Imports/thunks and unknown addresses still come back to run() as the return value.
   Build with -DAOT_NO_TAIL (or a compiler without musttail) to get the old EXIT() behaviour everywhere. */
#if !defined(AOT_NO_TAIL) && defined(__clang__) && defined(__has_attribute)
#if __has_attribute(musttail)
#define AOT_HAVE_TAIL 1
#endif
#endif
#ifdef AOT_HAVE_TAIL
#define AOT_MUSTTAIL __attribute__((musttail))
uint64_t aot_tail(CPU *c, uint64_t pc);
#define JMPREG(k, a) do { SAVES(); AOT_MUSTTAIL return region_##k(c, (a)); } while (0)
#define AOT_TAILDISP(p, p0) do { if ((p) == (p0)) EXIT(p); SAVES(); AOT_MUSTTAIL return aot_tail(c, (p)); } while (0)
#else
#define AOT_MUSTTAIL
#define JMPREG(k, a) EXIT(a)
#define AOT_TAILDISP(p, p0) EXIT(p)
#endif

static inline int64_t aot_d2i(double v, int bits) {   /* x86 cvtt: "integer indefinite" on overflow/NaN */
  if (v != v) return bits == 64 ? INT64_MIN : INT32_MIN;
  if (bits == 64) return (v >= 9223372036854775808.0 || v < -9223372036854775808.0) ? INT64_MIN : (int64_t)v;
  return (v >= 2147483648.0 || v < -2147483648.0) ? INT32_MIN : (int32_t)v;
}
