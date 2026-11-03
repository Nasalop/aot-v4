__declspec(dllimport) void* __stdcall GetStdHandle(unsigned); __declspec(dllimport) int __stdcall WriteFile(void*, const void*, unsigned, unsigned*, void*); __declspec(dllimport) void __stdcall ExitProcess(unsigned);
static void out(const char *t) { unsigned n = 0, w; while (t[n]) n++; WriteFile(GetStdHandle((unsigned)-11), t, n, &w, 0); }
static void num(const char *k, unsigned v) { char b[40]; int i = 0; out(k); out("="); char t[12]; int j = 0; if (!v) t[j++] = '0'; while (v) { t[j++] = '0' + v % 10; v /= 10; } while (j) b[i++] = t[--j]; b[i++] = ' '; b[i] = 0; out(b); }
__declspec(dllimport) int __stdcall WSAStartup(unsigned short, void*); __declspec(dllimport) unsigned __stdcall socket(int,int,int); __declspec(dllimport) int __stdcall closesocket(unsigned);
__declspec(dllimport) unsigned short __stdcall htons(unsigned short); __declspec(dllimport) unsigned long __stdcall inet_addr(const char*); __declspec(dllimport) int __stdcall WSAGetLastError(void);
__declspec(dllimport) int __stdcall CryptAcquireContextA(unsigned*,const char*,const char*,unsigned,unsigned); __declspec(dllimport) int __stdcall CryptGenRandom(unsigned,unsigned,unsigned char*);
__declspec(dllimport) long __stdcall RegOpenKeyExA(unsigned,const char*,unsigned,unsigned,unsigned*);
__declspec(dllimport) unsigned short* __stdcall SysAllocString(const unsigned short*);
typedef struct { unsigned cb, style; void *proc; int cls, wnd; void *inst, *icon, *cur, *bg; const char *menu, *name; void *ism; } WC;
typedef struct { void *hwnd; unsigned msg; unsigned wp, lp, time; int x, y; } MSG;
__declspec(dllimport) unsigned short __stdcall RegisterClassExA(WC*); __declspec(dllimport) void* __stdcall CreateWindowExA(unsigned,const char*,const char*,unsigned,int,int,int,int,void*,void*,void*,void*);
__declspec(dllimport) int __stdcall PeekMessageA(MSG*,void*,unsigned,unsigned,unsigned); __declspec(dllimport) int __stdcall DispatchMessageA(MSG*); __declspec(dllimport) int __stdcall ShowWindow(void*,int);
__declspec(dllimport) long __stdcall DefWindowProcA(void*,unsigned,unsigned,long);
static unsigned seen_create, cs_cx, cs_cy, paints;
long __stdcall WP(void *h, unsigned m, unsigned w, long l) {
  if (m == 1) { seen_create = 1; cs_cx = *(int*)(l + 20); cs_cy = *(int*)(l + 16); }
  if (m == 0xF) paints++;
  return DefWindowProcA(h, m, w, l);
}
int main(void) {
  unsigned char wsa[400]; num("wsa", WSAStartup(0x202, wsa));
  unsigned s = socket(2, 1, 0); num("sock_ok", s != 0xFFFFFFFFu); num("htons", htons(0x1234)); closesocket(s);
  num("inet", (unsigned)inet_addr("1.2.3.4"));
  unsigned p = 0; unsigned char r[16] = {0}; num("crypt", CryptAcquireContextA(&p, 0, 0, 1, 0)); num("gen", CryptGenRandom(p, 16, r)); num("reg", (unsigned)RegOpenKeyExA(0x80000001u, "Software", 0, 1, &p));
  unsigned short src[] = { 'h','i',0 }; unsigned short *b = SysAllocString(src); num("bstrlen", *(unsigned*)((char*)b - 4)); num("bstr0", b[0]);
  WC wc = { sizeof(WC) }; wc.proc = WP; wc.name = "AOT"; num("class", RegisterClassExA(&wc) != 0);
  void *w = CreateWindowExA(0, "AOT", "t", 0x10CF0000, 0, 0, 640, 480, 0, 0, 0, 0); num("create", seen_create); num("cx", cs_cx); num("cy", cs_cy);
  ShowWindow(w, 1); MSG m; int n = 0; while (PeekMessageA(&m, 0, 0, 0, 1) && n++ < 20) DispatchMessageA(&m); num("paints", paints > 0);
  out("\n"); ExitProcess(0); return 0; }
int mainCRTStartup(void) { ExitProcess((unsigned)main()); return 0; }
