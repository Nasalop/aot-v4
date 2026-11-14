/* Release build only (force-included with -include by .github/workflows/release.yml).
   Silences all diagnostic output: fprintf to stderr/stdout becomes a no-op whose arguments are never evaluated.
   The per-lock heap consistency check (aot_heap_check, ~800k calls per session) is compiled out of win_sys.c. */
#ifndef AOT_RELEASE_H
#define AOT_RELEASE_H
#include <stdio.h>
#define fprintf(f, ...) ((((f) == stderr) || ((f) == stdout)) ? 0 : fprintf((f), __VA_ARGS__))
#ifdef AOT_NO_HEAPCHECK
#include "aot_runtime.h"
#define aot_heap_check(w) ((void)0)
#endif
#endif
