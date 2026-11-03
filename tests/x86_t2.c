/* PE32: cdecl CRT imports (x87 double args/returns), low malloc, and command-line import */
typedef unsigned int u32; typedef long long i64;
__declspec(dllimport) void* __stdcall GetStdHandle(u32);
__declspec(dllimport) int __stdcall WriteFile(void*, const void*, u32, u32*, void*);
__declspec(dllimport) void __stdcall ExitProcess(u32);
__declspec(dllimport) char* __stdcall GetCommandLineA(void);
__declspec(dllimport) double __cdecl sqrt(double); __declspec(dllimport) double __cdecl pow(double,double); __declspec(dllimport) double __cdecl atan2(double,double);
__declspec(dllimport) void* __cdecl malloc(unsigned); __declspec(dllimport) unsigned __cdecl strlen(const char*); __declspec(dllimport) void* __cdecl memcpy(void*,const void*,unsigned);
int _fltused;
static void out(const char*s){u32 n=0,w;while(s[n])n++;WriteFile(GetStdHandle((u32)-11),s,n,&w,0);}
static void num(i64 v){char b[32];int i=31;b[i]=0;int neg=v<0;unsigned long long u=neg?-v:v;do{b[--i]='0'+(char)(u%10);u/=10;}while(u);if(neg)b[--i]='-';out(b+i);}
int mainCRTStartup(void){
  out("sqrt2e6=");num((i64)(sqrt(2.0)*1e6));out(" pow=");num((i64)pow(3.0,4.0));out(" atan2e6=");num((i64)(atan2(1.0,1.0)*1e6));out("\n");
  char*m=(char*)malloc(32);memcpy(m,"hello",6);out("len=");num(strlen(m));out(" low=");num((u32)(unsigned long)m<0xFFFFFFFFu);out("\n");
  out("calling GetCommandLineA\n"); char*c=GetCommandLineA(); out(c); ExitProcess(0); return 0;}
