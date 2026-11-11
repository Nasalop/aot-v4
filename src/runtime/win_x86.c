/* 32-bit (PE32) support: low-4GB allocator, verified stdcall/cdecl import table, and the 32-bit -> shim call adapter.
 * Only imports listed in aot_x86_apis are bound; everything else stays unresolved and aborts loudly when called. */
#define _GNU_SOURCE
#include "shims.h"
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>
#include <strings.h>
#include <sched.h>

/* name table (parsed by src/tools/pe_dependency_inspect.py): dll, symbol, bytes popped by callee (stdcall; 0 = cdecl), fp = double args/return */
shim_fn aot_x86_dll_resolve(const char *dll, const char *name, int *pop);   /* win_x86_dll.c */
typedef struct { const char *dll, *name; int pop, fp; shim_fn fn; } X86Api;
#define SX(n) static uint64_t x86_##n(CPU *c)
SX(InterlockedIncrement) { return __atomic_add_fetch((int32_t *)(uintptr_t)(uint32_t)c->r[1], 1, 5); }
SX(InterlockedDecrement) { return __atomic_sub_fetch((int32_t *)(uintptr_t)(uint32_t)c->r[1], 1, 5); }
SX(InterlockedExchange) { return __atomic_exchange_n((int32_t *)(uintptr_t)(uint32_t)c->r[1], (int32_t)c->r[2], 5); }
SX(InterlockedExchangeAdd) { return __atomic_fetch_add((int32_t *)(uintptr_t)(uint32_t)c->r[1], (int32_t)c->r[2], 5); }
SX(SwitchToThread) { (void)c; return sched_yield() == 0; }
SX(OutputDebugStringA) { fflush(stdout); fprintf(stderr, "[debug] %s", (const char *)(uintptr_t)(uint32_t)c->r[1]); return 0; }
SX(LocalFree) { aot_gfree((void *)(uintptr_t)(uint32_t)c->r[1]); return 0; }
#define K(n, b) { "kernel32.dll", #n, b, 0, NULL }
#define KL(n, b) { "kernel32.dll", #n, b, 0, x86_##n }
#define D(dll, n, b) { dll, #n, b, 0, NULL }
#define C(n) { "msvcrt.dll", #n, 0, 0, NULL }
#define CF(n, nd) { "msvcrt.dll", #n, 0, nd, NULL }
/* Fiber-local storage maps onto the thread-local-storage shims (the FLS destructor callback is never run) */
static uint64_t tls_by_name(CPU *c, const char *n) { shim_fn f = aot_resolve_import("kernel32.dll", n); return f ? f(c) : 0; }
static uint64_t x86_FlsAlloc(CPU *c) { return tls_by_name(c, "TlsAlloc"); }
static uint64_t x86_FlsGetValue(CPU *c) { return tls_by_name(c, "TlsGetValue"); }
static uint64_t x86_FlsSetValue(CPU *c) { return tls_by_name(c, "TlsSetValue"); }
static uint64_t x86_FlsFree(CPU *c) { return tls_by_name(c, "TlsFree"); }
static const X86Api aot_x86_apis[] = {
  K(GetStdHandle, 4), K(WriteFile, 20), K(ReadFile, 20), K(CloseHandle, 4), K(ExitProcess, 4), K(GetTickCount, 0), K(Sleep, 4),
  K(GetLastError, 0), K(SetLastError, 4), K(QueryPerformanceCounter, 4), K(QueryPerformanceFrequency, 4),
  K(GetCurrentThreadId, 0), K(GetCurrentProcessId, 0), K(TlsAlloc, 0), K(TlsGetValue, 4), K(TlsSetValue, 8), K(TlsFree, 4), KL(FlsAlloc, 4), KL(FlsGetValue, 4), KL(FlsSetValue, 8), KL(FlsFree, 4),
  K(InitializeCriticalSection, 4), K(EnterCriticalSection, 4), K(LeaveCriticalSection, 4), K(DeleteCriticalSection, 4),
  K(GetProcessHeap, 0), K(HeapAlloc, 12), K(HeapFree, 12), K(HeapReAlloc, 16), K(VirtualAlloc, 16), K(VirtualFree, 12),
  K(CreateFileA, 28), K(SetFilePointer, 16), K(GetFileSize, 8), K(GetModuleHandleA, 4),
  C(malloc), C(calloc), C(realloc), C(free), C(memcpy), C(memmove), C(memset), C(memcmp), C(strlen), C(strcmp), C(strncmp), C(strcpy), C(strcat), C(exit),
  CF(sqrt, 1), CF(sin, 1), CF(cos, 1), CF(tan, 1), CF(atan, 1), CF(exp, 1), CF(log, 1), CF(floor, 1), CF(ceil, 1), CF(fabs, 1), CF(atan2, 2), CF(pow, 2), CF(fmod, 2),
  /* batch 2: scalar / low-pointer APIs whose existing shims are layout-neutral (no pointer-bearing structs, no 64-bit-by-value args) */
  K(GetCurrentProcess, 0), K(GetCurrentThread, 0), K(CreateEventA, 16), K(SetEvent, 4), K(ResetEvent, 4), K(WaitForSingleObject, 8), K(FlushFileBuffers, 4),
  K(GetFileType, 4), K(GetFileSizeEx, 8), K(TryEnterCriticalSection, 4), K(InitializeCriticalSectionAndSpinCount, 8), K(IsDebuggerPresent, 0), K(SetErrorMode, 4),
  K(SetUnhandledExceptionFilter, 4), K(UnhandledExceptionFilter, 4), K(SetConsoleCtrlHandler, 8), K(GetACP, 0), K(GetConsoleMode, 8), K(HeapSize, 12),
  K(GetCurrentDirectoryA, 8), K(CreateDirectoryA, 8), K(DeleteFileA, 4), K(GetFileAttributesA, 4), K(GetModuleFileNameA, 12), K(GetSystemTimeAsFileTime, 4),
  K(WriteConsoleA, 20), K(FindFirstFileA, 8), K(FindNextFileA, 8), K(FindClose, 4),
  KL(InterlockedIncrement, 4), KL(InterlockedDecrement, 4), KL(InterlockedExchange, 8), KL(InterlockedExchangeAdd, 8), KL(SwitchToThread, 0), KL(OutputDebugStringA, 4), KL(LocalFree, 4),
  /* compatibility report: process, environment, console and locale */
  K(DebugBreak, 0), K(GetCommandLineA, 0), K(GetConsoleCP, 0), K(GetConsoleOutputCP, 0), K(GetEnvironmentStrings, 0), K(GetEnvironmentStringsW, 0), K(GetOEMCP, 0), K(GetUserDefaultLCID, 0), K(GetVersion, 0),
  K(FreeEnvironmentStringsA, 4), K(FreeEnvironmentStringsW, 4), K(FreeLibrary, 4), K(GetDriveTypeA, 4), K(GetLocalTime, 4), K(GetStartupInfoA, 4), K(GetSystemInfo, 4), K(GetSystemTime, 4), K(GetTimeZoneInformation, 4), K(GetVersionExA, 4), K(HeapDestroy, 4), K(IsValidCodePage, 4), K(LoadLibraryA, 4), K(PulseEvent, 4), K(RemoveDirectoryA, 4), K(SetCurrentDirectoryA, 4), K(SetEndOfFile, 4), K(SetHandleCount, 4),
  K(EnumSystemLocalesA, 8), K(FatalAppExitA, 8), K(FileTimeToLocalFileTime, 8), K(FileTimeToSystemTime, 8), K(GetCPInfo, 8), K(GetLogicalDriveStringsA, 8), K(GetTempPathA, 8), K(IsValidLocale, 8), K(GetProcAddress, 8), K(LocalFileTimeToFileTime, 8), K(MoveFileA, 8), K(SetEnvironmentVariableA, 8), K(SetFileAttributesA, 8), K(SetStdHandle, 8), K(SetThreadAffinityMask, 8), K(SetThreadPriority, 8), K(SetThreadPriorityBoost, 8), K(SystemTimeToFileTime, 8), K(TerminateProcess, 8),
  K(CopyFileA, 12), K(GetComputerNameExA, 12), K(GetPrivateProfileSectionNamesA, 12), K(GetProcessAffinityMask, 12), K(HeapCreate, 12), K(OpenThread, 12), K(ReleaseSemaphore, 12), K(VirtualQuery, 12),
  K(GetFileTime, 16), K(GetFullPathNameA, 16), K(GetStringTypeW, 16), K(GetLocaleInfoA, 16), K(GetLocaleInfoW, 16), K(GetOverlappedResult, 16), K(GetPrivateProfileIntA, 16), K(GetPrivateProfileSectionA, 16), K(RaiseException, 16), K(RtlUnwind, 16), K(SetFileTime, 16), K(WaitForMultipleObjects, 16), K(WritePrivateProfileStringA, 16),
  K(GetProcessTimes, 20), K(GetStringTypeA, 20), K(LockFile, 20), K(UnlockFile, 20), K(WriteConsoleW, 20),
  K(CompareStringA, 24), K(CompareStringW, 24), K(CreateThread, 24), K(GetDateFormatA, 24), K(GetPrivateProfileStringA, 24), K(GetTimeFormatA, 24), K(LCMapStringA, 24), K(LCMapStringW, 24), K(LockFileEx, 24), K(MultiByteToWideChar, 24),
  K(DuplicateHandle, 28), K(FormatMessageA, 28), K(WideCharToMultiByte, 32),
  /* compatibility report: threads and synchronization */
  K(CreateSemaphoreA, 16), K(ExitThread, 4), K(GetExitCodeThread, 8), K(ResumeThread, 4), K(SuspendThread, 4),
  /* Direct3D 10 / DXGI entry points (COM methods get their stdcall sizes in win_gfx.c) */
  D("d3d10.dll", D3D10CreateDevice, 24), D("d3d10.dll", D3D10CreateDeviceAndSwapChain, 32), D("d3d10.dll", D3D10CompileShader, 40), D("d3d10.dll", D3D10ReflectShader, 12), D("dxgi.dll", CreateDXGIFactory, 8), D("dxgi.dll", CreateDXGIFactory1, 8),
  { NULL, NULL, 0, 0, NULL }
};

/* resolves a PE32 import: must be in the verified table AND have a runtime shim; otherwise NULL (never a silent stub) */
shim_fn aot_x86_resolve(const char *dll, const char *name, int *pop, int *fp) {
  for (const X86Api *a = aot_x86_apis; a->name; a++)
    if (!strcasecmp(a->dll, dll) && !strcmp(a->name, name)) { *pop = a->pop; *fp = a->fp; return a->fn ? a->fn : aot_resolve_import(dll, name); }
  { int p; shim_fn f = aot_x86_dll_resolve(dll, name, &p); if (f) { *pop = p; *fp = 0; return f; } }
  return NULL;
}

/* ---- low (<4GB) memory: guest pointers must fit in 32 bits ---- */
static uint8_t *low_cur, *low_end; static pthread_mutex_t low_mu = PTHREAD_MUTEX_INITIALIZER, map_mu = PTHREAD_MUTEX_INITIALIZER;
#define NCLS 40
static void *freel[NCLS];
void *aot_low_map(size_t n, uint64_t hint) {
  n = (n + 0xFFF) & ~0xFFFULL; static uint64_t next = 0x20000000ULL; pthread_mutex_lock(&map_mu); void *p = MAP_FAILED;
  if (hint) p = mmap((void *)hint, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  else {   /* first-fit scan of the 32-bit space: occupied ranges are skipped (never relocated above 4 GB), freed holes are found again after wrap-around */
    const uint64_t lo = 0x10000000ULL, hi = 0xF0000000ULL; uint64_t h = next < lo || next + n > hi ? lo : next, start = h; int wrapped = 0;
    for (;;) {
      if (h + n > hi) { if (wrapped) break; wrapped = 1; h = lo; }
      if (wrapped && h >= start) break;
      p = mmap((void *)h, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
      if (p == (void *)h) { next = h + ((n + 0xFFFFULL) & ~0xFFFFULL); break; }
      if (p != MAP_FAILED) { munmap(p, n); p = MAP_FAILED; }   /* kernel without FIXED_NOREPLACE ignored the address */
      h += (n + 0xFFFFULL) & ~0xFFFFULL; if (h < next && !wrapped) h = next;
    }
  }
  pthread_mutex_unlock(&map_mu);
  if (p == MAP_FAILED || (uint64_t)p + n > 0x100000000ULL) { if (p != MAP_FAILED) munmap(p, n); return NULL; }
  return p;
}
static int cls(size_t n) { int k = 5; while (((size_t)1 << k) < n + 16) k++; return k; }
void *aot_gmalloc(size_t n) {
  if (!aot_is_pe32) return malloc(n);
  int k = cls(n ? n : 1); pthread_mutex_lock(&low_mu); uint64_t *b = freel[k];
  if (b) freel[k] = (void *)b[1];
  else {
    size_t sz = (size_t)1 << k;
    if (!low_cur || low_cur + sz > low_end) { size_t chunk = sz > (64u << 20) ? sz : (64u << 20); uint8_t *m = NULL;
      for (; chunk >= sz; chunk = chunk > sz * 2 ? chunk / 4 > sz ? chunk / 4 : sz : sz / 2) { m = aot_low_map(chunk, 0); if (m || chunk == sz) break; }   /* a fragmented 4 GB space may have no 64 MB hole left: fall back to smaller heap chunks */
      if (!m) { static int note; if (!note++) fprintf(stderr, "[aot] low (32-bit) address space exhausted: cannot map %zu bytes for the guest heap\n", sz); pthread_mutex_unlock(&low_mu); return NULL; } low_cur = m; low_end = m + chunk; }
    b = (uint64_t *)low_cur; low_cur += sz;
  }
  pthread_mutex_unlock(&low_mu); b[0] = (uint64_t)k; return b + 2;
}
void aot_gfree(void *p) {
  if (!aot_is_pe32) { free(p); return; }
  if (!p) return; uint64_t *b = (uint64_t *)p - 2; int k = (int)b[0]; pthread_mutex_lock(&low_mu); b[1] = (uint64_t)freel[k]; freel[k] = b; pthread_mutex_unlock(&low_mu);
}
void *aot_gcalloc(size_t a, size_t b) { if (!aot_is_pe32) return calloc(a, b); void *p = aot_gmalloc(a * b); if (p) memset(p, 0, a * b); return p; }
void *aot_grealloc(void *p, size_t n) {
  if (!aot_is_pe32) return realloc(p, n); if (!p) return aot_gmalloc(n);
  size_t old = ((size_t)1 << ((uint64_t *)p)[-2]) - 16; if (n <= old) return p;
  void *q = aot_gmalloc(n); if (q) { memcpy(q, p, old); aot_gfree(p); } return q;
}
void *aot_map_anon(size_t n) {   /* VirtualAlloc backing */
  if (!aot_is_pe32) { void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); return p == MAP_FAILED ? NULL : p; }
  return aot_low_map(n, 0);
}

/* ---- 32-bit handle aliases: shims hand out 64-bit tagged handles (0xA07x...), guests only hold 32 bits ---- */
#define ALIAS_BASE 0xA0000000u
#define ALIAS_MAX 4096
static uint64_t alias_tab[ALIAS_MAX]; static int alias_n; static pthread_mutex_t alias_mu = PTHREAD_MUTEX_INITIALIZER;
uint64_t aot_x86_alias_in(uint64_t v) {   /* guest arg -> real handle (identity if not an alias) */
  if (v >= ALIAS_BASE && v < ALIAS_BASE + 4u * ALIAS_MAX && !(v & 3)) { uint64_t r = alias_tab[(v - ALIAS_BASE) >> 2]; if (r) return r; }
  return v;
}
uint64_t aot_x86_alias_out(uint64_t r) {  /* shim result -> 32-bit value */
  /* Opaque runtime module handles are host pointers too; alias those just like
     tagged kernel handles.  Negative sentinels and ordinary 32-bit values pass through. */
  if (r <= 0xffffffffu || r >= 0xffff800000000000ULL) return r;
  pthread_mutex_lock(&alias_mu);
  for (int i = 0; i < alias_n; i++) if (alias_tab[i] == r) { pthread_mutex_unlock(&alias_mu); return ALIAS_BASE + 4u * (uint32_t)i; }
  if (alias_n >= ALIAS_MAX) { pthread_mutex_unlock(&alias_mu); fprintf(stderr, "[aot] out of 32-bit handle aliases\n"); exit(74); }
  alias_tab[alias_n] = r; uint32_t a = ALIAS_BASE + 4u * (uint32_t)alias_n++; pthread_mutex_unlock(&alias_mu); return a;
}

/* ---- call adapter: guest cdecl/stdcall frame -> Win64-style shim ---- */
__thread uint32_t aot_x86_gesp;   /* guest esp while a shim runs (c->r[4] points at the host arg frame then); aot_call nests below it */
uint64_t aot_x86_thunk(CPU *c, shim_fn fn, int pop, int fp) {
  uint32_t esp = (uint32_t)c->r[4], prev_gesp = aot_x86_gesp; aot_x86_gesp = esp; uint64_t a[12], sc[16] = { 0 };
  for (int n = 0; n < 12; n++) a[n] = aot_x86_alias_in(LD(uint32_t, esp + 4 + 4 * n));
  for (int n = 4; n < 12; n++) sc[1 + n] = a[n];
  c->r[1] = a[0]; c->r[2] = a[1]; c->r[8] = a[2]; c->r[9] = a[3]; c->r[4] = (uint64_t)(uintptr_t)sc;
  if (fp) { c->x[0].g[0] = LD(double, esp + 4); c->x[1].g[0] = LD(double, esp + 12); }
  uint64_t r = fn(c);
  aot_x86_gesp = prev_gesp;
  uint32_t ret = LD(uint32_t, esp);
  r = aot_x86_alias_out(r); c->r[0] = (uint32_t)r; c->r[2] = (uint32_t)(r >> 32);
  if (fp) FPUSH(c->x[0].g[0]);
  c->r[4] = (uint32_t)(esp + 4 + pop);
  return ret;
}
