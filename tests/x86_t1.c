/* freestanding 32-bit Win32 test: stack/regs/stdcall/x87/jump table/heap/64-bit math */
typedef unsigned long long u64; typedef unsigned int u32; typedef long long i64;
__declspec(dllimport) void* __stdcall GetStdHandle(u32);
__declspec(dllimport) int __stdcall WriteFile(void*, const void*, u32, u32*, void*);
__declspec(dllimport) void __stdcall ExitProcess(u32);
__declspec(dllimport) void* __stdcall GetProcessHeap(void);
__declspec(dllimport) void* __stdcall HeapAlloc(void*, u32, u32);
__declspec(dllimport) int __stdcall HeapFree(void*, u32, void*);
__declspec(dllimport) void* __stdcall VirtualAlloc(void*, u32, u32, u32);
int _fltused;
static void out(const char*s){u32 n=0,w;while(s[n])n++;WriteFile(GetStdHandle((u32)-11),s,n,&w,0);}
static void num(i64 v){char b[32];int i=31;b[i]=0;int neg=v<0;u64 u=neg?-v:v;do{b[--i]='0'+(char)(u%10);u/=10;}while(u);if(neg)b[--i]='-';out(b+i);}
static int fib(int n){return n<2?n:fib(n-1)+fib(n-2);}
static int sw(int x){switch(x){case 0:return 10;case 1:return 21;case 2:return 33;case 3:return 47;case 4:return 52;case 5:return 68;case 6:return 71;default:return -1;}}
static double poly(double x){return x*x*3.0+x/7.0-1.5;}
__declspec(noinline) static int __fastcall fc(int a,int b,int c){return a*100+b*10+c;}
__declspec(noinline) static int __cdecl cd(int a,int b){return a-b;}
struct P{int a;long long b;double d;};
int mainCRTStartup(void){
  out("fib(20)=");num(fib(20));out("\n");
  u32 h=2166136261u;for(int i=0;i<1000;i++){h^=(u32)i;h*=16777619u;h=(h<<13)|(h>>19);}out("hash=");num(h%1000000007u);out("\n");
  for(int i=0;i<9;i++){out("sw=");num(sw(i));out(i<8?" ":"\n");}
  double d=poly(2.5);out("poly*1000=");num((i64)(d*1000.0));out("\n");
  out("fc=");num(fc(1,2,3));out(" cd=");num(cd(9,4));out("\n");
  struct P p={7,-9000000000LL,2.25};p.b*=3;p.d*=p.a;out("struct:");num(p.b);out(",");num((i64)p.d);out("\n");
  int*a=(int*)HeapAlloc(GetProcessHeap(),8,400);for(int i=0;i<100;i++)a[i]=(i*37)%100;
  for(int i=0;i<100;i++)for(int j=0;j<99-i;j++)if(a[j]>a[j+1]){int t=a[j];a[j]=a[j+1];a[j+1]=t;}
  out("sorted:");num(a[0]);out(",");num(a[50]);out(",");num(a[99]);out(" heap<4G=");num((u32)(unsigned long)a>0x1000);out("\n");HeapFree(GetProcessHeap(),0,a);
  i64 q=-1234567;q*=1000;out("mul64:");num(q);out(" sh:");num(q>>3);out("\n");
  ExitProcess(5);return 0;}
