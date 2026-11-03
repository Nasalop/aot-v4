#ifdef _WIN32
__declspec(dllimport) int printf(const char*,...);
__declspec(dllimport) void exit(int);
#endif
#ifndef _WIN32
int printf(const char*,...);
#endif
typedef unsigned int uint32_t;
int main(void){
  uint32_t a=0x80000001u,c;
  for(int cf=0;cf<2;cf++)for(int n=1;n<5;n++){
    uint32_t x=0x80000001u^(n*77),y=x; unsigned char f;
    __asm__ volatile("movb %4,%%al; addb $0xff,%%al; mov %3,%%ecx; rcll %%cl,%0; setc %1; mov %0,%2":"+r"(x),"=r"(f),"=r"(c):"r"(n),"r"((unsigned char)cf):"eax","ecx","cc");
    printf("rcl %d %d %08x %d\n",cf,n,x,f);
    __asm__ volatile("movb %4,%%al; addb $0xff,%%al; mov %3,%%ecx; rcrl %%cl,%0; setc %1; mov %0,%2":"+r"(y),"=r"(f),"=r"(c):"r"(n),"r"((unsigned char)cf):"eax","ecx","cc");
    printf("rcr %d %d %08x %d\n",cf,n,y,f);}
  float s[4]={1,2,3,4},t[4]={2,2,__builtin_nanf(""),5},r[4];
  __asm__ volatile("movups %1,%%xmm0; movups %2,%%xmm1; cmpnltps %%xmm1,%%xmm0; movups %%xmm0,%0":"=m"(r):"m"(s),"m"(t):"xmm0","xmm1");
  printf("nlt %08x %08x %08x %08x\n",((uint32_t*)r)[0],((uint32_t*)r)[1],((uint32_t*)r)[2],((uint32_t*)r)[3]);
  __asm__ volatile("movups %1,%%xmm0; movups %2,%%xmm1; cmpunordps %%xmm1,%%xmm0; movups %%xmm0,%0":"=m"(r):"m"(s),"m"(t):"xmm0","xmm1");
  printf("unord %08x %08x %08x %08x\n",((uint32_t*)r)[0],((uint32_t*)r)[1],((uint32_t*)r)[2],((uint32_t*)r)[3]);
  int cnt=0,k=5; __asm__ volatile("1: inc %0; loop 1b":"+r"(cnt),"+c"(k)::"cc"); printf("loop %d\n",cnt);
  return 0;}
#ifdef _WIN32
int mainCRTStartup(void){exit(main());return 0;}
#endif
