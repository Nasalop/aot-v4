/* PE32 Direct3D 10: device+swapchain, GetBuffer, RTV, Clear, Present through 32-bit COM vtables (stdcall) */
typedef unsigned int u32; typedef long long i64;
__declspec(dllimport) void* __stdcall GetStdHandle(u32); __declspec(dllimport) int __stdcall WriteFile(void*, const void*, u32, u32*, void*);
__declspec(dllimport) void __stdcall ExitProcess(u32);
__declspec(dllimport) long __stdcall D3D10CreateDeviceAndSwapChain(void*,u32,void*,u32,u32,void*,void**,void**);
int _fltused;
static void out(const char*s){u32 n=0,w;while(s[n])n++;WriteFile(GetStdHandle((u32)-11),s,n,&w,0);}
static void num(i64 v){char b[32];int i=31;b[i]=0;int neg=v<0;unsigned long long u=neg?-v:v;do{b[--i]='0'+(char)(u%10);u/=10;}while(u);if(neg)b[--i]='-';out(b+i);}
typedef long (__stdcall *M)(void*,...);
#define VT(o,i) (((void***)(o))[0][i])
typedef long (__stdcall *F2)(void*,void*); typedef long (__stdcall *F3)(void*,void*,void*); typedef long (__stdcall *F4)(void*,void*,void*,void*);
#define C2(o,i,a) ((F2)VT(o,i))(o,(void*)(a))
#define C3(o,i,a,b) ((F3)VT(o,i))(o,(void*)(a),(void*)(b))
#define C4(o,i,a,b,c) ((F4)VT(o,i))(o,(void*)(a),(void*)(b),(void*)(c))
static const u32 IID_tex2d[4]={0x9B7E4C05,0x342C4106,0xA19FA5A4,0x6A7D4B8A}; /* any IID: runtime ignores it */
int mainCRTStartup(void){
  u32 desc[15]={0}; desc[0]=640;desc[1]=360;desc[4]=28;desc[7]=1;desc[9]=0x20;desc[10]=1;desc[11]=0x1234;desc[12]=1;   /* BufferDesc W,H,Fmt; SampleDesc.Count; Usage; Count; HWND@44; Windowed@48 */
  void *sc=0,*dev=0,*tex=0,*rtv=0; long hr=D3D10CreateDeviceAndSwapChain(0,1,0,0,29,desc,&sc,&dev);
  out("create hr=");num(hr);out(" objs_low=");num((u32)(unsigned long)sc<0xFFFFFFFFu && (u32)(unsigned long)dev!=0);out("\n");
  hr=C4(sc,9,0,IID_tex2d,&tex); out("getbuffer hr=");num(hr);out("");out("\n");           /* IDXGISwapChain::GetBuffer */
  hr=C4(dev,76,tex,0,&rtv); out("creatertv hr=");num(hr);out("\n");               /* ID3D10Device::CreateRenderTargetView (slot 76 in d3d10 headers) */
  C4(dev,24,1,&rtv,0);                                                              /* OMSetRenderTargets */
  float col[4]={0.2f,0.4f,0.6f,1.0f}; for(int f=0;f<3;f++){ col[0]=0.2f*(f+1); C3(dev,35,rtv,col); hr=C3(sc,8,0,0); }   /* ClearRenderTargetView, Present */
  out("present hr=");num(hr);out("\n"); u32 desc2[15]={0}; C2(sc,12,desc2); out("getdesc w=");num(desc2[0]);out(" h=");num(desc2[1]);out("\n");
  ExitProcess(4);return 0;}
