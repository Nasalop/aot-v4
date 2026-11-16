/* Release build only (force-included with -include by .github/workflows/release.yml).
   Silences per-call diagnostic output (call traces, statistics) but keeps what is needed to understand a failure: fatal reports, start-up facts
   (CPU placement, which DXVK tier was chosen, graphics set-up) and every error line. aot_keep() decides from the format string and bounds each
   message to a few dozen prints, so a message in a loop can never flood the log. Dropped messages never evaluate their arguments.
   The per-lock heap consistency check (aot_heap_check, ~800k calls per session) is compiled out of win_sys.c. */
#ifndef AOT_RELEASE_H
#define AOT_RELEASE_H
#include <stdio.h>
#ifdef __cplusplus
extern "C" int aot_keep(const char *fmt);   /* runtime.c; the aotgame target also force-includes this header into its C++ files */
#else
int aot_keep(const char *fmt);   /* runtime.c */
#endif
#define AOT_FMT0(a, ...) a
#define fprintf(f, ...) ((((f) != stderr) && ((f) != stdout)) || aot_keep(AOT_FMT0(__VA_ARGS__, 0)) ? fprintf((f), __VA_ARGS__) : 0)
#ifdef AOT_NO_HEAPCHECK
#include "aot_runtime.h"
#define aot_heap_check(w) ((void)0)
#endif
#endif
