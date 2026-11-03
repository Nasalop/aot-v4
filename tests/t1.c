/* freestanding Win64 test: kernel32 only */
typedef unsigned long long u64; typedef unsigned int u32; typedef long long i64;
__declspec(dllimport) void* __stdcall GetStdHandle(u32);
__declspec(dllimport) int __stdcall WriteFile(void*, const void*, u32, u32*, void*);
__declspec(dllimport) void __stdcall ExitProcess(u32);
static void out(const char*s){u32 n=0,w;while(s[n])n++;WriteFile(GetStdHandle((u32)-11),s,n,&w,0);}
static void num(i64 v){char b[32];int i=31;b[i]=0;int neg=v<0;u64 u=neg?-v:v;do{b[--i]='0'+u%10;u/=10;}while(u);if(neg)b[--i]='-';out(b+i);}
static int fib(int n){return n<2?n:fib(n-1)+fib(n-2);}
static const char*names[]={"zero","one","two","three","four","five","six","seven"};
static int sw(int x){switch(x){case 0:return 10;case 1:return 21;case 2:return 33;case 3:return 47;case 4:return 52;case 5:return 68;case 6:return 71;default:return -1;}}
static double poly(double x){return x*x*3.0+x/7.0-1.5;}
struct P{int a;long long b;double d;};
int mainCRTStartup(void){
  out("fib(20)=");num(fib(20));out("\n");
  u64 h=1469598103934665603ULL;for(int i=0;i<1000;i++){h^=(u64)i;h*=1099511628211ULL;h=(h<<13)|(h>>51);}out("hash=");num((i64)(h%1000000007));out("\n");
  i64 s=0;for(int i=1;i<=100;i++){s+=(i*i)%7-(i/3)*(i&5);}out("sum=");num(s);out("\n");
  for(int i=0;i<9;i++){out(i<8?names[i]:"eight");out("=");num(sw(i));out(i<8?" ":"\n");}
  double d=poly(2.5);out("poly*1000=");num((i64)(d*1000.0));out("\n");
  int arr[50];for(int i=0;i<50;i++)arr[i]=(i*37)%50;for(int i=0;i<50;i++)for(int j=0;j<49-i;j++)if(arr[j]>arr[j+1]){int t=arr[j];arr[j]=arr[j+1];arr[j+1]=t;}
  out("sorted:");num(arr[0]);out(",");num(arr[25]);out(",");num(arr[49]);out("\n");
  struct P p={7,-9000000000LL,2.25};p.b*=3;p.d*=p.a;out("struct:");num(p.b);out(",");num((i64)p.d);out("\n");
  i64 q=-1234567;out("div:");num(q/1000);out(",");num(q%1000);out(",");num((i64)((u64)q/1000));out("\n");
  ExitProcess(7);return 0;}
