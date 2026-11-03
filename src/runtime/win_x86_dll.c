/* PE32 imports from the remaining system DLLs (user32 extras, advapi32, ole32, oleaut32, shell32, iphlpapi, wininet, winmm,
 * ws2_32, dinput8, dsound, eax, xinput1_3, d3dx10_36, binkw32). Each entry is either a real implementation or an explicit,
 * documented failure result (never a silent success). Stdcall pop sizes are per the Win32 headers.
 * Tables are parsed by src/tools/pe_dependency_inspect.py: R(dll,name,pop) = reuse existing shim, U(dll,name,pop) = u_<name> here,
 * UO(dll,"#ord",pop,fn) = ordinal import. */
#define _GNU_SOURCE
#include "shims.h"
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>
#include <sys/random.h>

#define A(n) argn(c, (n))
static inline uint64_t argn(CPU *c, int n) { return n == 0 ? c->r[1] : n == 1 ? c->r[2] : n == 2 ? c->r[8] : n == 3 ? c->r[9] : LD(uint64_t, c->r[4] + 8 + 8 * n); }
#define P(n) ((void *)(uintptr_t)(uint32_t)A(n))
#define S(n) uint64_t u_##n(CPU *c)
static void note_once(int *flag, const char *msg) { if (!__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED)) { fflush(stdout); fprintf(stderr, "[aot] %s\n", msg); } }

/* ---- user32 extras (the rest reuse win_input.c / win_gfx.c) ---- */
S(SetWindowsHookExA) { static int n; note_once(&n, "SetWindowsHookExA: low-level hooks are not supported; returning failure"); aot_last_error = 1427; return 0; }
S(UnhookWindowsHookEx) { return 0; }
S(CallNextHookEx) { return 0; }   /* no next hook in the chain */
S(LoadStringA) { char *b = P(2); int n = (int)A(3); if (b && n > 0) b[0] = 0; aot_last_error = 1814; return 0; }   /* no string resources: ERROR_RESOURCE_NAME_NOT_FOUND */
S(SystemParametersInfoA) { aot_last_error = 87; return 0; }
S(ToUnicode) {   /* US-layout ASCII only: (vk, scan, keystate*, buf*, cch, flags) */
  uint32_t vk = (uint32_t)A(0); const uint8_t *ks = P(2); uint16_t *b = P(3); int sh = ks && (ks[0x10] & 0x80), ch = 0;
  if (vk >= 'A' && vk <= 'Z') ch = sh ? (int)vk : (int)vk + 32; else if (vk >= '0' && vk <= '9') ch = (int)vk; else if (vk == 0x20) ch = ' ';
  if (!ch || (int)A(4) < 1) return 0; b[0] = (uint16_t)ch; return 1;
}

/* ---- advapi32 ---- */
S(CryptAcquireContextA) { if (P(0)) ST(uint32_t, A(0), 1); return 1; }
S(CryptGenRandom) { uint8_t *b = P(2); uint32_t n = (uint32_t)A(1), got = 0; while (got < n) { ssize_t r = getrandom(b + got, n - got, 0); if (r <= 0) return 0; got += (uint32_t)r; } return 1; }
S(CryptReleaseContext) { return 1; }
S(GetUserNameA) { char *b = P(0); uint32_t *sz = P(1); if (!sz) return 0; if (!b || *sz < 7) { *sz = 7; aot_last_error = 122; return 0; } memcpy(b, "player", 7); *sz = 7; return 1; }
S(RegOpenKeyExA) { return 2; }       /* ERROR_FILE_NOT_FOUND: the registry is not emulated, so no key exists */
S(RegQueryValueExA) { return 2; }
S(RegCloseKey) { return 0; }

/* ---- ole32 / oleaut32 / shell32 ---- */
S(CoInitialize) { return 0; }
S(CoUninitialize) { return 0; }
S(CoCreateInstance) { if (P(4)) ST(uint32_t, A(4), 0); return 0x80040154u; }   /* REGDB_E_CLASSNOTREG: no COM classes are registered */
S(SysAllocString) {
  const uint16_t *s = P(0); if (!s) return 0; uint32_t n = 0; while (s[n]) n++;
  uint8_t *b = aot_gmalloc(4 + 2 * n + 2); if (!b) return 0; ST(uint32_t, b, 2 * n); memcpy(b + 4, s, 2 * n + 2); return (uint32_t)(uintptr_t)(b + 4);
}
S(SHCreateDirectoryExA) {
  char p[512]; snprintf(p, sizeof p, "%s", (const char *)P(1)); for (char *q = p; *q; q++) if (*q == '\\') *q = '/';
  int existed = 0; for (char *q = p + 1; ; q++) { char ch = *q; if (ch == '/' || !ch) { *q = 0; if (mkdir(p, 0777) && errno == EEXIST) existed = 1; else existed = 0; *q = ch; } if (!ch) break; }
  return existed ? 183 : 0;
}
S(SHGetSpecialFolderPathA) { char *b = P(1); if (!b || !getcwd(b, 260)) return 0; return 1; }   /* every special folder maps to the game data directory */

/* ---- iphlpapi / wininet / winmm ---- */
S(GetAdaptersInfo) { return 232; }                                    /* ERROR_NO_DATA: no network adapters are reported */
S(InternetGetCookieW) { aot_last_error = 259; return 0; }
S(InternetSetCookieW) { aot_last_error = 5; return 0; }
S(waveOutGetDevCapsA) { return 6; }                                   /* MMSYSERR_NODRIVER */

/* ---- DirectInput / DirectSound / EAX / XInput / D3DX10 / Bink: explicit "unavailable" results ---- */
S(DirectInput8Create) { static int n; note_once(&n, "dinput8: DirectInput unavailable (DIERR_DEVICENOTREG); game must fall back to window messages"); if (P(3)) ST(uint32_t, A(3), 0); return 0x80040154u; }
S(dsound_enum) { return 0; }                                          /* DirectSoundEnumerateA: no devices enumerated */
S(dsound_create) { static int n; note_once(&n, "dsound/eax: no audio device layer; DirectSoundCreate* returns DSERR_NODRIVER (game audio disabled)"); if (P(1)) ST(uint32_t, A(1), 0); return 0x88780078u; }
S(xinput_none) { return 1167; }                                       /* ERROR_DEVICE_NOT_CONNECTED */
S(D3DX10UnsetAllDeviceObjects) { return 0; }                          /* teardown helper: nothing is cached on our side */
S(bink_unavail) { static int n; note_once(&n, "binkw32: Bink video is not available; BinkOpen fails so videos are skipped"); return 0; }

/* ---- ws2_32 (BSD sockets underneath; Winsock constants/layouts translated) ---- */
static __thread int wsa_err;
static int wsa_map(int e) {
  switch (e) { case EWOULDBLOCK: case EINPROGRESS: return 10035; case EINTR: return 10004; case EBADF: return 10009; case EACCES: return 10013; case EFAULT: return 10014; case EINVAL: return 10022;
    case EMFILE: return 10024; case EADDRINUSE: return 10048; case EADDRNOTAVAIL: return 10049; case ENETUNREACH: return 10051; case ECONNABORTED: return 10053; case ECONNRESET: return 10054;
    case ENOTCONN: return 10057; case ETIMEDOUT: return 10060; case ECONNREFUSED: return 10061; case ENOTSOCK: return 10038; case EPROTONOSUPPORT: return 10043; case EAFNOSUPPORT: return 10047; case EISCONN: return 10056; case EMSGSIZE: return 10040; default: return 10000 + e; }
}
static uint64_t sret(long r) { if (r < 0) { wsa_err = wsa_map(errno); return 0xFFFFFFFFu; } return (uint32_t)r; }
S(WSAStartup) { uint8_t *d = P(1); if (d) { memset(d, 0, 400); ST(uint16_t, d, 0x0202); ST(uint16_t, d + 2, 0x0202); memcpy(d + 4, "AOT Winsock", 12); ST(uint16_t, d + 390, 32767); } return 0; }
S(WSACleanup) { return 0; }
S(WSAGetLastError) { return (uint32_t)wsa_err; }
S(WSAAsyncSelect) { static int n; note_once(&n, "WSAAsyncSelect: window-message socket notification is not supported; returning SOCKET_ERROR"); wsa_err = 10045; return 0xFFFFFFFFu; }
S(socket) { return sret(socket((int)A(0), (int)A(1), (int)A(2))); }
S(closesocket) { return sret(close((int)A(0))); }
S(bind) { return sret(bind((int)A(0), P(1), (socklen_t)A(2))); }
S(connect) { return sret(connect((int)A(0), P(1), (socklen_t)A(2))); }
S(listen) { return sret(listen((int)A(0), (int)A(1))); }
S(accept) { socklen_t l = 0, *pl = NULL; if (P(2)) { l = LD(int32_t, A(2)); pl = &l; } long r = accept((int)A(0), P(1), pl); if (pl && r >= 0) ST(int32_t, A(2), (int32_t)l); return sret(r); }
S(send) { return sret(send((int)A(0), P(1), (size_t)A(2), (int)A(3))); }
S(recv) { return sret(recv((int)A(0), P(1), (size_t)A(2), (int)A(3))); }
S(sendto) { return sret(sendto((int)A(0), P(1), (size_t)A(2), (int)A(3), P(4), (socklen_t)A(5))); }
S(recvfrom) { socklen_t l = 0, *pl = NULL; if (P(5)) { l = LD(int32_t, A(5)); pl = &l; } long r = recvfrom((int)A(0), P(1), (size_t)A(2), (int)A(3), P(4), pl); if (pl && r >= 0) ST(int32_t, A(5), (int32_t)l); return sret(r); }
S(getpeername) { socklen_t l = LD(int32_t, A(2)); long r = getpeername((int)A(0), P(1), &l); if (r >= 0) ST(int32_t, A(2), (int32_t)l); return sret(r); }
S(getsockname) { socklen_t l = LD(int32_t, A(2)); long r = getsockname((int)A(0), P(1), &l); if (r >= 0) ST(int32_t, A(2), (int32_t)l); return sret(r); }
static int sockopt(int level, int opt, int *L, int *O) {   /* Winsock -> Linux level/option */
  if (level == 0xffff) { *L = SOL_SOCKET; switch (opt) { case 4: *O = SO_REUSEADDR; return 0; case 8: *O = SO_KEEPALIVE; return 0; case 0x20: *O = SO_BROADCAST; return 0; case 0x80: *O = SO_LINGER; return 0;
    case 0x1001: *O = SO_SNDBUF; return 0; case 0x1002: *O = SO_RCVBUF; return 0; case 0x1007: *O = SO_ERROR; return 0; case 0x1008: *O = SO_TYPE; return 0; default: return -1; } }
  if (level == 6 && opt == 1) { *L = IPPROTO_TCP; *O = TCP_NODELAY; return 0; }
  return -1;
}
S(setsockopt) { int L, O; if (sockopt((int)A(1), (int)A(2), &L, &O)) { wsa_err = 10042; return 0xFFFFFFFFu; } return sret(setsockopt((int)A(0), L, O, P(3), (socklen_t)A(4))); }
S(getsockopt) { int L, O; if (sockopt((int)A(1), (int)A(2), &L, &O)) { wsa_err = 10042; return 0xFFFFFFFFu; } socklen_t l = LD(int32_t, A(4)); long r = getsockopt((int)A(0), L, O, P(3), &l); if (r >= 0) ST(int32_t, A(4), (int32_t)l); return sret(r); }
S(ioctlsocket) {
  uint32_t cmd = (uint32_t)A(1); int fd = (int)A(0);
  if (cmd == 0x8004667Eu) { int fl = fcntl(fd, F_GETFL); if (fl < 0) return sret(-1); return sret(fcntl(fd, F_SETFL, LD(uint32_t, A(2)) ? fl | O_NONBLOCK : fl & ~O_NONBLOCK)); }   /* FIONBIO */
  if (cmd == 0x4004667Fu) { int n = 0; long r = ioctl(fd, FIONREAD, &n); if (r >= 0) ST(uint32_t, A(2), (uint32_t)n); return sret(r); }   /* FIONREAD */
  wsa_err = 10022; return 0xFFFFFFFFu;
}
/* Winsock fd_set: u_int count + SOCKET[64]; timeval = two int32 */
static void ws_in(const uint8_t *w, fd_set *o, int *mx) { FD_ZERO(o); if (!w) return; uint32_t n = LD(uint32_t, (uintptr_t)w); for (uint32_t i = 0; i < n && i < 64; i++) { int fd = (int)LD(uint32_t, (uintptr_t)w + 4 + 4 * i); if (fd >= 0 && fd < FD_SETSIZE) { FD_SET(fd, o); if (fd > *mx) *mx = fd; } } }
static void ws_out(uint8_t *w, const fd_set *s) { if (!w) return; uint32_t n = LD(uint32_t, (uintptr_t)w), k = 0; for (uint32_t i = 0; i < n && i < 64; i++) { uint32_t fd = LD(uint32_t, (uintptr_t)w + 4 + 4 * i); if (fd < FD_SETSIZE && FD_ISSET((int)fd, s)) ST(uint32_t, (uintptr_t)w + 4 + 4 * k++, fd); } ST(uint32_t, (uintptr_t)w, k); }
S(select) {
  fd_set r, w, e; int mx = -1; ws_in(P(1), &r, &mx); ws_in(P(2), &w, &mx); ws_in(P(3), &e, &mx);
  struct timeval tv, *ptv = NULL; if (P(4)) { tv.tv_sec = LD(int32_t, A(4)); tv.tv_usec = LD(int32_t, A(4) + 4); ptv = &tv; }
  long n = select(mx + 1, P(1) ? &r : NULL, P(2) ? &w : NULL, P(3) ? &e : NULL, ptv);
  if (n >= 0) { ws_out(P(1), &r); ws_out(P(2), &w); ws_out(P(3), &e); } return sret(n);
}
S(__WSAFDIsSet) { uint32_t fd = (uint32_t)A(0), n = LD(uint32_t, A(1)); for (uint32_t i = 0; i < n && i < 64; i++) if (LD(uint32_t, A(1) + 4 + 4 * i) == fd) return 1; return 0; }
S(htonl) { return __builtin_bswap32((uint32_t)A(0)); }
S(ntohl) { return __builtin_bswap32((uint32_t)A(0)); }
S(htons) { return __builtin_bswap16((uint16_t)A(0)); }
S(ntohs) { return __builtin_bswap16((uint16_t)A(0)); }
S(inet_addr) { struct in_addr a; if (!inet_aton((const char *)P(0), &a)) return 0xFFFFFFFFu; return a.s_addr; }
S(inet_ntoa) { static __thread char *buf; if (!buf) buf = aot_gmalloc(32); struct in_addr a; a.s_addr = (uint32_t)A(0); snprintf(buf, 32, "%s", inet_ntoa(a)); return (uint32_t)(uintptr_t)buf; }
S(gethostname) { char *b = P(0); int n = (int)A(1); if (gethostname(b, (size_t)n)) return sret(-1); b[n - 1] = 0; return 0; }
static uint64_t hostent32(struct hostent *h) {   /* 32-bit HOSTENT in low memory: name, aliases, addrtype(2), length(2), addr_list */
  static __thread uint8_t *blk; if (!blk) blk = aot_gmalloc(512); if (!h || h->h_addrtype != AF_INET) { wsa_err = 11001; return 0; }
  memset(blk, 0, 512); uint8_t *p = blk + 16; ST(uint32_t, blk + 4, 0); snprintf((char *)p, 128, "%s", h->h_name); ST(uint32_t, blk, (uint32_t)(uintptr_t)p); p += 128;
  uint8_t *lst = p; p += 4 * 9; int n = 0;
  for (; h->h_addr_list[n] && n < 8; n++) { memcpy(p, h->h_addr_list[n], 4); ST(uint32_t, lst + 4 * n, (uint32_t)(uintptr_t)p); p += 4; }
  ST(uint16_t, blk + 8, AF_INET); ST(uint16_t, blk + 10, 4); ST(uint32_t, blk + 12, (uint32_t)(uintptr_t)lst); return (uint32_t)(uintptr_t)blk;
}
S(gethostbyname) { return hostent32(gethostbyname((const char *)P(0))); }
S(gethostbyaddr) { return hostent32(gethostbyaddr(P(0), (socklen_t)A(1), (int)A(2))); }

#define U(dll, n, b) { dll, #n, b, u_##n }
#define UO(dll, n, b, f) { dll, n, b, u_##f }
#define R(dll, n, b) { dll, #n, b, NULL }
typedef struct { const char *dll, *name; int pop; shim_fn fn; } DllApi;
static const DllApi tab[] = {
  R("user32.dll", AdjustWindowRect, 12), R("user32.dll", ClipCursor, 4), R("user32.dll", CreateWindowExA, 48), R("user32.dll", DefWindowProcA, 16), R("user32.dll", DestroyWindow, 4),
  R("user32.dll", DispatchMessageA, 4), R("user32.dll", GetClientRect, 8), R("user32.dll", GetCursorPos, 4), R("user32.dll", GetDesktopWindow, 0), R("user32.dll", GetKeyboardState, 4),
  R("user32.dll", GetSystemMetrics, 4), R("user32.dll", GetWindowRect, 8), R("user32.dll", IsIconic, 4), R("user32.dll", IsWindow, 4), R("user32.dll", IsWindowVisible, 4),
  R("user32.dll", LoadCursorA, 8), R("user32.dll", MessageBoxA, 16), R("user32.dll", MoveWindow, 24), R("user32.dll", PeekMessageA, 20), R("user32.dll", PostQuitMessage, 4),
  R("user32.dll", RegisterClassExA, 4), R("user32.dll", ScreenToClient, 8), R("user32.dll", ShowCursor, 4), R("user32.dll", ShowWindow, 8), R("user32.dll", TranslateMessage, 4), R("user32.dll", UpdateWindow, 4),
  U("user32.dll", SetWindowsHookExA, 16), U("user32.dll", UnhookWindowsHookEx, 4), U("user32.dll", CallNextHookEx, 16), U("user32.dll", LoadStringA, 16), U("user32.dll", SystemParametersInfoA, 16), U("user32.dll", ToUnicode, 24),
  U("advapi32.dll", CryptAcquireContextA, 20), U("advapi32.dll", CryptGenRandom, 12), U("advapi32.dll", CryptReleaseContext, 8), U("advapi32.dll", GetUserNameA, 8),
  U("advapi32.dll", RegOpenKeyExA, 20), U("advapi32.dll", RegQueryValueExA, 24), U("advapi32.dll", RegCloseKey, 4),
  U("ole32.dll", CoInitialize, 4), U("ole32.dll", CoUninitialize, 0), U("ole32.dll", CoCreateInstance, 20), U("oleaut32.dll", SysAllocString, 4),
  U("shell32.dll", SHCreateDirectoryExA, 12), U("shell32.dll", SHGetSpecialFolderPathA, 16),
  U("iphlpapi.dll", GetAdaptersInfo, 8), U("wininet.dll", InternetGetCookieW, 16), U("wininet.dll", InternetSetCookieW, 12), U("winmm.dll", waveOutGetDevCapsA, 12),
  U("dinput8.dll", DirectInput8Create, 20), U("d3dx10_36.dll", D3DX10UnsetAllDeviceObjects, 4),
  UO("dsound.dll", "#2", 8, dsound_enum), UO("dsound.dll", "#11", 12, dsound_create), UO("eax.dll", "#6", 12, dsound_create),
  UO("xinput1_3.dll", "#2", 8, xinput_none), UO("xinput1_3.dll", "#3", 8, xinput_none), UO("xinput1_3.dll", "#4", 12, xinput_none),
  UO("binkw32.dll", "_BinkOpen", 8, bink_unavail), UO("binkw32.dll", "_BinkClose", 4, bink_unavail), UO("binkw32.dll", "_BinkCopyToBuffer", 28, bink_unavail), UO("binkw32.dll", "_BinkDoFrame", 4, bink_unavail),
  UO("binkw32.dll", "_BinkGetFrameBuffersInfo", 8, bink_unavail), UO("binkw32.dll", "_BinkNextFrame", 4, bink_unavail), UO("binkw32.dll", "_BinkOpenDirectSound", 4, bink_unavail), UO("binkw32.dll", "_BinkPause", 8, bink_unavail),
  UO("binkw32.dll", "_BinkSetMemory", 8, bink_unavail), UO("binkw32.dll", "_BinkSetSoundSystem", 8, bink_unavail), UO("binkw32.dll", "_BinkSetSoundTrack", 8, bink_unavail), UO("binkw32.dll", "_BinkSetVolume", 12, bink_unavail),
  UO("binkw32.dll", "_BinkShouldSkip", 4, bink_unavail), UO("binkw32.dll", "_BinkWait", 4, bink_unavail),
  U("ws2_32.dll", WSAAsyncSelect, 16), U("ws2_32.dll", WSACleanup, 0), U("ws2_32.dll", WSAGetLastError, 0), U("ws2_32.dll", WSAStartup, 8), U("ws2_32.dll", __WSAFDIsSet, 8),
  U("ws2_32.dll", accept, 12), U("ws2_32.dll", bind, 12), U("ws2_32.dll", closesocket, 4), U("ws2_32.dll", connect, 12), U("ws2_32.dll", gethostbyaddr, 12), U("ws2_32.dll", gethostbyname, 4),
  U("ws2_32.dll", gethostname, 8), U("ws2_32.dll", getpeername, 12), U("ws2_32.dll", getsockname, 12), U("ws2_32.dll", getsockopt, 20), U("ws2_32.dll", htonl, 4), U("ws2_32.dll", htons, 4),
  U("ws2_32.dll", inet_addr, 4), U("ws2_32.dll", inet_ntoa, 4), U("ws2_32.dll", ioctlsocket, 12), U("ws2_32.dll", listen, 8), U("ws2_32.dll", ntohl, 4), U("ws2_32.dll", ntohs, 4),
  U("ws2_32.dll", recv, 16), U("ws2_32.dll", recvfrom, 24), U("ws2_32.dll", select, 20), U("ws2_32.dll", send, 16), U("ws2_32.dll", sendto, 24), U("ws2_32.dll", setsockopt, 20), U("ws2_32.dll", socket, 12),
  { NULL, NULL, 0, NULL }
};
shim_fn aot_x86_dll_resolve(const char *dll, const char *name, int *pop) {
  for (const DllApi *a = tab; a->name; a++)
    if (!strcasecmp(a->dll, dll) && !strcmp(a->name, name)) { *pop = a->pop; return a->fn ? a->fn : aot_resolve_import(dll, name); }
  return NULL;
}
