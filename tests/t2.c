#ifdef _WIN32
__declspec(dllimport) int printf(const char*,...);
__declspec(dllimport) void*malloc(unsigned long long);__declspec(dllimport) void free(void*);
__declspec(dllimport) void*memcpy(void*,const void*,unsigned long long);__declspec(dllimport) unsigned long long strlen(const char*);
__declspec(dllimport) double sqrt(double);__declspec(dllimport) double sin(double);
__declspec(dllimport) void exit(int);
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#endif
typedef struct Node{int v;struct Node*next;}Node;
typedef struct{float x,y,z;}V3;
static V3 add(V3 a,V3 b){V3 r={a.x+b.x,a.y+b.y,a.z+b.z};return r;}
static float dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static unsigned long long mulhi(unsigned long long a,unsigned long long b){return (unsigned long long)(((unsigned __int128)a*b)>>64);}
static int cmpi(const void*a,const void*b){return *(const int*)a-*(const int*)b;}
static void isort(int*a,int n){for(int i=1;i<n;i++){int k=a[i],j=i-1;while(j>=0&&a[j]>k){a[j+1]=a[j];j--;}a[j+1]=k;}}
static int (*ops[4])(int,int);
static int o0(int a,int b){return a+b;}static int o1(int a,int b){return a*b;}static int o2(int a,int b){return a-b;}static int o3(int a,int b){return b?a/b:0;}
static char buf[256];
int main(void){
  Node*head=0;for(int i=0;i<10;i++){Node*n=malloc(sizeof(Node));n->v=i*i;n->next=head;head=n;}
  int s=0;for(Node*n=head;n;n=n->next)s+=n->v;printf("list sum=%d\n",s);while(head){Node*n=head->next;free(head);head=n;}
  V3 a={1.5f,2.25f,-3.f},b={0.5f,1.f,2.f};V3 c=add(a,b);printf("v3 %.3f %.3f %.3f dot=%.4f\n",c.x,c.y,c.z,dot(a,b));
  printf("sqrt=%.6f sin=%.6f\n",sqrt(2.0),sin(1.0));
  printf("mulhi=%llu\n",mulhi(0xDEADBEEFCAFEBABEULL,0x123456789ABCDEF1ULL));
  int arr[40];for(int i=0;i<40;i++)arr[i]=(i*7919)%101;isort(arr,40);printf("sorted %d %d %d %d\n",arr[0],arr[13],arr[27],arr[39]);
  ops[0]=o0;ops[1]=o1;ops[2]=o2;ops[3]=o3;int acc=7;for(int i=0;i<20;i++)acc=ops[i&3](acc,i+3);printf("fnptr acc=%d\n",acc);
  const char*str="hello, recompiler";memcpy(buf,str,strlen(str)+1);buf[0]='H';printf("%s len=%llu %5d|%-5d|%05d|%x|%c|%lld\n",buf,(unsigned long long)strlen(buf),42,42,42,255,'Z',-5000000000LL);
  double dd=0;for(int i=1;i<=1000;i++)dd+=1.0/i;printf("harm=%.10f\n",dd);
  unsigned u=0xF0F0u;int bits=0;while(u){bits+=u&1;u>>=1;}printf("bits=%d  %d\n",bits,(int)(-17>>2));
  long long big=1;for(int i=0;i<20;i++)big=big*3+i;printf("big=%lld  %lld\n",big,big%1000003);
  return 3;}
#ifdef _WIN32
int _fltused;
int mainCRTStartup(void){exit(main());return 0;}
#endif
