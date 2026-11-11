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
  uint64_t m; uint16_t se; memcpy(&m, (const void *)(uintptr_t)a, 8); memcpy(&se, (const void *)(uintptr_t)(a + 8), 2);
  int e = se & 0x7fff, sg = se >> 15; double r;
  if (e == 0 && m == 0) r = 0.0; else if (e == 0x7fff) r = (m << 1) ? NAN : INFINITY; else r = ldexp((double)m, e - 16383 - 63);
  return sg ? -r : r;
#endif
}
static inline void aot_st80(uint64_t a, aot_f80 v) {
#if defined(__x86_64__) && !defined(AOT_FPU_DOUBLE)
  memcpy((void *)(uintptr_t)a, &v, 10);
#else
  uint64_t m = 0; uint16_t se = signbit(v) ? 0x8000 : 0; double d = fabs(v);
  if (d != d) { se |= 0x7fff; m = 0xC000000000000000ULL; } else if (d == INFINITY) { se |= 0x7fff; m = 0x8000000000000000ULL; }
  else if (d != 0) { int e; double f = frexp(d, &e); m = (uint64_t)ldexp(f, 64); se |= (uint16_t)(e - 1 + 16383); }
  memcpy((void *)(uintptr_t)a, &m, 8); memcpy((void *)(uintptr_t)(a + 8), &se, 2);
#endif
}
/* NaN guard for x87 stores: the game's grid-walk loops spin forever on NaN. First sites are logged; NaN is stored as 0. */
static inline aot_f80 aot_nanckm(aot_f80 v, uint32_t pc, uint64_t a) {
  if (__builtin_expect(v != v, 0)) {
    static volatile int n;
    if (n < 24) { n++; const uint32_t *w = (const uint32_t *)(uintptr_t)(a & ~3ULL);
      fprintf(stderr, "[aot] x87 NaN LOADED at pc=%#x from %#llx: [%08x %08x %08x %08x | %08x %08x %08x %08x]\n", pc, (unsigned long long)a, w[-4], w[-3], w[-2], w[-1], w[0], w[1], w[2], w[3]); }
  }
  return v;
}
static inline aot_f80 aot_nanck(aot_f80 v, uint32_t pc) {
  if (__builtin_expect(v != v, 0)) {
    static volatile int n;
    if (n < 24) { n++; fprintf(stderr, "[aot] x87 NaN store at pc=%#x -> (value kept, as on hardware)\n", pc); }
  }
  return v;
}
/* x87 float -> integer with FCW rounding control (rc<0: truncate). Out-of-range / NaN -> integer indefinite. */
static inline int64_t aot_fist(aot_f80 v, int bits, uint16_t fcw, int trunc_) {
  int rc = trunc_ ? 3 : (fcw >> 10) & 3; aot_f80 r;
  if (v != v) goto bad;
  r = rc == 0 ? F80(nearbyint)(v) : rc == 1 ? F80(floor)(v) : rc == 2 ? F80(ceil)(v) : F80(trunc)(v);
  if (bits == 16) { if (r > 32767 || r < -32768) goto bad; return (int16_t)r; }
  if (bits == 32) { if (r > 2147483647.0 || r < -2147483648.0) goto bad; return (int32_t)r; }
  if (r >= 9223372036854775808.0 || r < -9223372036854775808.0) goto bad; return (int64_t)r;
bad: return bits == 16 ? INT16_MIN : bits == 32 ? INT32_MIN : INT64_MIN;
}
/* FRNDINT: round to integral value per FCW rounding control, staying in floating point (inf/NaN pass through). */
static inline aot_f80 aot_fist_r(aot_f80 v, uint16_t fcw) {
  int rc = (fcw >> 10) & 3;
  return rc == 0 ? F80(nearbyint)(v) : rc == 1 ? F80(floor)(v) : rc == 2 ? F80(ceil)(v) : F80(trunc)(v);
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
void aot_heap_check(const char *where);
uint64_t aot_register_thunk(aot_host_fn fn, const char *name);

#define LD(T, a) ({ T _v; memcpy(&_v, (const void *)(uintptr_t)(a), sizeof _v); _v; })
/* Heap-corruption guard: stores into the game allocator's tree-bin table (0x199dec8..0x199df48) are checked.
   A 4-byte non-pointer there is dropped and reported with the writing region (see aot_stguard). */
extern int aot_stguard(uint64_t addr, uint64_t val, unsigned size);
#define ST(T, a, v) do { T _v = (T)(v); uint64_t _a = (uint64_t)(uintptr_t)(a); \
  if (__builtin_expect((uint32_t)(_a - 0x199dec8u) < 0x80u && _a < 0x100000000ull, 0) && !aot_stguard(_a, (uint64_t)_v, sizeof _v)) break; \
  memcpy((void *)(uintptr_t)_a, &_v, sizeof _v); } while (0)
#define LDX(a) ({ XMM _t; memcpy(&_t, (const void *)(uintptr_t)(a), 16); _t; })
#define STX(a, v) do { XMM _t = (v); memcpy((void *)(uintptr_t)(a), &_t, 16); } while (0)
#define PAR(v) ((uint8_t)!__builtin_parity((uint8_t)(v)))
extern const int aot_is_pe32;   /* 1 when the recompiled image is a 32-bit PE32 (guest pointers are 32-bit) */
#define PUSH32(v) do { uint32_t _p = (uint32_t)(v); r4 = (uint32_t)(r4 - 4); ST(uint32_t, r4, _p); } while (0)
#define POP32() ({ uint32_t _p = LD(uint32_t, r4); r4 = (uint32_t)(r4 + 4); _p; })
#define PUSH(v) do { uint64_t _p = (v); r4 -= 8; ST(uint64_t, r4, _p); } while (0)
#define POP() ({ uint64_t _p = LD(uint64_t, r4); r4 += 8; _p; })

#define DECL_STATE \
  uint64_t r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3], r4 = c->r[4], r5 = c->r[5], r6 = c->r[6], r7 = c->r[7], \
           r8 = c->r[8], r9 = c->r[9], r10 = c->r[10], r11 = c->r[11], r12 = c->r[12], r13 = c->r[13], r14 = c->r[14], r15 = c->r[15]; \
  uint8_t cf = c->cf, zf = c->zf, sf = c->sf, of = c->of, pf = c->pf
#define SAVES() do { c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7; \
  c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11; c->r[12] = r12; c->r[13] = r13; c->r[14] = r14; c->r[15] = r15; \
  c->cf = cf; c->zf = zf; c->sf = sf; c->of = of; c->pf = pf; } while (0)
#define EXIT(a) do { SAVES(); return (a); } while (0)

static inline int64_t aot_d2i(double v, int bits) {   /* x86 cvtt: "integer indefinite" on overflow/NaN */
  if (v != v) return bits == 64 ? INT64_MIN : INT32_MIN;
  if (bits == 64) return (v >= 9223372036854775808.0 || v < -9223372036854775808.0) ? INT64_MIN : (int64_t)v;
  return (v >= 2147483648.0 || v < -2147483648.0) ? INT32_MIN : (int32_t)v;
}
