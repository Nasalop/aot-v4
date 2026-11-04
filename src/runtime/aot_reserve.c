/* Preloaded into the app process (via lib/<abi>/wrap.sh -> LD_PRELOAD) before ART starts: reserves the address range a 32-bit
 * Windows image needs (0x400000..0x3000000). ART otherwise puts its heap at 0x02000000 and the fixed-base image cannot be mapped. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/mman.h>
#define AOT_RES_LO 0x400000UL
#define AOT_RES_HI 0x3000000UL
__attribute__((constructor(101))) static void aot_reserve_image_range(void) {
  void *p = mmap((void *)AOT_RES_LO, AOT_RES_HI - AOT_RES_LO, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
  if (p == (void *)AOT_RES_LO) { setenv("AOT_RESERVED_LO", "0x400000", 1); setenv("AOT_RESERVED_HI", "0x3000000", 1); }
}
