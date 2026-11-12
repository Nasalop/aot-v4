/* Threads, synchronisation, per-thread TLS and SEH-lite (vectored handlers / RaiseException / crash diagnostics). */
#define _GNU_SOURCE
#include "shims.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#define A(n) argn(c, (n))
static inline uint64_t argn(CPU *c, int n) { return n == 0 ? c->r[1] : n == 1 ? c->r[2] : n == 2 ? c->r[8] : n == 3 ? c->r[9] : LD(uint64_t, c->r[4] + 8 + 8 * n); }
#define SYS(n) static uint64_t sys_##n(CPU *c)
#define E(n) { #n, sys_##n }
#define EA(n, f) { n, sys_##f }

#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 0x102u
#define WAIT_FAILED 0xFFFFFFFFu
#define WAIT_ABANDONED 0x80u
#define INFINITE_MS 0xFFFFFFFFu
#define H_BASE 0xA07C000000000000ULL
#define H_MASK 0xFFFF000000000000ULL
#define NOBJ 512
enum { K_THREAD = 1, K_MUTEX, K_SEM };
typedef struct {
  int kind, open; uint32_t gen;
  pthread_t th; uint64_t fn, param; size_t stack; uint32_t tid, exit_code; int done, started;   /* thread */
  uint32_t owner, count;                                                                      /* mutex */
  int32_t sem, sem_max;                                                                       /* semaphore */
} Obj;
static Obj objs[NOBJ];
static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t C = PTHREAD_COND_INITIALIZER;
static uint32_t next_tid = 0x1000;
static __thread uint32_t tl_tid;
static uint32_t cur_tid(void) { if (!tl_tid) tl_tid = __atomic_add_fetch(&next_tid, 4, __ATOMIC_SEQ_CST); return tl_tid; }

static uint64_t h_of(Obj *o) { return H_BASE | ((uint64_t)o->gen << 16) | (uint64_t)(o - objs + 1); }
static Obj *h_get(uint64_t h) {
  if ((h & H_MASK) != H_BASE) return NULL;
  uint32_t i = (uint32_t)(h & 0xffff); if (!i || i > NOBJ) return NULL;
  Obj *o = &objs[i - 1]; return o->open && o->gen == (uint16_t)(h >> 16) ? o : NULL;
}
static Obj *obj_new(int kind) {   /* G held */
  for (int i = 0; i < NOBJ; i++) if (!objs[i].open) { Obj *o = &objs[i]; uint32_t g = o->gen + 1; memset(o, 0, sizeof *o); o->gen = g & 0xffff ? g : 1; o->kind = kind; o->open = 1; return o; }
  return NULL;
}
static shim_fn file_shim(const char *name) { for (const ShimEnt *e = aot_shim_table_file; e->name; e++) if (!strcmp(e->name, name)) return e->fn; return NULL; }
static uint64_t delegate(CPU *c, const char *name, uint64_t a0, uint64_t a1) {
  shim_fn f = file_shim(name); if (!f) return 0;
  CPU t = *c; t.r[1] = a0; t.r[2] = a1; return f(&t);
}

/* ---- waiting ---- */
static int try_take(Obj *o, uint32_t tid) {   /* G held */
  switch (o->kind) {
    case K_THREAD: return o->done;
    case K_MUTEX: if (o->count == 0 || o->owner == tid) { o->owner = tid; o->count++; return 1; } return 0;
    case K_SEM: if (o->sem > 0) { o->sem--; return 1; } return 0;
  }
  return 0;
}
static void mono_deadline(struct timespec *t, uint32_t ms) { clock_gettime(CLOCK_REALTIME, t); t->tv_sec += ms / 1000; t->tv_nsec += (long)(ms % 1000) * 1000000L; if (t->tv_nsec >= 1000000000L) { t->tv_sec++; t->tv_nsec -= 1000000000L; } }
void aot_wd_wait(uint64_t h, uint32_t ms, int on);
static uint32_t wait_one2(CPU *c, uint64_t h, uint32_t ms);
static uint32_t wait_one(CPU *c, uint64_t h, uint32_t ms) { if (ms) aot_wd_wait(h, ms, 1); uint32_t r = wait_one2(c, h, ms); if (ms) aot_wd_wait(h, ms, 0); return r; }
static uint32_t wait_one2(CPU *c, uint64_t h, uint32_t ms) {
  Obj *o = h_get(h);
  if (!o) { if ((h & H_MASK) == H_BASE) { aot_last_error = 6; return WAIT_FAILED; } return (uint32_t)delegate(c, "WaitForSingleObject", h, ms); }
  uint32_t tid = cur_tid(); struct timespec dl; if (ms != INFINITE_MS) mono_deadline(&dl, ms);
  pthread_mutex_lock(&G);
  for (;;) {
    if (try_take(o, tid)) { pthread_mutex_unlock(&G); return WAIT_OBJECT_0; }
    if (!ms) { pthread_mutex_unlock(&G); return WAIT_TIMEOUT; }
    int e = ms == INFINITE_MS ? pthread_cond_wait(&C, &G) : pthread_cond_timedwait(&C, &G, &dl);
    if (e == ETIMEDOUT) { if (try_take(o, tid)) { pthread_mutex_unlock(&G); return WAIT_OBJECT_0; } pthread_mutex_unlock(&G); return WAIT_TIMEOUT; }
  }
}
SYS(WaitForSingleObject) { return wait_one(c, A(0), (uint32_t)A(1)); }
SYS(WaitForSingleObjectEx) { return wait_one(c, A(0), (uint32_t)A(1)); }
static void give_back(Obj *o) { if (o->kind == K_MUTEX && o->count) { if (!--o->count) o->owner = 0; } else if (o->kind == K_SEM) o->sem++; }
static uint64_t wait_handle_at(uint64_t arr, uint32_t i) {
  if (aot_is_pe32) return aot_x86_alias_in(LD(uint32_t, arr + 4ull * i));
  return LD(uint64_t, arr + 8ull * i);
}
static uint64_t wait_multi(CPU *c, uint32_t n, uint64_t arr, int all, uint32_t ms) {
  if (!n || n > 64) { aot_last_error = 87; return WAIT_FAILED; }
  struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    uint32_t got[64]; uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
      uint64_t h = wait_handle_at(arr, i); uint32_t r = wait_one(c, h, 0);
      if (r == WAIT_OBJECT_0) { got[k++] = i; if (!all) return WAIT_OBJECT_0 + i; }
      else if (r == WAIT_FAILED) return WAIT_FAILED;
      else if (all) { pthread_mutex_lock(&G); for (uint32_t j = 0; j < k; j++) { Obj *o = h_get(wait_handle_at(arr, got[j])); if (o) give_back(o); } pthread_mutex_unlock(&G); k = 0xffffffff; break; }
    }
    if (all && k == n) return WAIT_OBJECT_0;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (ms != INFINITE_MS && (uint64_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000) >= ms) return WAIT_TIMEOUT;
    usleep(300);
  }
}
SYS(WaitForMultipleObjects) { return wait_multi(c, (uint32_t)A(0), A(1), (int)A(2), (uint32_t)A(3)); }
SYS(WaitForMultipleObjectsEx) { return wait_multi(c, (uint32_t)A(0), A(1), (int)A(2), (uint32_t)A(3)); }
SYS(CloseHandle) {
  Obj *o = h_get(A(0));
  if (!o) return (uint64_t)(uint32_t)delegate(c, "CloseHandle", A(0), 0);
  pthread_mutex_lock(&G); if (o->kind == K_THREAD && !o->done) pthread_detach(o->th); o->open = 0; pthread_mutex_unlock(&G); return 1;
}

/* ---- threads ---- */
static void *thread_main(void *arg) {
  Obj *o = arg; tl_tid = o->tid;
  pthread_mutex_lock(&G); while (!o->started) pthread_cond_wait(&C, &G); pthread_mutex_unlock(&G);
  uint32_t code = (uint32_t)aot_thread_run(o->fn, o->param, o->stack, o->tid);
  pthread_mutex_lock(&G); o->exit_code = code; o->done = 1; pthread_cond_broadcast(&C); pthread_mutex_unlock(&G);
  return NULL;
}
SYS(CreateThread) {
  pthread_mutex_lock(&G); Obj *o = obj_new(K_THREAD);
  if (!o) { pthread_mutex_unlock(&G); aot_last_error = 8; return 0; }
  o->fn = A(2); o->param = A(3); o->stack = (size_t)A(1); o->tid = __atomic_add_fetch(&next_tid, 4, __ATOMIC_SEQ_CST); o->started = !(A(4) & 4);
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1u << 20);
  int e = pthread_create(&o->th, &at, thread_main, o); pthread_attr_destroy(&at);
  if (e) { o->open = 0; pthread_mutex_unlock(&G); aot_last_error = 8; return 0; }
  uint64_t h = h_of(o); if (A(5)) ST(uint32_t, A(5), o->tid);
  pthread_mutex_unlock(&G); return h;
}
SYS(ResumeThread) { Obj *o = h_get(A(0)); if (!o || o->kind != K_THREAD) return 0xFFFFFFFFu; pthread_mutex_lock(&G); int was = !o->started; o->started = 1; pthread_cond_broadcast(&C); pthread_mutex_unlock(&G); return was; }
SYS(OpenThread) {
  uint32_t tid = (uint32_t)A(2); uint64_t h = 0;
  pthread_mutex_lock(&G);
  for (int i = 0; i < NOBJ; i++) if (objs[i].open && objs[i].kind == K_THREAD && objs[i].tid == tid) { h = h_of(&objs[i]); break; }
  pthread_mutex_unlock(&G);
  if (!h) aot_last_error = 87;
  return h;
}
SYS(ExitThread) { Obj *main_chk = NULL; (void)main_chk; uint32_t id = cur_tid();
  pthread_mutex_lock(&G);
  for (int i = 0; i < NOBJ; i++) if (objs[i].open && objs[i].kind == K_THREAD && objs[i].tid == id) { objs[i].exit_code = (uint32_t)A(0); objs[i].done = 1; }
  pthread_cond_broadcast(&C); pthread_mutex_unlock(&G); pthread_exit(NULL); return 0; }
SYS(GetExitCodeThread) { Obj *o = h_get(A(0)); if (!o) return 0; pthread_mutex_lock(&G); uint32_t v = o->done ? o->exit_code : 259; pthread_mutex_unlock(&G); if (A(1)) ST(uint32_t, A(1), v); return 1; }
SYS(GetCurrentThreadId) { return cur_tid(); }
SYS(GetThreadId) { Obj *o = h_get(A(0)); return o ? o->tid : cur_tid(); }
SYS(SwitchToThread) { sched_yield(); return 1; }
SYS(SleepEx) { uint32_t ms = (uint32_t)A(0); if (!ms) sched_yield(); else usleep((useconds_t)ms * 1000u); return 0; }
SYS(Sleep) { return sys_SleepEx(c); }
SYS(one) { (void)c; return 1; }
SYS(zero) { (void)c; return 0; }
SYS(DuplicateHandle) {
  if (!A(3)) { aot_last_error = 87; return 0; }
  if (aot_is_pe32) ST(uint32_t, A(3), (uint32_t)aot_x86_alias_out(A(1)));
  else ST(uint64_t, A(3), A(1));
  return 1;
}
SYS(TerminateThread) { Obj *o = h_get(A(0)); if (o) { pthread_mutex_lock(&G); o->done = 1; o->exit_code = (uint32_t)A(1); pthread_cond_broadcast(&C); pthread_mutex_unlock(&G); } return 1; }
SYS(GetProcessAffinityMask) {
  if (aot_is_pe32) { if (A(1)) ST(uint32_t, A(1), 0xff); if (A(2)) ST(uint32_t, A(2), 0xff); }
  else { if (A(1)) ST(uint64_t, A(1), 0xff); if (A(2)) ST(uint64_t, A(2), 0xff); }
  return 1;
}

/* ---- mutex / semaphore ---- */
SYS(CreateMutexA) { pthread_mutex_lock(&G); Obj *o = obj_new(K_MUTEX); uint64_t h = 0; if (o) { if (A(1)) { o->owner = cur_tid(); o->count = 1; } h = h_of(o); } pthread_mutex_unlock(&G); return h; }
SYS(ReleaseMutex) { Obj *o = h_get(A(0)); if (!o || o->kind != K_MUTEX) return 0; pthread_mutex_lock(&G); int ok = o->count && o->owner == cur_tid(); if (ok) { if (!--o->count) o->owner = 0; pthread_cond_broadcast(&C); } pthread_mutex_unlock(&G); if (!ok) aot_last_error = 288; return ok; }
SYS(CreateSemaphoreA) { int32_t init = (int32_t)A(1), mx = (int32_t)A(2); if (mx <= 0 || init < 0 || init > mx) { aot_last_error = 87; return 0; }
  pthread_mutex_lock(&G); Obj *o = obj_new(K_SEM); uint64_t h = 0; if (o) { o->sem = init; o->sem_max = mx; h = h_of(o); } pthread_mutex_unlock(&G); return h; }
SYS(ReleaseSemaphore) { Obj *o = h_get(A(0)); if (!o || o->kind != K_SEM) return 0; int32_t n = (int32_t)A(1); pthread_mutex_lock(&G);
  if (n <= 0 || o->sem + n > o->sem_max) { pthread_mutex_unlock(&G); aot_last_error = 298; return 0; }
  if (A(2)) ST(int32_t, A(2), o->sem); o->sem += n; pthread_cond_broadcast(&C); pthread_mutex_unlock(&G); return 1; }

/* ---- critical sections: PE32 Recursion@8 Owner@12; PE64 Recursion@12 Owner@16 ---- */
static size_t cs_recursion_offset(void) { return aot_is_pe32 ? 8 : 12; }
static size_t cs_owner_offset(void) { return aot_is_pe32 ? 12 : 16; }
static uint64_t cs_owner_load(uint64_t cs) {
  uint64_t p = cs + cs_owner_offset();
  return aot_is_pe32 ? __atomic_load_n((uint32_t *)(uintptr_t)p, __ATOMIC_RELAXED) : __atomic_load_n((uint64_t *)(uintptr_t)p, __ATOMIC_RELAXED);
}
static int cs_owner_claim(uint64_t cs, uint64_t tid) {
  uint64_t p = cs + cs_owner_offset();
  if (aot_is_pe32) { uint32_t z = 0; return __atomic_compare_exchange_n((uint32_t *)(uintptr_t)p, &z, (uint32_t)tid, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED); }
  uint64_t z = 0; return __atomic_compare_exchange_n((uint64_t *)(uintptr_t)p, &z, tid, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static void cs_owner_release(uint64_t cs) {
  uint64_t p = cs + cs_owner_offset();
  if (aot_is_pe32) __atomic_store_n((uint32_t *)(uintptr_t)p, 0, __ATOMIC_RELEASE);
  else __atomic_store_n((uint64_t *)(uintptr_t)p, 0, __ATOMIC_RELEASE);
}
/* Contended critical sections: the old sched_yield x100 + usleep(100) loop cost ~0.1-1 ms per hand-off on Android; the game's allocator hammers a few
 * of them from several threads, which made every frame take seconds. Spin briefly, then sleep on a futex over the owner word and wake on release. */
#include <linux/futex.h>
#include <sys/syscall.h>
#if defined(__aarch64__)
#define CS_RELAX() __asm__ volatile("yield" ::: "memory")
#else
#define CS_RELAX() __asm__ volatile("" ::: "memory")
#endif
/* waiter count lives in the CS's own LockCount field (guest never reads it) so Leave only pays a futex syscall for a lock somebody is really waiting on */
#define CS_WAITERS(cs) ((int32_t *)(uintptr_t)((cs) + (aot_is_pe32 ? 4 : 8)))
static void cs_enter(uint64_t cs) {
  uint64_t tid = cur_tid(), rec = cs_recursion_offset();
  if (cs_owner_load(cs) == tid) { ST(int32_t, cs + rec, LD(int32_t, cs + rec) + 1); return; }
  int got = cs_owner_claim(cs, tid);
  for (int i = 0; !got && i < 400; i++) { CS_RELAX(); got = cs_owner_claim(cs, tid); }
  if (!got) {
    uint32_t *w = (uint32_t *)(uintptr_t)(cs + cs_owner_offset());
    __atomic_add_fetch(CS_WAITERS(cs), 1, __ATOMIC_SEQ_CST);
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!(got = cs_owner_claim(cs, tid))) {
      uint32_t cur = __atomic_load_n(w, __ATOMIC_RELAXED); if (!cur) continue;
      if (cur > (uint32_t)__atomic_load_n(&next_tid, __ATOMIC_RELAXED)) {   /* not a thread id we ever handed out (e.g. 0xCCCCCCCC fill): stale owner, take the lock now */
        if (__atomic_compare_exchange_n(w, &cur, (uint32_t)tid, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) { got = 1; break; } continue; }
      struct timespec tn; clock_gettime(CLOCK_MONOTONIC, &tn);
      if ((tn.tv_sec - t0.tv_sec) * 1000 + (tn.tv_nsec - t0.tv_nsec) / 1000000 > 3000) {
        /* Deadlock breaker: a guest thread has owned this critical section for >3 s while we wait. The owner is almost certainly blocked on an event/query inside the
         * lock (the game never expects that to stall), so let the waiter in; the owner's later Leave just clears the lock. Logged so the cause can be traced. */
        fprintf(stderr, "[aot] critical section %#llx held by thread %u for >3 s while thread %u waits: breaking the lock (possible deadlock)\n", (unsigned long long)cs, cur, (unsigned)tid);
        __atomic_store_n(w, (uint32_t)tid, __ATOMIC_SEQ_CST); got = 1; break; }
      struct timespec ts = { 0, 1000000 };   /* 1 ms safety timeout */
      syscall(SYS_futex, w, FUTEX_WAIT_PRIVATE, cur, &ts, NULL, 0);
    }
    __atomic_sub_fetch(CS_WAITERS(cs), 1, __ATOMIC_SEQ_CST);
  }
  ST(int32_t, cs + rec, 1);
}
static void cs_leave(uint64_t cs) {
  uint64_t rec = cs_recursion_offset(); int32_t r = LD(int32_t, cs + rec) - 1; ST(int32_t, cs + rec, r < 0 ? 0 : r);
  if (r <= 0) { cs_owner_release(cs); if (__atomic_load_n(CS_WAITERS(cs), __ATOMIC_SEQ_CST) > 0) syscall(SYS_futex, (uint32_t *)(uintptr_t)(cs + cs_owner_offset()), FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0); }
}
SYS(InitializeCriticalSection) { memset((void *)(uintptr_t)A(0), 0, aot_is_pe32 ? 24 : 40); return 0; }
SYS(InitializeCriticalSectionAndSpinCount) { memset((void *)(uintptr_t)A(0), 0, aot_is_pe32 ? 24 : 40); return 1; }
SYS(EnterCriticalSection) { if (A(0) == 0x199df80u) aot_heap_check("CS enter"); cs_enter(A(0)); return 0; }
SYS(LeaveCriticalSection) { if (A(0) == 0x199df80u) aot_heap_check("CS leave"); cs_leave(A(0)); return 0; }
SYS(TryEnterCriticalSection) { uint64_t cs = A(0), tid = cur_tid(), rec = cs_recursion_offset();
  if (cs_owner_load(cs) == tid) { ST(int32_t, cs + rec, LD(int32_t, cs + rec) + 1); return 1; }
  if (cs_owner_claim(cs, tid)) { ST(int32_t, cs + rec, 1); return 1; } return 0; }
SYS(DeleteCriticalSection) { return 0; }

/* ---- SRW locks / condition variables / init-once ---- */
SYS(InitializeSRWLock) { ST(uint64_t, A(0), 0); return 0; }
SYS(AcquireSRWLockExclusive) { uint64_t *w = (uint64_t *)(uintptr_t)A(0); for (int s = 0;; s++) { uint64_t z = 0; if (__atomic_compare_exchange_n(w, &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0; if (s < 100) sched_yield(); else usleep(100); } }
SYS(TryAcquireSRWLockExclusive) { uint64_t *w = (uint64_t *)(uintptr_t)A(0), z = 0; return __atomic_compare_exchange_n(w, &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED); }
SYS(ReleaseSRWLockExclusive) { __atomic_store_n((uint64_t *)(uintptr_t)A(0), 0, __ATOMIC_RELEASE); return 0; }
SYS(AcquireSRWLockShared) { uint64_t *w = (uint64_t *)(uintptr_t)A(0); for (int s = 0;; s++) { uint64_t v = __atomic_load_n(w, __ATOMIC_RELAXED); if (!(v & 1) && __atomic_compare_exchange_n(w, &v, v + 2, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0; if (s < 100) sched_yield(); else usleep(100); } }
SYS(TryAcquireSRWLockShared) { uint64_t *w = (uint64_t *)(uintptr_t)A(0); uint64_t v = __atomic_load_n(w, __ATOMIC_RELAXED); return !(v & 1) && __atomic_compare_exchange_n(w, &v, v + 2, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED); }
SYS(ReleaseSRWLockShared) { __atomic_sub_fetch((uint64_t *)(uintptr_t)A(0), 2, __ATOMIC_RELEASE); return 0; }
static uint32_t cv_wait(CPU *c, uint64_t cv, uint32_t ms, int srw, uint64_t lock, uint32_t flags) {
  uint64_t *gen = (uint64_t *)(uintptr_t)cv; struct timespec dl; if (ms != INFINITE_MS) mono_deadline(&dl, ms);
  pthread_mutex_lock(&G); uint64_t g0 = __atomic_load_n(gen, __ATOMIC_SEQ_CST); pthread_mutex_unlock(&G);
  int32_t rec = 0;
  if (srw) { CPU t = *c; t.r[1] = lock; if (flags & 1) sys_ReleaseSRWLockShared(&t); else sys_ReleaseSRWLockExclusive(&t); }
  else { uint64_t ro = cs_recursion_offset(); rec = LD(int32_t, lock + ro); ST(int32_t, lock + ro, 1); cs_leave(lock); }
  uint32_t ok = 1;
  pthread_mutex_lock(&G);
  while (__atomic_load_n(gen, __ATOMIC_SEQ_CST) == g0) {
    int e = ms == INFINITE_MS ? pthread_cond_wait(&C, &G) : pthread_cond_timedwait(&C, &G, &dl);
    if (e == ETIMEDOUT) { ok = __atomic_load_n(gen, __ATOMIC_SEQ_CST) != g0; break; }
  }
  pthread_mutex_unlock(&G);
  if (srw) { CPU t = *c; t.r[1] = lock; if (flags & 1) sys_AcquireSRWLockShared(&t); else sys_AcquireSRWLockExclusive(&t); }
  else { cs_enter(lock); ST(int32_t, lock + cs_recursion_offset(), rec); }
  if (!ok) aot_last_error = 1460;
  return ok;
}
SYS(InitializeConditionVariable) { ST(uint64_t, A(0), 0); return 0; }
SYS(SleepConditionVariableCS) { return cv_wait(c, A(0), (uint32_t)A(2), 0, A(1), 0); }
SYS(SleepConditionVariableSRW) { return cv_wait(c, A(0), (uint32_t)A(2), 1, A(1), (uint32_t)A(3)); }
SYS(WakeConditionVariable) { pthread_mutex_lock(&G); __atomic_add_fetch((uint64_t *)(uintptr_t)A(0), 1, __ATOMIC_SEQ_CST); pthread_cond_broadcast(&C); pthread_mutex_unlock(&G); return 0; }
SYS(InitOnceExecuteOnce) {   /* once word: 0 new, 1 running, 2 done */
  uint32_t *w = (uint32_t *)(uintptr_t)A(0);
  for (;;) {
    pthread_mutex_lock(&G); uint32_t v = *w;
    if (v == 2) { pthread_mutex_unlock(&G); return 1; }
    if (v == 0) { *w = 1; pthread_mutex_unlock(&G); break; }
    pthread_cond_wait(&C, &G); pthread_mutex_unlock(&G);
  }
  uint64_t r = aot_call(c, A(1), 3, A(0), A(2), A(3));
  pthread_mutex_lock(&G); *w = (r & 0xffffffffu) ? 2 : 0; pthread_cond_broadcast(&C); pthread_mutex_unlock(&G); return (r & 0xffffffffu) ? 1 : 0;
}
SYS(InterlockedIncrement) { return (uint32_t)__atomic_add_fetch((int32_t *)(uintptr_t)A(0), 1, __ATOMIC_SEQ_CST); }
SYS(InterlockedDecrement) { return (uint32_t)__atomic_sub_fetch((int32_t *)(uintptr_t)A(0), 1, __ATOMIC_SEQ_CST); }
SYS(InterlockedExchange) { return (uint32_t)__atomic_exchange_n((int32_t *)(uintptr_t)A(0), (int32_t)A(1), __ATOMIC_SEQ_CST); }
SYS(InterlockedExchangeAdd) { return (uint32_t)__atomic_fetch_add((int32_t *)(uintptr_t)A(0), (int32_t)A(1), __ATOMIC_SEQ_CST); }
SYS(InterlockedCompareExchange) { int32_t e = (int32_t)A(2); __atomic_compare_exchange_n((int32_t *)(uintptr_t)A(0), &e, (int32_t)A(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return (uint32_t)e; }
SYS(InterlockedCompareExchange64) { int64_t e = (int64_t)A(2); __atomic_compare_exchange_n((int64_t *)(uintptr_t)A(0), &e, (int64_t)A(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return (uint64_t)e; }

/* ---- per-thread TLS/FLS (slots array lives in the TEB at +0x58; slot 0 = static TLS block) ---- */
static uint64_t tls_used = 1;
SYS(TlsAlloc) { pthread_mutex_lock(&G); for (int i = 1; i < 64; i++) if (!(tls_used >> i & 1)) { tls_used |= 1ull << i; pthread_mutex_unlock(&G); return (uint64_t)i; } pthread_mutex_unlock(&G); aot_last_error = 8; return 0xFFFFFFFFu; }
SYS(TlsFree) { uint64_t i = A(0) & 0xffffffffu; if (i < 1 || i >= 64) return 0; pthread_mutex_lock(&G); tls_used &= ~(1ull << i); pthread_mutex_unlock(&G); return 1; }
SYS(TlsGetValue) {
  uint64_t i = A(0) & 0xffffffffu; if (i >= 64) { aot_last_error = 87; return 0; } aot_last_error = 0;
  if (aot_is_pe32) return LD(uint32_t, LD(uint32_t, c->gs + 0x2c) + 4 * i);
  return LD(uint64_t, LD(uint64_t, c->gs + 0x58) + 8 * i);
}
SYS(TlsSetValue) {
  uint64_t i = A(0) & 0xffffffffu; if (i >= 64) { aot_last_error = 87; return 0; }
  if (aot_is_pe32) ST(uint32_t, LD(uint32_t, c->gs + 0x2c) + 4 * i, (uint32_t)A(1));
  else ST(uint64_t, LD(uint64_t, c->gs + 0x58) + 8 * i, A(1));
  return 1;
}

/* ---- SEH-lite: vectored handlers, unhandled filter, RaiseException, crash diagnostics ---- */
static uint64_t veh[16]; static int nveh; static uint64_t unhandled_filter;
SYS(AddVectoredExceptionHandler) { pthread_mutex_lock(&G); uint64_t h = 0; if (nveh < 16) { if (A(0)) { memmove(veh + 1, veh, sizeof(uint64_t) * nveh); veh[0] = A(1); } else veh[nveh] = A(1); nveh++; h = A(1); } pthread_mutex_unlock(&G); return h; }
SYS(RemoveVectoredExceptionHandler) { pthread_mutex_lock(&G); for (int i = 0; i < nveh; i++) if (veh[i] == A(0)) { memmove(veh + i, veh + i + 1, sizeof(uint64_t) * (nveh - i - 1)); nveh--; break; } pthread_mutex_unlock(&G); return 1; }
SYS(SetUnhandledExceptionFilter) { uint64_t old = unhandled_filter; unhandled_filter = A(0); return old; }
static int dispatch_exception(CPU *c, uint32_t code, uint32_t flags, uint32_t n, uint64_t args) {   /* 1 = continue execution */
  if (n > 15) n = 15;
  size_t rec_size = aot_is_pe32 ? 80 : 160, ctx_size = aot_is_pe32 ? 0x2cc : 1280 + 16;
  uint8_t *rec = aot_gcalloc(1, rec_size), *ctx = aot_gcalloc(1, ctx_size);
  uint8_t *ptrs = aot_gcalloc(2, aot_is_pe32 ? 4 : 8);
  if (!rec || !ctx || !ptrs) { aot_gfree(rec); aot_gfree(ctx); aot_gfree(ptrs); return 0; }
  ST(uint32_t, rec, code); ST(uint32_t, rec + 4, flags);
  if (aot_is_pe32) {
    ST(uint32_t, ptrs, (uint32_t)(uintptr_t)rec); ST(uint32_t, ptrs + 4, (uint32_t)(uintptr_t)ctx);
    ST(uint32_t, rec + 12, (uint32_t)aot_last_pc); ST(uint32_t, rec + 16, n);
    for (uint32_t i = 0; i < n; i++) ST(uint32_t, rec + 20 + 4 * i, LD(uint32_t, args + 4ull * i));
    ST(uint32_t, ctx + 0xb8, (uint32_t)aot_last_pc); ST(uint32_t, ctx + 0xc4, (uint32_t)c->r[4]);
  } else {
    ST(uint64_t, ptrs, (uint64_t)(uintptr_t)rec); ST(uint64_t, ptrs + 8, (uint64_t)(uintptr_t)ctx);
    ST(uint64_t, rec + 16, aot_last_pc); ST(uint32_t, rec + 24, n);
    for (uint32_t i = 0; i < n; i++) ST(uint64_t, rec + 32 + 8 * i, LD(uint64_t, args + 8ull * i));
    ST(uint64_t, ctx + 0x98, c->r[4]); ST(uint64_t, ctx + 0xf8, aot_last_pc);
  }
  int handled = 0;
  for (int i = 0; i < nveh && !handled; i++) if ((int32_t)aot_call(c, veh[i], 1, (uint64_t)(uintptr_t)ptrs) == -1) handled = 1;
  if (!handled && unhandled_filter && (int32_t)aot_call(c, unhandled_filter, 1, (uint64_t)(uintptr_t)ptrs) == -1) handled = 1;
  aot_gfree(rec); aot_gfree(ctx); aot_gfree(ptrs); return handled;
}
SYS(RaiseException) {
  uint32_t code = (uint32_t)A(0), flags = (uint32_t)A(1), n = (uint32_t)A(2);
  /* debugger-only notifications (SetThreadName 0x406D1388, DBG_PRINTEXCEPTION_C 0x40010006): with no debugger attached the guest's own __except swallows them and execution continues after the call */
  if (code == 0x406D1388u || code == 0x40010006u) return 0;
  if (dispatch_exception(c, code, flags, n, A(3))) return 0;
  fflush(stdout); fprintf(stderr, "\n[aot] unhandled exception 0x%08x%s (no vectored/unhandled filter handled it; __try/__except unwinding is not implemented)\n", code, code == 0xE06D7363u ? " (C++ throw)" : "");
  exit((int)code);
}
SYS(RtlUnwind) { return 0; }
SYS(RtlCaptureContext) {
  uint64_t ctx = A(0); if (!ctx) return 0;
  if (aot_is_pe32) { memset((void *)(uintptr_t)ctx, 0, 0x2cc); ST(uint32_t, ctx + 0xb8, (uint32_t)aot_last_pc); ST(uint32_t, ctx + 0xc4, (uint32_t)c->r[4]); }
  else { memset((void *)(uintptr_t)ctx, 0, 1232); ST(uint64_t, ctx + 0x98, c->r[4]); ST(uint64_t, ctx + 0xf8, aot_last_pc); }
  return 0;
}
#if defined(__aarch64__) && defined(__ANDROID__)
#include <dlfcn.h>
#include <ucontext.h>
#include <sys/prctl.h>
static void crash_host_frames(void *uc) {   /* host-side pc/lr/frame-pointer chain: tells a DXVK/driver crash from a guest-code crash */
  mcontext_t *m = &((ucontext_t *)uc)->uc_mcontext; char b[300], tn[20] = { 0 }; prctl(PR_GET_NAME, (unsigned long)tn, 0, 0, 0);
  uint64_t fr[2 + 12]; int nf = 0; fr[nf++] = m->pc; fr[nf++] = m->regs[30]; uint64_t fp = m->regs[29];
  for (int i = 0; i < 12 && fp && !(fp & 7); i++) { uint64_t *p = (uint64_t *)fp; uint64_t nfp = p[0], lr = p[1]; fr[nf++] = lr; if (nfp <= fp) break; fp = nfp; }
  int n = snprintf(b, sizeof b, "[aot] crashing thread \"%s\"; host frames (pc, lr, fp-chain):\n", tn); if (write(2, b, n) < 0) {}
  for (int i = 0; i < nf; i++) { Dl_info d; const char *f = "?", *sy = "?"; uint64_t base = 0; if (dladdr((void *)fr[i], &d)) { f = d.dli_fname ? d.dli_fname : "?"; sy = d.dli_sname ? d.dli_sname : "?"; base = (uint64_t)d.dli_fbase; }
    n = snprintf(b, sizeof b, "  #%d %#llx %s+%#llx (%s)\n", i, (unsigned long long)fr[i], f, (unsigned long long)(fr[i] - base), sy); if (write(2, b, n) < 0) {} }
}
#else
static void crash_host_frames(void *uc) { (void)uc; }
#endif
static void crash_handler(int sig, siginfo_t *si, void *uc) {
  char b[200]; int n = snprintf(b, sizeof b, "\n[aot] fatal signal %d at address %p (last guest block 0x%llx)\n", sig, si->si_addr, (unsigned long long)aot_last_pc);
  if (write(2, b, n) < 0) {}
  { extern void aot_dump_crash_context(void); n = snprintf(b, sizeof b, "[aot] si_code=%d (1=unmapped address, 2=permission)\n", si->si_code); if (write(2, b, n) < 0) {} aot_dump_crash_context(); aot_heap_crash_dump(); crash_host_frames(uc); }
  _exit(139);
}
void aot_sys_init(void) {
  static char altstack[32768]; stack_t ss = { altstack, 0, sizeof altstack }; sigaltstack(&ss, NULL);
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGFPE, &sa, NULL); sigaction(SIGILL, &sa, NULL);
}

const ShimEnt aot_shim_table_sys[] = {
  E(CreateThread), E(OpenThread), E(ResumeThread), E(ExitThread), E(GetExitCodeThread), E(GetCurrentThreadId), E(GetThreadId), E(SwitchToThread), E(Sleep), E(SleepEx), E(TerminateThread),
  EA("SuspendThread", zero), EA("SetThreadPriority", one), EA("GetThreadPriority", zero), EA("SetThreadAffinityMask", one), EA("SetThreadIdealProcessor", zero), EA("SetThreadDescription", zero),
  EA("SetThreadPriorityBoost", one), E(GetProcessAffinityMask), E(DuplicateHandle),
  E(WaitForSingleObject), E(WaitForSingleObjectEx), E(WaitForMultipleObjects), E(WaitForMultipleObjectsEx), E(CloseHandle),
  E(CreateMutexA), EA("CreateMutexW", CreateMutexA), EA("CreateMutexExW", CreateMutexA), E(ReleaseMutex), E(CreateSemaphoreA), EA("CreateSemaphoreW", CreateSemaphoreA), E(ReleaseSemaphore),
  E(InitializeCriticalSection), E(InitializeCriticalSectionAndSpinCount), EA("InitializeCriticalSectionEx", InitializeCriticalSectionAndSpinCount), E(EnterCriticalSection), E(LeaveCriticalSection), E(TryEnterCriticalSection), E(DeleteCriticalSection),
  E(InitializeSRWLock), E(AcquireSRWLockExclusive), E(TryAcquireSRWLockExclusive), E(ReleaseSRWLockExclusive), E(AcquireSRWLockShared), E(TryAcquireSRWLockShared), E(ReleaseSRWLockShared),
  E(InitializeConditionVariable), E(SleepConditionVariableCS), E(SleepConditionVariableSRW), E(WakeConditionVariable), EA("WakeAllConditionVariable", WakeConditionVariable), E(InitOnceExecuteOnce),
  E(InterlockedIncrement), E(InterlockedDecrement), E(InterlockedExchange), E(InterlockedExchangeAdd), E(InterlockedCompareExchange), E(InterlockedCompareExchange64),
  E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue), EA("FlsAlloc", TlsAlloc), EA("FlsFree", TlsFree), EA("FlsGetValue", TlsGetValue), EA("FlsSetValue", TlsSetValue),
  E(AddVectoredExceptionHandler), E(RemoveVectoredExceptionHandler), E(SetUnhandledExceptionFilter), E(RaiseException), E(RtlUnwind), E(RtlCaptureContext),
  { 0, 0 }
};
