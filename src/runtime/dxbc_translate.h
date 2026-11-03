#ifndef AOT_DXBC_TRANSLATE_H
#define AOT_DXBC_TRANSLATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  AOT_DXBC_OK = 0,
  AOT_DXBC_INVALID_ARGUMENT = -1,
  AOT_DXBC_COMPILE_FAILED = -2,
  AOT_DXBC_OUT_OF_MEMORY = -3,
  AOT_DXBC_UNAVAILABLE = -4
};

int aot_dxbc_to_spirv(const void *data, size_t size, uint32_t **spirv, size_t *spirv_size);
void aot_dxbc_free_spirv(uint32_t *spirv);

#ifdef __cplusplus
}
#endif

#endif
