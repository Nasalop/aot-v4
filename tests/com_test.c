#include <assert.h>
#include <stdint.h>
#include "../src/runtime/gfx.h"
static uint64_t next_thunk = 0x6fff00000020ULL;
uint64_t aot_register_thunk(aot_host_fn fn, const char *name) { (void)fn; (void)name; return next_thunk += 16; }
int main(void) {
  CPU c = {0}; uint8_t stack[128] = {0}; uint64_t dev = 0, fl = 0, ctx = 0;
  c.r[4] = (uint64_t)(uintptr_t)(stack + 0); /* n=4,5,7 stack arguments */
  *(uint64_t *)(stack + 8 + 8 * 4) = (uint64_t)(uintptr_t)&dev;
  *(uint64_t *)(stack + 8 + 8 * 5) = (uint64_t)(uintptr_t)&fl;
  *(uint64_t *)(stack + 8 + 8 * 7) = (uint64_t)(uintptr_t)&ctx;
  assert(aot_d3d11_create_device_from_cpu(&c) == 0); assert(dev && ctx && fl == 0xB000);
  assert(*(uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)dev) > 0x6fff00000000ULL); assert(*(uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)ctx) > 0x6fff00000000ULL);
  return 0;
}
