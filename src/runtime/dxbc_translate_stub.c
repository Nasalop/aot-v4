#include "dxbc_translate.h"

int aot_dxbc_to_spirv(const void *data, size_t size, uint32_t **spirv, size_t *spirv_size) {
  (void)data;
  (void)size;
  if (spirv)
    *spirv = 0;
  if (spirv_size)
    *spirv_size = 0;
  return AOT_DXBC_UNAVAILABLE;
}

void aot_dxbc_free_spirv(uint32_t *spirv) {
  (void)spirv;
}
