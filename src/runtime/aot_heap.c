/* Replacement for AC1's built-in dlmalloc (mspace malloc 0x41ef40 / free 0x417890, hooked by the transpiler).
 * Size-segregated free lists (16-byte steps to 512 B, then 4 steps per power of two to 128 KB), 4 MB bump arenas, exact page mappings above that.
 * Every block keeps a dlmalloc-style header (size|PINUSE|CINUSE at u-4) so the game's size queries still work.  All blocks live below 4 GB. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
extern void *aot_map_anon(size_t n);
#define HK_MAGIC 0xA0C0DE01u
#define HK_LARGE (128u << 10)
#define HK_ARENA (4u << 20)
#define HK_NCLS 64
#define W32(a) (*(uint32_t *)(uintptr_t)(a))

static inline unsigned cls_of(uint32_t n, uint32_t *S) {
  if (n <= 512) { unsigned c = n ? (n + 15) / 16 - 1 : 0; *S = (c + 1) * 16; return c; }
  unsigned k = 31 - __builtin_clz(n - 1); uint32_t step = 1u << (k - 2), s = (n + step - 1) & ~(step - 1);
  *S = s; return 32 + (k - 9) * 4 + (s - (1u << k)) / step - 1;
}
typedef struct { uint32_t head; volatile int lock; } FL;
static FL fl[HK_NCLS];
static volatile int arena_lock; static uint32_t arena_cur, arena_end;
static uint8_t *volatile reg;                                   /* 1 bit per 16 bytes: live blocks (catches foreign pointers and double frees) */
static inline void lk(volatile int *l) { unsigned spin = 0; while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) { if (++spin > 64) { sched_yield(); spin = 0; } } }
static inline void ul(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }
static uint8_t *bits(void) {
  uint8_t *r = __atomic_load_n(&reg, __ATOMIC_ACQUIRE); if (r) return r;
  void *m = mmap(NULL, 1u << 24, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); if (m == MAP_FAILED) return NULL;
  uint8_t *e = NULL; if (!__atomic_compare_exchange_n(&reg, &e, (uint8_t *)m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { munmap(m, 1u << 24); return e; }
  return (uint8_t *)m;
}
static uint32_t carve(uint32_t S) {
  uint32_t stride = S + 16, u = 0;
  lk(&arena_lock);
  if (!arena_cur || arena_cur + stride > arena_end) {
    uint8_t *a = aot_map_anon(HK_ARENA);
    if (a && (uint64_t)(uintptr_t)a + HK_ARENA <= 0xffff0000ull) { arena_cur = (uint32_t)(uintptr_t)a; arena_end = arena_cur + HK_ARENA; } else arena_cur = arena_end = 0;
  }
  if (arena_cur) { W32(arena_cur) = 0; u = arena_cur + 16; arena_cur += stride; }
  ul(&arena_lock); return u;
}
uint32_t aot_hook_malloc(uint32_t n) {
  uint8_t *rg = bits(); if (!rg || n > 0x7fff0000u) return 0; if (!n) n = 1;
  uint32_t u, chunk;
  if (n >= HK_LARGE) {
    size_t len = ((size_t)n + 16 + 0xfff) & ~(size_t)0xfff; uint8_t *b = aot_map_anon(len);
    if (!b || (uint64_t)(uintptr_t)b + len > 0xffff0000ull) return 0;
    W32(b) = HK_MAGIC; W32(b + 4) = (uint32_t)len; u = (uint32_t)(uintptr_t)b + 16; chunk = (n + 8 + 7) & ~7u;
  } else {
    uint32_t S; unsigned c = cls_of(n, &S); FL *f = &fl[c];
    lk(&f->lock); u = f->head; if (u) f->head = W32(u); ul(&f->lock);
    if (!u) { u = carve(S); if (!u) return 0; }
    W32(u - 16) = 0; W32(u - 12) = S; chunk = S + 8;
  }
  W32(u - 8) = 0; W32(u - 4) = chunk | 3;
  __atomic_fetch_or(&rg[u >> 7], (uint8_t)(1u << ((u >> 4) & 7)), __ATOMIC_RELAXED);
  return u;
}
void aot_hook_free(uint32_t u) {
  static int bad; uint8_t *rg = bits(); if (!u || !rg) return;
  uint8_t bit = (uint8_t)(1u << ((u >> 4) & 7));
  if (!(__atomic_fetch_and(&rg[u >> 7], (uint8_t)~bit, __ATOMIC_RELAXED) & bit)) { if (bad++ < 20) fprintf(stderr, "[mem] free of a pointer the heap did not hand out (or double free): %#x\n", u); return; }
  if (W32(u - 16) == HK_MAGIC) { munmap((void *)(uintptr_t)(u - 16), W32(u - 12)); return; }
  uint32_t S = W32(u - 12), S2; unsigned c = cls_of(S, &S2); FL *f = &fl[c];
  lk(&f->lock); W32(u) = f->head; f->head = u; ul(&f->lock);
}
