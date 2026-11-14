/* Built-in sampling profiler (normal build only; compiled out with -DAOT_RELEASE). Every 2 ms of process CPU time one sample (pc + thread name) is taken;
   every 10 s for the first 60 s a report says which thread / library / function the CPU time went to. Nothing else in the runtime changes. */
#ifndef AOT_RELEASE
#define _GNU_SOURCE
#include "aot_runtime.h"
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#define NS 65536
static struct { uint64_t pc; char th[16]; } ring[NS];
static volatile unsigned head;
static void on_prof(int sig, siginfo_t *si, void *uc) {
  (void)sig; (void)si;
  unsigned h = __atomic_fetch_add(&head, 1, __ATOMIC_RELAXED) & (NS - 1);
#if defined(__aarch64__)
  ring[h].pc = ((ucontext_t *)uc)->uc_mcontext.pc;
#elif defined(__x86_64__)
  ring[h].pc = ((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#else
  ring[h].pc = 0; (void)uc;
#endif
  ring[h].th[0] = 0; prctl(PR_GET_NAME, (unsigned long)ring[h].th, 0, 0, 0);
}
typedef struct { char key[112]; unsigned n; } Ent;
static void bump(Ent *t, int *nt, const char *key) {
  for (int i = 0; i < *nt; i++) if (!strcmp(t[i].key, key)) { t[i].n++; return; }
  if (*nt < 600) { snprintf(t[*nt].key, sizeof t[*nt].key, "%s", key); t[(*nt)++].n = 1; }
}
static int cmp(const void *a, const void *b) { return (int)((const Ent *)b)->n - (int)((const Ent *)a)->n; }
static void label(uint64_t pc, char *out, size_t n) {
  Dl_info d; const char *f = "?", *s = NULL; uint64_t base = 0;
  if (dladdr((void *)pc, &d)) { if (d.dli_fname) { f = strrchr(d.dli_fname, '/'); f = f ? f + 1 : d.dli_fname; } s = d.dli_sname; base = (uint64_t)d.dli_fbase; }
  int k = -1; uintptr_t best = 0;
  for (int m = 0; m < aot_nregions; m++) { uintptr_t fn = (uintptr_t)aot_region_fn[m]; if (fn <= pc && fn > best && pc - fn < 0x40000) { best = fn; k = m; } }
  if (k >= 0 && (!s || strstr(f, "aotgame") || strstr(f, "libaot"))) snprintf(out, n, "guest-code region %#llx-%#llx", (unsigned long long)aot_region_lo[k], (unsigned long long)aot_region_hi[k]);
  else if (s) snprintf(out, n, "%s %s", f, s);
  else snprintf(out, n, "%s +%#llx", f, (unsigned long long)((pc - base) & ~0xFFFull));
}
static void *reporter(void *arg) {
  (void)arg; unsigned last = 0; static Ent tk[600], th[64], tl[600];
  for (int rep = 1; rep <= 6; rep++) {
    sleep(10); unsigned h = head, cnt = h - last; if (cnt > NS) cnt = NS; last = h; int nk = 0, nh = 0; unsigned tot = 0;
    for (unsigned i = 0; i < cnt; i++) { unsigned ix = (h - 1 - i) & (NS - 1); char l[112], key[112]; label(ring[ix].pc, l, sizeof l);
      snprintf(key, sizeof key, "%.14s | %s", ring[ix].th, l); bump(tk, &nk, key); bump(th, &nh, ring[ix].th); tot++; }
    qsort(tk, nk, sizeof tk[0], cmp); qsort(th, nh, sizeof th[0], cmp);
    dprintf(2, "[prof] ---- %ds: %u samples (2 ms of CPU each) ----\n", 10 * rep, tot);
    for (int i = 0; i < nh && i < 8; i++) dprintf(2, "[prof] thread %-15s %5.1f%%\n", th[i].key, 100.0 * th[i].n / (tot ? tot : 1));
    for (int i = 0; i < nk && i < 16; i++) dprintf(2, "[prof] %5.1f%%  %s\n", 100.0 * tk[i].n / (tot ? tot : 1), tk[i].key);
    (void)tl; memset(tk, 0, sizeof tk); memset(th, 0, sizeof th);
  }
  return NULL;
}
__attribute__((constructor)) static void prof_init(void) {
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGPROF, &sa, NULL);
  struct itimerval it = { { 0, 2000 }, { 0, 2000 } }; setitimer(ITIMER_PROF, &it, NULL);
  pthread_t t; if (!pthread_create(&t, NULL, reporter, NULL)) pthread_detach(t);
}
#endif
