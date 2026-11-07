/* Additional Win32 compatibility APIs shared by PE32 and PE64 guests. */
#define _GNU_SOURCE
#include "shims.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <strings.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define A(n) argn(c, (n))
static inline uint64_t argn(CPU *c, int n) { return n == 0 ? c->r[1] : n == 1 ? c->r[2] : n == 2 ? c->r[8] : n == 3 ? c->r[9] : LD(uint64_t, c->r[4] + 8 + 8 * n); }
#define SH(n) static uint64_t sh_##n(CPU *c)
#define S(n) { #n, sh_##n }
#define SA(n, f) { n, sh_##f }
#define STR(p) ((const char *)(uintptr_t)(p))
#define WSTR(p) ((const uint16_t *)(uintptr_t)(p))
#define STP(a, v) do { uintptr_t _p = (uintptr_t)(v); if (aot_is_pe32) ST(uint32_t, (a), _p); else ST(uint64_t, (a), _p); } while (0)
#define EPOCH_DELTA 11644473600LL
#define FTICKS 10000000ULL
#define S_OK 0u
#define E_NOTIMPL 0x80004001u

static char *bytes_dup(const char *s, int n) {
  if (!s) return NULL;
  if (n < 0) n = (int)strlen(s) + 1;
  char *p = aot_gmalloc((size_t)n + 1);
  if (!p) { aot_last_error = 8; return NULL; }
  memcpy(p, s, (size_t)n); p[n] = 0; return p;
}

static size_t wlen(const uint16_t *s) { size_t n = 0; if (s) while (s[n]) n++; return n; }
static uint16_t wlower(uint16_t v) { return v < 128 ? (uint16_t)tolower(v) : (uint16_t)towlower((wint_t)v); }
static uint16_t wupper(uint16_t v) { return v < 128 ? (uint16_t)toupper(v) : (uint16_t)towupper((wint_t)v); }

static int put_ansi(uint64_t out, uint32_t cap, const char *s) {
  size_t n = strlen(s) + 1;
  if (!cap) return (int)n;
  if (!out || cap < n) { if (out && cap) ((char *)(uintptr_t)out)[0] = 0; aot_last_error = 122; return 0; }
  memcpy((void *)(uintptr_t)out, s, n); return (int)n;
}

static int put_wide(uint64_t out, uint32_t cap, const uint16_t *s, size_t count) {
  if (!cap) return (int)count;
  if (!out || cap < count) { if (out && cap) * (uint16_t *)(uintptr_t)out = 0; aot_last_error = 122; return 0; }
  memcpy((void *)(uintptr_t)out, s, count * 2); return (int)count;
}

/* ---- string comparison / mapping / character types ---- */
SH(CompareStringA) {
  int n1 = (int)A(3), n2 = (int)A(5); char *a = bytes_dup(STR(A(2)), n1), *b = bytes_dup(STR(A(4)), n2);
  if (!a || !b) { aot_gfree(a); aot_gfree(b); return 0; }
  int insensitive = A(1) & 1; size_t l1 = strlen(a), l2 = strlen(b);
  int r = insensitive ? strcasecmp(a, b) : strcmp(a, b);
  if (n1 >= 0 || n2 >= 0) { size_t n = l1 < l2 ? l1 : l2; r = insensitive ? strncasecmp(a, b, n) : strncmp(a, b, n); if (!r) r = (l1 > l2) - (l1 < l2); }
  aot_gfree(a); aot_gfree(b); return r < 0 ? 1 : r > 0 ? 3 : 2;
}
SH(CompareStringW) {
  const uint16_t *a = WSTR(A(2)), *b = WSTR(A(4)); int na = (int)A(3), nb = (int)A(5);
  if (!a || !b || na < -1 || nb < -1) { aot_last_error = 87; return 0; }
  size_t la = na < 0 ? wlen(a) + 1 : (size_t)na, lb = nb < 0 ? wlen(b) + 1 : (size_t)nb;
  size_t n = la < lb ? la : lb; int r = 0, insensitive = A(1) & 1;
  for (size_t i = 0; i < n && !r; i++) { uint16_t x = insensitive ? wlower(a[i]) : a[i], y = insensitive ? wlower(b[i]) : b[i]; r = (x > y) - (x < y); }
  if (!r) r = (la > lb) - (la < lb); return r < 0 ? 1 : r > 0 ? 3 : 2;
}
static uint64_t lcmap_a(CPU *c, int wide) {
  int n = (int)A(3), upper = A(1) & 0x200, lower = A(1) & 0x100;
  if (n == 0 || n < -1) { aot_last_error = 87; return 0; }
  if (wide) {
    const uint16_t *s = WSTR(A(2)); if (!s) { aot_last_error = 87; return 0; }
    size_t count = n < 0 ? wlen(s) + 1 : (size_t)n;
    if (A(5)) { if ((size_t)A(5) < count || !A(4)) { aot_last_error = 122; return 0; } for (size_t i = 0; i < count; i++) ST(uint16_t, A(4) + 2 * i, upper ? wupper(s[i]) : lower ? wlower(s[i]) : s[i]); }
    return count;
  }
  const char *s = STR(A(2)); if (!s) { aot_last_error = 87; return 0; }
  size_t count = n < 0 ? strlen(s) + 1 : (size_t)n;
  if (A(5)) { if ((size_t)A(5) < count || !A(4)) { aot_last_error = 122; return 0; } for (size_t i = 0; i < count; i++) ((char *)(uintptr_t)A(4))[i] = upper ? (char)toupper((unsigned char)s[i]) : lower ? (char)tolower((unsigned char)s[i]) : s[i]; }
  return count;
}
SH(LCMapStringA) { return lcmap_a(c, 0); }
SH(LCMapStringW) { return lcmap_a(c, 1); }
static uint16_t ctype1(unsigned int v) {
  uint16_t r = 0; if (isalpha(v)) r |= 0x0100 | (isupper(v) ? 1 : 2); if (isdigit(v)) r |= 4; if (isxdigit(v)) r |= 0x80;
  if (isspace(v)) r |= 8; if (ispunct(v)) r |= 0x10; if (iscntrl(v)) r |= 0x20; if (v >= 32 && v < 127) r |= 0x40; return r;
}
SH(GetStringTypeA) {
  const unsigned char *s = (const unsigned char *)(uintptr_t)A(2); int n = (int)A(3); if (!s || !A(4) || A(1) != 1 || !n) { aot_last_error = 87; return 0; }
  if (n < 0) n = (int)strlen((const char *)s) + 1; for (int i = 0; i < n; i++) ST(uint16_t, A(4) + 2 * i, ctype1(s[i])); return 1;
}
SH(GetStringTypeW) {
  const uint16_t *s = WSTR(A(1)); int n = (int)A(2); if (!s || !A(3) || A(0) != 1 || !n) { aot_last_error = 87; return 0; }
  if (n < 0) n = (int)wlen(s) + 1; for (int i = 0; i < n; i++) ST(uint16_t, A(3) + 2 * i, s[i] < 128 ? ctype1(s[i]) : (uint16_t)0x0100); return 1;
}

/* ---- UTF conversion and console ---- */
static int append_cp(uint32_t cp, uint32_t page, char *out, size_t cap, size_t *used) {
  uint8_t b[4]; size_t n;
  if (page != 65001 || cp < 128) { b[0] = cp <= 255 ? (uint8_t)cp : '?'; n = 1; }
  else if (cp < 0x800) { b[0] = 0xc0 | (cp >> 6); b[1] = 0x80 | (cp & 63); n = 2; }
  else if (cp < 0x10000) { b[0] = 0xe0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 63); b[2] = 0x80 | (cp & 63); n = 3; }
  else { b[0] = 0xf0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 63); b[2] = 0x80 | ((cp >> 6) & 63); b[3] = 0x80 | (cp & 63); n = 4; }
  if (out) { if (*used + n > cap) return -1; memcpy(out + *used, b, n); } *used += n; return 0;
}
SH(WideCharToMultiByte) {
  uint32_t page = (uint32_t)A(0); const uint16_t *s = WSTR(A(2)); int count = (int)A(3); char *out = (char *)(uintptr_t)A(4); size_t cap = A(5);
  if (!s || count < -1 || !count || (page != 65001 && page != 0 && page != 1 && page != 1252 && page != 437)) { aot_last_error = 87; return 0; }
  if (count < 0) count = (int)wlen(s) + 1; size_t used = 0;
  for (int i = 0; i < count; i++) {
    uint32_t cp = s[i]; if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < count && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff) cp = 0x10000 + ((cp - 0xd800) << 10) + (s[++i] - 0xdc00);
    if (append_cp(cp, page, out, cap, &used)) { aot_last_error = 122; return 0; }
  }
  if (A(7)) ((uint8_t *)(uintptr_t)A(7))[0] = 0; return used;
}
SH(WriteConsoleW) {
  const uint16_t *s = WSTR(A(1)); uint32_t n = (uint32_t)A(2); if (A(3)) ST(uint32_t, A(3), 0); if (!s && n) { aot_last_error = 87; return 0; }
  size_t cap = (size_t)n * 4 + 1, used = 0; char *tmp = aot_gmalloc(cap); if (!tmp) { aot_last_error = 8; return 0; }
  for (uint32_t i = 0; i < n; i++) { uint32_t cp = s[i]; if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff) cp = 0x10000 + ((cp - 0xd800) << 10) + (s[++i] - 0xdc00); append_cp(cp, 65001, tmp, cap, &used); }
  ssize_t wr = write(1, tmp, used); aot_gfree(tmp); if (wr < 0) return 0; if (A(3)) ST(uint32_t, A(3), n); return 1;
}
SH(GetConsoleCP) { (void)c; return 65001; }
SH(GetConsoleOutputCP) { (void)c; return 65001; }
SH(GetOEMCP) { (void)c; return 437; }
SH(GetCPInfo) { if (!A(1)) { aot_last_error = 87; return 0; } memset((void *)(uintptr_t)A(1), 0, 20); ST(uint32_t, A(1), (uint32_t)A(0) == 65001 ? 4 : 1); ST(uint8_t, A(1) + 4, '?'); return 1; }
SH(IsValidCodePage) { uint32_t p = (uint32_t)A(0); return p == 0 || p == 1 || p == 437 || p == 850 || p == 1252 || p == 65001; }

/* ---- environment / process / version ---- */
extern char **environ;
SH(GetEnvironmentStrings) {
  size_t size = 1; for (char **e = environ; e && *e; e++) size += strlen(*e) + 1;
  char *block = aot_gmalloc(size), *p = block; if (!block) { aot_last_error = 8; return 0; }
  for (char **e = environ; e && *e; e++) { size_t n = strlen(*e) + 1; memcpy(p, *e, n); p += n; } *p = 0; return (uint64_t)(uintptr_t)block;
}
SH(FreeEnvironmentStringsA) { aot_gfree((void *)(uintptr_t)A(0)); return 1; }
SH(FreeLibrary) { (void)c; return 1; } /* modules are process-lifetime runtime tables */
SH(SetEnvironmentVariableA) {
  const char *name = STR(A(0)), *value = STR(A(1)); if (!name || !*name || strchr(name, '=')) { aot_last_error = 87; return 0; }
  int r = value ? setenv(name, value, 1) : unsetenv(name); if (r) aot_last_error = 8; return r == 0;
}
SH(GetComputerNameExA) {
  char name[256] = "aot"; if ((uint32_t)A(0) <= 7) gethostname(name, sizeof name); name[255] = 0; size_t need = strlen(name) + 1, cap = A(2) ? LD(uint32_t, A(2)) : 0;
  if (A(2)) ST(uint32_t, A(2), (uint32_t)need); if (!A(1) || cap < need) { aot_last_error = 122; return 0; } memcpy((void *)(uintptr_t)A(1), name, need); return 1;
}
SH(GetVersion) { (void)c; return 0x1db10106u; }
SH(GetVersionExA) {
  uint64_t p = A(0); if (!p || LD(uint32_t, p) < 148) { aot_last_error = 87; return 0; }
  ST(uint32_t, p + 4, 6); ST(uint32_t, p + 8, 1); ST(uint32_t, p + 12, 7600); ST(uint32_t, p + 16, 2); memset((void *)(uintptr_t)(p + 20), 0, 128); strcpy((char *)(uintptr_t)(p + 20), "Compatibility Service Pack 1"); return 1;
}
SH(GetUserDefaultLCID) { (void)c; return 0x0409; }
SH(IsValidLocale) { uint32_t l = (uint32_t)A(0); return l == 0x0409 || l == 0x007f || l == 0x0400 || l == 0x0800; }
SH(TerminateProcess) { fflush(stdout); exit((int)A(1)); }
SH(DebugBreak) { raise(SIGTRAP); return 0; }
SH(FatalAppExitA) { fflush(stdout); fprintf(stderr, "[fatal] %s: %s\n", A(0) ? STR(A(0)) : "application", A(1) ? STR(A(1)) : "fatal error"); exit(1); }
SH(HeapCreate) { (void)c; return 0x7001; }
SH(HeapDestroy) { return A(0) == 0x7001; }
SH(SetHandleCount) { return A(0); }
SH(GetLogicalDriveStringsA) {
  static const char drives[] = "C:\\\0"; size_t need = sizeof drives;
  if (A(1) && A(0) >= need) { memcpy((void *)(uintptr_t)A(1), drives, need); return need - 1; } return need;
}
SH(GetDriveTypeA) { return !A(0) || !*STR(A(0)) ? 1 : 3; }
SH(GetTempPathA) {
  char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof cwd)) return 0; strcat(cwd, "/"); size_t need = strlen(cwd) + 1;
  if (A(1) && A(0) >= need) { memcpy((void *)(uintptr_t)A(1), cwd, need); return need - 1; } return need;
}

/* ---- locale information / date and time formatting ---- */
static const char *locale_text(uint32_t type) {
  switch (type & 0xffff) {
    case 0x0002: case 0x1002: return "United States"; case 0x0003: case 0x1001: return "English";
    case 0x0004: return "USA"; case 0x0005: return "ENG"; case 0x005c: return "en-US";
    case 0x001d: return "/"; case 0x001e: return ":"; case 0x001f: return "M/d/yyyy";
    case 0x1003: return "h:mm:ss tt"; case 0x1004: return "1252"; case 0x100b: return "437";
    case 0x100c: return "1"; case 0x100d: return "2"; case 0x000e: return ","; case 0x000f: return ".";
    default: return "";
  }
}
static uint64_t locale_info(CPU *c, int wide) {
  uint32_t type = (uint32_t)A(1); const char *v = locale_text(type);
  if (type & 0x20000000) { if (!A(2) || A(3) < 4) return 0; uint32_t n = (uint32_t)strtoul(v, NULL, 10); ST(uint32_t, A(2), n); return 4; }
  if (!wide) return put_ansi(A(2), (uint32_t)A(3), v);
  size_t n = strlen(v) + 1; if (!A(3)) return n; if (!A(2) || A(3) < n) { aot_last_error = 122; return 0; }
  for (size_t i = 0; i < n; i++) ST(uint16_t, A(2) + 2 * i, (uint8_t)v[i]); return n;
}
SH(GetLocaleInfoA) { return locale_info(c, 0); }
SH(GetLocaleInfoW) { return locale_info(c, 1); }
static void put_system_time(uint64_t p, const struct tm *tm, unsigned ms) {
  uint16_t v[8] = { (uint16_t)(tm->tm_year + 1900), (uint16_t)(tm->tm_mon + 1), (uint16_t)tm->tm_wday, (uint16_t)tm->tm_mday,
                    (uint16_t)tm->tm_hour, (uint16_t)tm->tm_min, (uint16_t)tm->tm_sec, (uint16_t)ms };
  for (int i = 0; i < 8; i++) ST(uint16_t, p + 2 * i, v[i]);
}
SH(GetLocalTime) { struct timeval tv; gettimeofday(&tv, NULL); time_t t = tv.tv_sec; struct tm tm; localtime_r(&t, &tm); put_system_time(A(0), &tm, tv.tv_usec / 1000); return 0; }
SH(GetSystemTime) { struct timeval tv; gettimeofday(&tv, NULL); time_t t = tv.tv_sec; struct tm tm; gmtime_r(&t, &tm); put_system_time(A(0), &tm, tv.tv_usec / 1000); return 0; }
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2; int era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + doe - 719468;
}
static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) {
  z += 719468; int era = (z >= 0 ? z : z - 146096) / 146097; unsigned doe = (unsigned)(z - (int64_t)era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; *y = (int)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1; *m = mp + (mp < 10 ? 3 : (unsigned)-9); *y += *m <= 2;
}
SH(FileTimeToSystemTime) {
  uint64_t ft = LD(uint64_t, A(0)); int64_t sec = (int64_t)(ft / FTICKS) - EPOCH_DELTA, days = sec / 86400, sod = sec % 86400;
  if (sod < 0) { sod += 86400; days--; } int y; unsigned m, d; civil_from_days(days, &y, &m, &d);
  uint16_t v[8] = { (uint16_t)y, (uint16_t)m, 0, (uint16_t)d, (uint16_t)(sod / 3600), (uint16_t)((sod / 60) % 60), (uint16_t)(sod % 60), (uint16_t)((ft % FTICKS) / 10000) };
  int64_t z = days + 719468 - days_from_civil(1970, 1, 1); (void)z; struct tm tm = {0}; time_t tt = (time_t)sec; gmtime_r(&tt, &tm); v[2] = (uint16_t)tm.tm_wday;
  for (int i = 0; i < 8; i++) ST(uint16_t, A(1) + 2 * i, v[i]); return 1;
}
SH(SystemTimeToFileTime) {
  uint64_t p = A(0); uint16_t v[8]; for (int i = 0; i < 8; i++) v[i] = LD(uint16_t, p + 2 * i);
  if (v[1] < 1 || v[1] > 12 || !v[3] || v[4] > 23 || v[5] > 59 || v[6] > 60) return 0;
  int64_t sec = days_from_civil(v[0], v[1], v[3]) * 86400LL + v[4] * 3600 + v[5] * 60 + v[6];
  ST(uint64_t, A(1), (uint64_t)(sec + EPOCH_DELTA) * FTICKS + (uint64_t)v[7] * 10000); return 1;
}
SH(FileTimeToLocalFileTime) {
  uint64_t ft = LD(uint64_t, A(0)); time_t utc = (time_t)(ft / FTICKS - EPOCH_DELTA); struct tm tm; localtime_r(&utc, &tm); time_t local = timegm(&tm);
  ST(uint64_t, A(1), (uint64_t)((int64_t)local + EPOCH_DELTA) * FTICKS + ft % FTICKS); return 1;
}
SH(LocalFileTimeToFileTime) {
  uint64_t ft = LD(uint64_t, A(0)); time_t local = (time_t)(ft / FTICKS - EPOCH_DELTA); struct tm tm; gmtime_r(&local, &tm); tm.tm_isdst = -1; time_t utc = mktime(&tm);
  ST(uint64_t, A(1), (uint64_t)((int64_t)utc + EPOCH_DELTA) * FTICKS + ft % FTICKS); return 1;
}
SH(GetTimeZoneInformation) {
  uint64_t p = A(0); if (!p) return 0xFFFFFFFFu; memset((void *)(uintptr_t)p, 0, 172); time_t now = time(NULL); struct tm local, utc; localtime_r(&now, &local); gmtime_r(&now, &utc);
  time_t local_as_utc = mktime(&local), utc_as_utc = mktime(&utc); int bias = (int)difftime(utc_as_utc, local_as_utc) / 60; ST(int32_t, p, bias);
  const char *name = local.tm_isdst > 0 ? "Daylight" : "Standard"; size_t off = local.tm_isdst > 0 ? 88 : 4; for (size_t i = 0; name[i]; i++) ST(uint16_t, p + off + 2 * i, (uint8_t)name[i]);
  return local.tm_isdst > 0 ? 2 : 1;
}
SH(GetProcessTimes) {
  struct timeval tv; gettimeofday(&tv, NULL); uint64_t creation = ((uint64_t)tv.tv_sec + EPOCH_DELTA) * FTICKS + tv.tv_usec * 10;
  struct rusage ru; getrusage(RUSAGE_SELF, &ru); uint64_t user = ((uint64_t)ru.ru_utime.tv_sec * 1000000 + ru.ru_utime.tv_usec) * 10;
  uint64_t kernel = ((uint64_t)ru.ru_stime.tv_sec * 1000000 + ru.ru_stime.tv_usec) * 10;
  ST(uint64_t, A(1), creation); ST(uint64_t, A(2), creation); ST(uint64_t, A(3), kernel); ST(uint64_t, A(4), user); return 1;
}
static void dt_text(char *out, size_t cap, size_t *used, const char *s) {
  size_t n = strlen(s); if (*used + n >= cap) { *used = cap; return; }
  memcpy(out + *used, s, n); *used += n;
}
static void dt_number(char *out, size_t cap, size_t *used, int value, int width) {
  char b[16]; snprintf(b, sizeof b, "%0*d", width, value); dt_text(out, cap, used, b);
}
/* Win32 date/time pictures are tokens, not printf format strings. */
static int format_dt(char *out, size_t cap, const char *fmt, const struct tm *tm, int date) {
  static const char *months[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
  static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
  char tmp[512]; size_t used = 0; int quoted = 0;
  if (!fmt) fmt = date ? "MM/dd/yyyy" : "HH:mm:ss";
  for (size_t i = 0; fmt[i] && used < sizeof tmp - 1;) {
    if (fmt[i] == '\'') {
      if (fmt[i + 1] == '\'') { tmp[used++] = '\''; i += 2; }
      else { quoted = !quoted; i++; }
      continue;
    }
    char ch = fmt[i]; size_t run = 1; while (fmt[i + run] == ch) run++;
    if (!quoted && date && ch == 'y') { int year = tm->tm_year + 1900; dt_number(tmp, sizeof tmp, &used, run <= 2 ? year % 100 : year, run <= 2 ? 2 : 4); i += run; continue; }
    if (!quoted && date && ch == 'M') {
      if (run >= 4) dt_text(tmp, sizeof tmp, &used, months[tm->tm_mon]);
      else if (run == 3) { char b[4]; memcpy(b, months[tm->tm_mon], 3); b[3] = 0; dt_text(tmp, sizeof tmp, &used, b); }
      else dt_number(tmp, sizeof tmp, &used, tm->tm_mon + 1, run == 2 ? 2 : 1);
      i += run; continue;
    }
    if (!quoted && date && ch == 'd') {
      if (run >= 4) dt_text(tmp, sizeof tmp, &used, days[tm->tm_wday]);
      else if (run == 3) { char b[4]; memcpy(b, days[tm->tm_wday], 3); b[3] = 0; dt_text(tmp, sizeof tmp, &used, b); }
      else dt_number(tmp, sizeof tmp, &used, tm->tm_mday, run == 2 ? 2 : 1);
      i += run; continue;
    }
    if (!quoted && !date && (ch == 'H' || ch == 'h')) { int hour = ch == 'h' ? (tm->tm_hour + 11) % 12 + 1 : tm->tm_hour; dt_number(tmp, sizeof tmp, &used, hour, run == 2 ? 2 : 1); i += run; continue; }
    if (!quoted && !date && ch == 'm') { dt_number(tmp, sizeof tmp, &used, tm->tm_min, run == 2 ? 2 : 1); i += run; continue; }
    if (!quoted && !date && ch == 's') { dt_number(tmp, sizeof tmp, &used, tm->tm_sec, run == 2 ? 2 : 1); i += run; continue; }
    if (!quoted && !date && ch == 't') { const char *ampm = tm->tm_hour < 12 ? "AM" : "PM"; dt_text(tmp, sizeof tmp, &used, run == 1 ? (tm->tm_hour < 12 ? "A" : "P") : ampm); i += run; continue; }
    tmp[used++] = ch; i++;
  }
  if (used >= sizeof tmp) used = sizeof tmp - 1; tmp[used] = 0;
  if (!cap) return (int)used + 1;
  if (!out || cap <= used) { aot_last_error = 122; return 0; }
  memcpy(out, tmp, used + 1); return (int)used + 1;
}
static uint64_t format_dt_shim(CPU *c, int date) {
  uint64_t sp = A(2); time_t now = time(NULL); struct tm tm; if (sp) { struct tm v = {0}; v.tm_year = LD(uint16_t, sp) - 1900; v.tm_mon = LD(uint16_t, sp + 2) - 1; v.tm_mday = LD(uint16_t, sp + 6); v.tm_hour = LD(uint16_t, sp + 8); v.tm_min = LD(uint16_t, sp + 10); v.tm_sec = LD(uint16_t, sp + 12); v.tm_isdst = -1; mktime(&v); tm = v; } else localtime_r(&now, &tm);
  return format_dt((char *)(uintptr_t)A(4), A(5), A(3) ? STR(A(3)) : NULL, &tm, date);
}
SH(GetDateFormatA) { return format_dt_shim(c, 1); }
SH(GetTimeFormatA) { return format_dt_shim(c, 0); }

/* ---- callbacks / diagnostics / memory queries ---- */
SH(EnumSystemLocalesA) {
  static uint64_t locale; if (!A(0)) { aot_last_error = 87; return 0; }
  if (!locale) { char *s = aot_gmalloc(9); if (!s) return 0; memcpy(s, "00000409", 9); locale = (uint64_t)(uintptr_t)s; }
  uint64_t r = aot_call(c, A(0), 1, locale); return (uint32_t)r;
}
int aot_vres_lookup(uint64_t a, uint64_t *base, uint64_t *size);
SH(VirtualQuery) {   /* Windows semantics: reservations report their real AllocationBase/RegionSize; unmapped space is MEM_FREE (a success, not an error) */
  uintptr_t addr = (uintptr_t)A(0), page = addr & ~(uintptr_t)4095; unsigned char vec;
  size_t need = aot_is_pe32 ? 28 : 48; if (!A(1) || A(2) < need) { aot_last_error = 24; return 0; } uint8_t *o = (uint8_t *)(uintptr_t)A(1); memset(o, 0, need);
  uint64_t rb = 0, rn = 0; int vres = aot_vres_lookup(page, &rb, &rn), mapped = vres || !mincore((void *)page, 4096, &vec);
  uint64_t alloc_base = vres ? rb : mapped ? page : 0, region = vres ? rb + rn - page : mapped ? 4096 : 0x10000 - (page & 0xffff);
  uint32_t state = mapped ? 0x1000 : 0x10000, prot = mapped ? 4 : 1, alloc_prot = mapped ? 4 : 0, type = mapped ? 0x20000 : 0; if (!mapped) prot = 1, alloc_prot = 0;
  if (aot_is_pe32) { ST(uint32_t, o, page); ST(uint32_t, o + 4, alloc_base); ST(uint32_t, o + 8, alloc_prot); ST(uint32_t, o + 12, region); ST(uint32_t, o + 16, state); ST(uint32_t, o + 20, mapped ? prot : 1); ST(uint32_t, o + 24, type); }
  else { ST(uint64_t, o, page); ST(uint64_t, o + 8, alloc_base); ST(uint32_t, o + 16, alloc_prot); ST(uint64_t, o + 24, region); ST(uint32_t, o + 32, state); ST(uint32_t, o + 36, mapped ? prot : 1); ST(uint32_t, o + 40, type); }
  return need;
}

/* FormatMessageA: system text, literal source, and numbered string inserts. */
SH(FormatMessageA) {
  uint32_t flags = (uint32_t)A(0); const char *source = NULL;
  if (flags & 0x1000) source = strerror((int)A(2));
  else if (flags & 0x400) source = A(1) ? STR(A(1)) : NULL;
  if (!source) { aot_last_error = 87; return 0; }
  size_t cap = strlen(source) + 256; char *msg = aot_gmalloc(cap); if (!msg) return 0; size_t k = 0;
  for (size_t i = 0; source[i] && k + 1 < cap;) {
    if (source[i] == '%' && source[i + 1] >= '1' && source[i + 1] <= '9') {
      unsigned slot = (unsigned)(source[i + 1] - '1'); const char *ins = "";
      if (A(6)) ins = aot_is_pe32 ? STR(LD(uint32_t, A(6) + 4ull * slot)) : STR(LD(uint64_t, A(6) + 8ull * slot));
      size_t n = strlen(ins); if (k + n >= cap) n = cap - k - 1; memcpy(msg + k, ins, n); k += n; i += 2;
    } else msg[k++] = source[i++];
  }
  while (k && (msg[k - 1] == '\r' || msg[k - 1] == '\n')) k--; msg[k] = 0;
  if (flags & 0x100) { char *guest = aot_gmalloc(k + 1); if (!guest) { aot_gfree(msg); return 0; } memcpy(guest, msg, k + 1); STP(A(4), guest); }
  else { if (!A(4) || A(5) <= k) { aot_gfree(msg); aot_last_error = 122; return 0; } memcpy((void *)(uintptr_t)A(4), msg, k + 1); }
  aot_gfree(msg); return k;
}

const ShimEnt aot_shim_table_compat[] = {
  S(CompareStringA), S(CompareStringW), S(LCMapStringA), S(LCMapStringW), S(GetStringTypeA), S(GetStringTypeW),
  S(WideCharToMultiByte), S(WriteConsoleW), S(GetConsoleCP), S(GetConsoleOutputCP), S(GetOEMCP), S(GetCPInfo), S(IsValidCodePage),
  S(GetEnvironmentStrings), S(FreeEnvironmentStringsA), S(FreeLibrary), S(SetEnvironmentVariableA), S(GetComputerNameExA),
  S(GetVersion), S(GetVersionExA), S(GetUserDefaultLCID), S(IsValidLocale), S(TerminateProcess), S(DebugBreak), S(FatalAppExitA),
  S(HeapCreate), S(HeapDestroy), S(SetHandleCount), S(GetLogicalDriveStringsA), S(GetDriveTypeA), S(GetTempPathA),
  S(GetLocaleInfoA), S(GetLocaleInfoW), S(GetLocalTime), S(GetSystemTime), S(FileTimeToSystemTime), S(SystemTimeToFileTime),
  S(FileTimeToLocalFileTime), S(LocalFileTimeToFileTime), S(GetTimeZoneInformation), S(GetProcessTimes), S(GetDateFormatA), S(GetTimeFormatA),
  S(EnumSystemLocalesA), S(VirtualQuery), S(FormatMessageA),
  { 0, 0 }
};
