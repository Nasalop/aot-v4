#include "shims.h"

#define ARG(n) ((n) == 0 ? c->r[1] : (n) == 1 ? c->r[2] : \
    (n) == 2 ? c->r[8] : (n) == 3 ? c->r[9] : \
    LD(uint64_t, c->r[4] + 8 + 8 * (n)))

static uint64_t d3d10_create_device(CPU *c) {
  return aot_win_gfx_d3d10_create(ARG(5), 0, 0);
}

static uint64_t d3d10_create_device_and_swapchain(CPU *c) {
  return aot_win_gfx_d3d10_create(ARG(7), ARG(6), ARG(5));
}

static void clear_guest_pointer(uint64_t address) {
  if (!address) return;
  if (aot_is_pe32) ST(uint32_t, address, 0);
  else ST(uint64_t, address, 0);
}

static uint64_t d3d10_compile_shader(CPU *c) {
  clear_guest_pointer(ARG(8));
  clear_guest_pointer(ARG(9));
  fprintf(stderr, "[aot] D3D10CompileShader is unavailable: no HLSL compiler backend\n");
  return 0x80004001u; /* E_NOTIMPL: explicit failure, never a fake shader blob */
}

static uint64_t d3d10_reflect_shader(CPU *c) {
  clear_guest_pointer(ARG(2));
  fprintf(stderr, "[aot] D3D10ReflectShader is unavailable: no shader reflection backend\n");
  return 0x80004001u; /* E_NOTIMPL */
}

const ShimEnt aot_shim_table_d3d10[] = {
  { "D3D10CreateDevice", d3d10_create_device },
  { "D3D10CreateDeviceAndSwapChain", d3d10_create_device_and_swapchain },
  { "D3D10CompileShader", d3d10_compile_shader },
  { "D3D10ReflectShader", d3d10_reflect_shader },
  { 0, 0 }
};

#undef ARG
