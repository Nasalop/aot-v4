/* Bounded D3D11 graphics stress test. Build shaders with fxc via the GitHub workflow. */
typedef unsigned long long u64;
typedef unsigned u32;
typedef long HR;
typedef struct { float x, y, z, r, g, b, a; } Vertex;
typedef struct { u32 byte_width, usage, bind_flags, cpu_access, misc_flags, stride; } BufferDesc;
typedef struct { const void *data; u32 pitch, slice_pitch; } InitialData;
typedef struct {
  const unsigned short *semantic;
  u32 semantic_index, format, slot, offset, slot_class, step_rate;
} InputElement;

#include "gfx_stress_shaders.h"

__declspec(dllimport) int printf(const char *, ...);
__declspec(dllimport) void exit(int);
__declspec(dllimport) HR D3D11CreateDeviceAndSwapChain(
  void *, u32, void *, u32, const u32 *, u32, u32, void *, void **, void **, u32 *, void **);
__declspec(dllimport) u32 RegisterClassExW(void *);
__declspec(dllimport) void *CreateWindowExW(
  u32, const unsigned short *, const unsigned short *, u32, int, int, int, int,
  void *, void *, void *, void *);
__declspec(dllimport) int ShowWindow(void *, int);
__declspec(dllimport) int PeekMessageW(void *, void *, u32, u32, u32);
__declspec(dllimport) u32 timeGetTime(void);

#define VT(object) (*(void ***)(object))
#define GRID_X 24
#define GRID_Y 16
#define FRAME_COUNT 60
#define VERTEX_COUNT (GRID_X * GRID_Y * 6)

static Vertex vertices[VERTEX_COUNT];
static const unsigned short class_name[] = { 'A', 'O', 'T', 'G', 'f', 'x', 0 };
static const unsigned short semantic_position[] = { 'P', 'O', 'S', 'I', 'T', 'I', 'O', 'N', 0 };
static const unsigned short semantic_color[] = { 'C', 'O', 'L', 'O', 'R', 0 };
int _fltused;

static void fail(const char *message, HR hr) {
  printf("gfx stress: FAIL: %s (HRESULT 0x%x)\n", message, (u32)hr);
  exit(1);
}

static void set_vertex(Vertex *vertex, u32 x, u32 y, u32 corner) {
  vertex->x = -1.0f + 2.0f * (float)x / (float)GRID_X;
  vertex->y = -1.0f + 2.0f * (float)y / (float)GRID_Y;
  vertex->z = 0.0f;
  vertex->r = (float)((x + corner) % GRID_X) / (float)(GRID_X - 1);
  vertex->g = (float)((y + corner * 3) % GRID_Y) / (float)(GRID_Y - 1);
  vertex->b = (float)((x + y + corner * 5) % (GRID_X + GRID_Y)) /
              (float)(GRID_X + GRID_Y - 1);
  vertex->a = 1.0f;
}

static void build_mesh(void) {
  u32 index = 0;
  for (u32 y = 0; y < GRID_Y; y++) {
    for (u32 x = 0; x < GRID_X; x++) {
      set_vertex(&vertices[index++], x, y, 0);
      set_vertex(&vertices[index++], x + 1, y, 1);
      set_vertex(&vertices[index++], x + 1, y + 1, 2);
      set_vertex(&vertices[index++], x, y, 0);
      set_vertex(&vertices[index++], x + 1, y + 1, 2);
      set_vertex(&vertices[index++], x, y + 1, 3);
    }
  }
}

int mainCRTStartup(void) {
  build_mesh();
  unsigned char window_class[80] = { 0 };
  *(u32 *)window_class = sizeof(window_class);
  *(const unsigned short **)(window_class + 64) = class_name;
  RegisterClassExW(window_class);
  void *window = CreateWindowExW(0, class_name, class_name, 0x00CF0000,
    0, 0, 1280, 720, 0, 0, 0, 0);
  ShowWindow(window, 5);

  u32 swap_desc[18] = { 0 };
  swap_desc[4] = 28;
  swap_desc[7] = 1;
  swap_desc[9] = 0x20;
  swap_desc[10] = 2;
  *(void **)&swap_desc[12] = window;
  swap_desc[14] = 1;
  swap_desc[15] = 0;
  const u32 feature_levels[2] = { 0xB000, 0xA000 };
  void *swapchain = 0, *device = 0, *context = 0;
  u32 feature_level = 0;
  HR hr = D3D11CreateDeviceAndSwapChain(0, 1, 0, 0, feature_levels, 2, 7,
    swap_desc, &swapchain, &device, &feature_level, &context);
  if (hr) fail("device/swapchain creation", hr);

  void *vertex_shader = 0, *pixel_shader = 0, *input_layout = 0, *vertex_buffer = 0;
  hr = ((HR (*)(void *, const void *, u64, void *, void **))VT(device)[12])(
    device, aot_stress_vs, aot_stress_vs_size, 0, &vertex_shader);
  if (hr) fail("vertex shader creation", hr);
  hr = ((HR (*)(void *, const void *, u64, void *, void **))VT(device)[15])(
    device, aot_stress_ps, aot_stress_ps_size, 0, &pixel_shader);
  if (hr) fail("pixel shader creation", hr);

  InputElement elements[2] = {
    { semantic_position, 0, 6, 0, 0, 0, 0 },
    { semantic_color, 0, 2, 0, 12, 0, 0 }
  };
  hr = ((HR (*)(void *, const InputElement *, u32, const void *, u64, void **))VT(device)[11])(
    device, elements, 2, aot_stress_vs, aot_stress_vs_size, &input_layout);
  if (hr) fail("input layout creation", hr);

  BufferDesc desc = { sizeof(vertices), 0, 1, 0, 0, 0 };
  InitialData initial = { vertices, 0, 0 };
  hr = ((HR (*)(void *, const BufferDesc *, const InitialData *, void **))VT(device)[3])(
    device, &desc, &initial, &vertex_buffer);
  if (hr) fail("vertex buffer creation", hr);

  unsigned char iid_texture2d[16] = { 0xf2, 0xaa, 0x15, 0x6f };
  void *backbuffer = 0, *render_target = 0;
  hr = ((HR (*)(void *, u32, const void *, void **))VT(swapchain)[9])(
    swapchain, 0, iid_texture2d, &backbuffer);
  if (hr) fail("backbuffer retrieval", hr);
  hr = ((HR (*)(void *, void *, void *, void **))VT(device)[9])(
    device, backbuffer, 0, &render_target);
  if (hr) fail("render-target creation", hr);

  ((void (*)(void *, u32, void **, void *))VT(context)[33])(
    context, 1, &render_target, 0);
  ((void (*)(void *, void *))VT(context)[17])(context, input_layout);
  u32 stride = sizeof(Vertex), offset = 0;
  ((void (*)(void *, u32, u32, void **, const u32 *, const u32 *))VT(context)[18])(
    context, 0, 1, &vertex_buffer, &stride, &offset);
  ((void (*)(void *, u32))VT(context)[24])(context, 4);
  ((void (*)(void *, void *, void *, u32))VT(context)[11])(context, vertex_shader, 0, 0);
  ((void (*)(void *, void *, void *, u32))VT(context)[9])(context, pixel_shader, 0, 0);

  printf("gfx stress: mesh=%u triangles/frame, frames=%u, feature level=0x%x\n",
    VERTEX_COUNT / 3, FRAME_COUNT, feature_level);
  u32 start = timeGetTime();
  for (u32 frame = 0; frame < FRAME_COUNT; frame++) {
    float clear_color[4] = {
      (float)(frame % 3) * 0.025f,
      (float)((frame + 1) % 3) * 0.025f,
      0.04f,
      1.0f
    };
    ((void (*)(void *, void *, const float *))VT(context)[50])(
      context, render_target, clear_color);
    ((void (*)(void *, u32, u32))VT(context)[13])(context, VERTEX_COUNT, 0);
    unsigned char message[64];
    while (PeekMessageW(message, 0, 0, 0, 1)) {}
    hr = ((HR (*)(void *, u32, u32))VT(swapchain)[8])(swapchain, 1, 0);
    if (hr) fail("draw/present", hr);
  }
  printf("gfx stress: PASS frames=%u triangles=%u elapsed_ms=%u\n",
    FRAME_COUNT, (VERTEX_COUNT / 3) * FRAME_COUNT, timeGetTime() - start);
  exit(0);
  return 0;
}
