/* D3D11 clear+present test (64-bit, no SDK headers: COM calls by vtable slot) */
typedef unsigned long long u64; typedef unsigned u32; typedef long HR;
__declspec(dllimport) int printf(const char*, ...); __declspec(dllimport) void exit(int);
__declspec(dllimport) HR D3D11CreateDeviceAndSwapChain(void*, u32, void*, u32, const u32*, u32, u32, void*, void**, void**, u32*, void**);
__declspec(dllimport) u32 RegisterClassExW(void*);
__declspec(dllimport) void* CreateWindowExW(u32, const unsigned short*, const unsigned short*, u32, int, int, int, int, void*, void*, void*, void*);
__declspec(dllimport) int ShowWindow(void*, int); __declspec(dllimport) int PeekMessageW(void*, void*, u32, u32, u32);
__declspec(dllimport) int GetSystemMetrics(int);
#define VT(o) (*(void***)(o))
static long long __stdcall wndproc(void* h, u32 m, u64 w, long long l) { return 0; }
int _fltused;
int mainCRTStartup(void) {
  static const unsigned short cls[] = { 'T', 'e', 's', 't', 0 };
  unsigned char wc[80] = { 0 }; *(u32*)wc = 80; *(void**)(wc + 8) = (void*)wndproc; *(const unsigned short**)(wc + 64) = cls;
  RegisterClassExW(wc);
  void* hwnd = CreateWindowExW(0, cls, cls, 0xCF0000, 0, 0, 1280, 720, 0, 0, 0, 0); ShowWindow(hwnd, 5);
  u32 d[18] = { 0 }; d[4] = 60; d[5] = 1; d[6] = 28; d[7] = 1; d[9] = 0x20; d[10] = 2; *(void**)&d[12] = hwnd; d[14] = 1; d[15] = 1;
  void *sc = 0, *dev = 0, *ctx = 0; u32 fl = 0; u32 want[2] = { 0xB000, 0xA000 };
  HR hr = D3D11CreateDeviceAndSwapChain(0, 1, 0, 0, want, 2, 7, d, &sc, &dev, &fl, &ctx);
  if (hr) { printf("create failed %x\n", hr); exit(1); }
  unsigned char iid[16] = { 0xf2, 0xaa, 0x15, 0x6f }; void *bb = 0, *rtv = 0;
  hr = ((HR(*)(void*, u32, const void*, void**))VT(sc)[9])(sc, 0, iid, &bb);
  hr |= ((HR(*)(void*, void*, void*, void**))VT(dev)[9])(dev, bb, 0, &rtv);
  if (hr) { printf("setup failed %x\n", hr); exit(2); }
  ((void(*)(void*, u32, void**, void*))VT(ctx)[33])(ctx, 1, &rtv, 0);
  u32 w = 0, h = 0; { unsigned char td[44]; ((void(*)(void*, void*))VT(bb)[10])(bb, td); w = *(u32*)td; h = *(u32*)(td + 4); }
  int frames = 0;
  for (int f = 0; f < 120; f++) {
    float col[4] = { (f % 60) / 60.0f, ((f * 2) % 60) / 60.0f, 0.3f, 1.0f };
    ((void(*)(void*, void*, const float*))VT(ctx)[50])(ctx, rtv, col);
    unsigned char msg[64]; while (PeekMessageW(msg, 0, 0, 0, 1)) {}
    ((HR(*)(void*, u32, u32))VT(sc)[8])(sc, 1, 0); frames++;
  }
  printf("d3d11 test: %d frames presented, backbuffer %ux%u, feature level 0x%x\n", frames, w, h, fl);
  exit(0); return 0;
}
