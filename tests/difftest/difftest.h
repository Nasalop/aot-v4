/* Differential-test scaffolding for output of src/transpiler/aot_transpiler.py.
 *
 * The generated file only uses names that the real runtime provides (LD, ST, PUSH32,
 * POP32, PAR, DECL_STATE, SAVES, aot_unimpl, c->x, c->mm, c->sregs, c->df, c->fsw ...),
 * with two deliberate differences, both documented in tests/difftest/run.py:
 *   - memory is a sparse page table that returns zeroed pages for anything not
 *     preloaded, exactly like the Unicorn oracle's unmapped-page hook;
 *   - the real ST() heap guards (aot_stguard / aot_nodehit) are absent, because this
 *     harness measures instruction semantics, not the corruption guards.
 * Flag *values* are kept bit-identical to the runtime's assignments.
 */
#ifndef DIFFTEST_H
#define DIFFTEST_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef union { uint8_t b[16]; uint16_t w[8]; uint32_t d[4]; uint64_t q[2]; float f[4]; double g[2]; } XMM;
typedef union { uint8_t b[8]; uint16_t w[4]; uint32_t d[2]; uint64_t q; } MM;
typedef union { uint8_t b[10]; uint16_t w[5]; uint32_t d[2]; } aot_f80;

typedef struct {
  uint64_t r[16]; XMM x[18]; XMM xh[16]; MM mm[8];
  uint64_t gs; uint32_t mxcsr; uint16_t sregs[6];   /* es, cs, ss, ds, fs, gs */
  uint8_t cf, zf, sf, of, pf, df;
  aot_f80 fst[8]; uint16_t fcw, fsw; uint8_t ftop;
  uint32_t pc;
} CPU;

/* ---- memory: sparse, zero filled, identical to the oracle's view ---- */
extern uint8_t *dt_pages[1u << 20];   /* one entry per 4 KB page of the whole 32-bit space */
uint8_t *dt_page(uint32_t a);
void dt_ld(void *dst, uint32_t a, unsigned n);
void dt_st(uint32_t a, const void *src, unsigned n);

#define LD(T, a) ({ T _v; dt_ld(&_v, (uint32_t)(a), sizeof _v); _v; })
#define ST(T, a, v) do { T _v = (T)(v); dt_st((uint32_t)(a), &_v, sizeof _v); } while (0)
#define LDX(a) ({ XMM _t; dt_ld(&_t, (uint32_t)(a), 16); _t; })
#define STX(a, v) do { XMM _t = (v); dt_st((uint32_t)(a), &_t, 16); } while (0)

#define PAR(v) ((uint8_t)!__builtin_parity((uint8_t)(v)))
#define PUSH32(v) do { uint32_t _p = (uint32_t)(v); r4 = (uint32_t)(r4 - 4); ST(uint32_t, r4, _p); } while (0)
#define POP32() ({ uint32_t _p = LD(uint32_t, r4); r4 = (uint32_t)(r4 + 4); _p; })
#define PUSH(v) PUSH32(v)
#define POP() POP32()

#define DECL_STATE \
  uint64_t r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3], r4 = c->r[4], r5 = c->r[5], r6 = c->r[6], r7 = c->r[7], \
           r8 = c->r[8], r9 = c->r[9], r10 = c->r[10], r11 = c->r[11], r12 = c->r[12], r13 = c->r[13], r14 = c->r[14], r15 = c->r[15]; \
  uint8_t cf = c->cf, zf = c->zf, sf = c->sf, of = c->of, pf = c->pf

#define SAVES() do { \
  c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7; \
  c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11; c->r[12] = r12; c->r[13] = r13; c->r[14] = r14; c->r[15] = r15; \
  c->cf = cf; c->zf = zf; c->sf = sf; c->of = of; c->pf = pf; } while (0)

#define EXIT(a) do { SAVES(); return (a); } while (0)
#define aot_rdtsc() ((uint64_t)0)   /* deterministic: the oracle's RDTSC is 0 too */

static inline int64_t aot_d2i(double v, int bits) {   /* same as the runtime's cvtt helper */
  if (v != v) return bits == 64 ? INT64_MIN : INT32_MIN;
  if (bits == 64) return (v >= 9223372036854775808.0 || v < -9223372036854775808.0) ? INT64_MIN : (int64_t)v;
  return (v >= 2147483648.0 || v < -2147483648.0) ? INT32_MIN : (int32_t)v;
}

/* ---- trace ---- */
void dt_snap(CPU *c, uint32_t pc, const uint64_t *r, uint8_t cf, uint8_t zf, uint8_t sf, uint8_t of, uint8_t pf);
void dt_unimpl(uint32_t pc, const char *what);
#define SNAP(a) dt_snap(c, (a), (uint64_t[]){ r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15 }, cf, zf, sf, of, pf)
#define aot_unimpl(pc, s) do { dt_unimpl((uint32_t)(pc), (s)); goto dtreexit; } while (0)

uint32_t difftest_run(CPU *c);   /* emitted by tests/difftest/gen.py */

#endif
