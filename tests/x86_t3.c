/* PE32: locally-implemented Interlocked*, events, x87 reversed ops (fsubr/fdivr), batch-2 imports */
typedef unsigned int u32; typedef long long i64;
__declspec(dllimport) void* __stdcall GetStdHandle(u32); __declspec(dllimport) int __stdcall WriteFile(void*, const void*, u32, u32*, void*);
__declspec(dllimport) void __stdcall ExitProcess(u32);
__declspec(dllimport) long __stdcall InterlockedIncrement(volatile long*); __declspec(dllimport) long __stdcall InterlockedExchangeAdd(volatile long*,long); __declspec(dllimport) long __stdcall InterlockedExchange(volatile long*,long);
__declspec(dllimport) void* __stdcall CreateEventA(void*,int,int,const char*); __declspec(dllimport) int __stdcall SetEvent(void*); __declspec(dllimport) u32 __stdcall WaitForSingleObject(void*,u32);
__declspec(dllimport) void* __stdcall GetCurrentProcess(void);
int _fltused;
static void out(const char*s){u32 n=0,w;while(s[n])n++;WriteFile(GetStdHandle((u32)-11),s,n,&w,0);}
static void num(i64 v){char b[32];int i=31;b[i]=0;int neg=v<0;unsigned long long u=neg?-v:v;do{b[--i]='0'+(char)(u%10);u/=10;}while(u);if(neg)b[--i]='-';out(b+i);}
int mainCRTStartup(void){
  volatile long v=5; long a=InterlockedIncrement(&v), b=InterlockedExchangeAdd(&v,10), c=InterlockedExchange(&v,99);
  out("inter=");num(a);out(",");num(b);out(",");num(c);out(",");num(v);out("\n");
  void*e=CreateEventA(0,1,0,0); SetEvent(e); out("wait=");num(WaitForSingleObject(e,1000));out(" proc=");num(GetCurrentProcess()!=0);out("\n");
  double x=10.0,y=4.0,r1,r2,r3,r4;
  __asm__ volatile("fldl %1; fldl %2; fsubrp %%st,%%st(1); fstpl %0" : "=m"(r1) : "m"(y),"m"(x));        /* x-y or y-x per AT&T quirk: checked vs native */
  __asm__ volatile("fldl %1; fldl %2; fdivrp %%st,%%st(1); fstpl %0" : "=m"(r2) : "m"(y),"m"(x));
  __asm__ volatile("fldl %1; fsubrl %2; fstpl %0" : "=m"(r3) : "m"(y),"m"(x));
  __asm__ volatile("fldl %1; fdivrl %2; fstpl %0" : "=m"(r4) : "m"(y),"m"(x));
  out("x87rev=");num((i64)(r1*1000));out(",");num((i64)(r2*1000));out(",");num((i64)(r3*1000));out(",");num((i64)(r4*1000));out("\n");
  ExitProcess(3);return 0;}
