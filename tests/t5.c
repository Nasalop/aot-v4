/* threads + sync + x87 + AVX + message loop + vectored SEH */
typedef unsigned long long u64; typedef unsigned int u32; typedef long long i64; typedef unsigned char u8;
#define K __declspec(dllimport) __stdcall
K void* GetStdHandle(u32); K int WriteFile(void*,const void*,u32,u32*,void*); K void ExitProcess(u32);
K void* CreateThread(void*,u64,u32(__stdcall*)(void*),void*,u32,u32*); K u32 WaitForSingleObject(void*,u32);
K int CloseHandle(void*); K void Sleep(u32); K void InitializeCriticalSection(void*); K void EnterCriticalSection(void*); K void LeaveCriticalSection(void*);
K void* AddVectoredExceptionHandler(u32,void*); K void RaiseException(u32,u32,u32,const u64*); K void* CreateEventA(void*,int,int,void*); K int SetEvent(void*);
K u32 TlsAlloc(void); K void* TlsGetValue(u32); K int TlsSetValue(u32,void*);
K void* RegisterClassExA(void*); K void* CreateWindowExA(u32,const char*,const char*,u32,int,int,int,int,void*,void*,void*,void*);
K int PeekMessageA(void*,void*,u32,u32,u32); K int TranslateMessage(void*); K i64 DispatchMessageA(void*); K int PostMessageA(void*,u32,u64,i64);
K void PostQuitMessage(int); K i64 DefWindowProcA(void*,u32,u64,i64); K int GetAsyncKeyState(int);
static void out(const char*s){u32 n=0,w;while(s[n])n++;WriteFile(GetStdHandle((u32)-11),s,n,&w,0);}
static void num(i64 v){char b[32];int i=31;b[i]=0;int neg=v<0;u64 u=neg?-v:v;do{b[--i]='0'+u%10;u/=10;}while(u);if(neg)b[--i]='-';out(b+i);}
static void kv(const char*k,i64 v){out(k);out("=");num(v);out("\n");}
int _fltused; static u8 cs[64]; static volatile int counter, plain; static u32 tls;
static u32 __stdcall worker(void*p){ TlsSetValue(tls,p); for(int i=0;i<1000;i++){ EnterCriticalSection(cs); plain++; LeaveCriticalSection(cs); __sync_fetch_and_add(&counter,1);} 
  return (u32)(u64)TlsGetValue(tls)*10; }
static int veh_hits;
static long __stdcall veh(void*ep){ veh_hits++; u64*rec=*(u64**)ep; (void)rec; return -1; }
static int wm_seen, wm_user; static i64 __stdcall wp(void*h,u32 m,u64 w,i64 l){ if(m==0x400+1){wm_user+=(int)w;return 5;} if(m==0x10||m==2){wm_seen++;PostQuitMessage(3);return 0;} return DefWindowProcA(h,m,w,l);}
int mainCRTStartup(void){
  /* threads */
  InitializeCriticalSection(cs); tls=TlsAlloc(); void*h[4]; 
  for(int i=0;i<4;i++) h[i]=CreateThread(0,0,worker,(void*)(u64)(i+1),0,0);
  u32 sum=0; for(int i=0;i<4;i++){ WaitForSingleObject(h[i],0xFFFFFFFF); extern K u32 GetExitCodeThread(void*,u32*); u32 ec=0; GetExitCodeThread(h[i],&ec); sum+=ec; CloseHandle(h[i]); }
  kv("counter",counter); kv("plain",plain); kv("exitsum",sum);
  /* x87 */
  double a=2.0,r1,r2; i64 iv; short cw;
  __asm__ volatile("fldl %2; fsqrt; fldl %2; fmulp; fstpl %0; " : "=m"(r1) : "m"(a), "m"(a));          /* sqrt(2)*2 */
  kv("x87sqrt*1e6",(i64)(r1*1e6));
  __asm__ volatile("fldl %1; fldpi; fmulp; fistpll %0" : "=m"(iv) : "m"(a));                              /* 2*pi ->6 */
  kv("x87pi2",iv);
  double sn=0.5,sc; __asm__ volatile("fldl %1; fsin; fstpl %0" : "=m"(sc) : "m"(sn)); kv("x87sin.5*1e6",(i64)(sc*1e6));
  __asm__ volatile("fldl %1; fld1; faddp; fld1; fld1; faddp; fdivrp; fstpl %0" : "=m"(r2) : "m"(a)); kv("x87div*1e6",(i64)(r2*1e6)); /* 2/(2+1)? */
  int lt; __asm__ volatile("fldl %1; fldl %2; fcomip %%st(1),%%st; fstp %%st(0); seta %b0; movzbl %b0,%0" : "=q"(lt) : "m"(a),"m"(sn) : "cc"); kv("x87cmp",lt);
  __asm__ volatile("fnstcw %0" : "=m"(cw)); kv("x87cw",cw&0x3f);
  /* AVX */
  float x[8]={1,2,3,4,5,6,7,8},y[8]={8,7,6,5,4,3,2,1},z[8]; 
  __asm__ volatile("vmovups (%0),%%ymm0; vmovups (%1),%%ymm1; vaddps %%ymm1,%%ymm0,%%ymm2; vmulps %%ymm0,%%ymm1,%%ymm3; vsubps %%ymm3,%%ymm2,%%ymm2; vmovups %%ymm2,(%2); vzeroupper" :: "r"(x),"r"(y),"r"(z) : "memory","xmm0","xmm1","xmm2","xmm3");
  i64 zs=0; for(int i=0;i<8;i++) zs=zs*3+(i64)z[i]+100; kv("avxps",zs);
  int ia[8]={1,2,3,4,5,6,7,8},ib[4]={10,20,30,40},ic[4]; 
  __asm__ volatile("vmovdqu (%0),%%xmm0; vmovdqu (%1),%%xmm1; vpaddd %%xmm1,%%xmm0,%%xmm2; vpxor %%xmm0,%%xmm2,%%xmm2; vpslld $2,%%xmm2,%%xmm2; vmovdqu %%xmm2,(%2)" :: "r"(ia),"r"(ib),"r"(ic):"memory","xmm0","xmm1","xmm2");
  kv("avxi",(i64)ic[0]*1000000+ic[1]*10000+ic[2]*100+ic[3]);
  double d1=1.5,d2=2.25,d3; __asm__ volatile("vmovsd %1,%%xmm0; vaddsd %2,%%xmm0,%%xmm1; vsqrtsd %%xmm1,%%xmm1,%%xmm1; vmovsd %%xmm1,%0" : "=m"(d3) : "m"(d1),"m"(d2):"xmm0","xmm1"); kv("avxsd*1e6",(i64)(d3*1e6));
  /* SEH-lite */
  AddVectoredExceptionHandler(1,(void*)veh); RaiseException(0xE0000001,0,0,0); RaiseException(0xE0000002,0,0,0); kv("veh",veh_hits);
  /* messages */
  u8 wc[80]; for(int i=0;i<80;i++)wc[i]=0; *(u32*)wc=80; *(void**)(wc+8)=(void*)wp; *(const char**)(wc+64)="T4";
  RegisterClassExA(wc); void*hw=CreateWindowExA(0,"T4","t",0x10CF0000,0,0,320,200,0,0,0,0);
  kv("hwnd_ok",hw!=0);
  PostMessageA(hw,0x401,7,0); PostMessageA(hw,0x401,5,0); PostMessageA(hw,0x10,0,0);
  u8 msg[64]; int n=0,quit=0; while(n<100 && PeekMessageA(msg,0,0,0,1)){ n++; if(*(u32*)(msg+8)==0x12){quit=1;break;} TranslateMessage(msg); DispatchMessageA(msg);} 
  kv("wm_user",wm_user); kv("wm_close",wm_seen); kv("quit",quit); kv("key_idle",GetAsyncKeyState(0x41));
  ExitProcess(9); return 0; }
