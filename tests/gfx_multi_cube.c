typedef unsigned long long u64;
typedef unsigned u32;
typedef long HR;
typedef struct { float x, y, z; } Position;
typedef struct { float u, v; } Texcoord;
typedef struct { u32 byte_width, usage, bind_flags, cpu_access, misc_flags, stride; } BufferDesc;
typedef struct { const void *data; u32 pitch, slice_pitch; } InitialData;
typedef struct {
  const unsigned short *semantic;
  u32 semantic_index, format, slot, offset, slot_class, step_rate;
} InputElement;
typedef struct { float value[16]; } MatrixConstants;
typedef struct { float value[8]; } MaterialConstants;

#include "gfx_multi_cube_shaders.h"

__declspec(dllimport) int printf(const char *, ...);
__declspec(dllimport) void exit(int);
__declspec(dllimport) double sin(double);
__declspec(dllimport) double cos(double);
__declspec(dllimport) double tan(double);
__declspec(dllimport) HR D3D11CreateDeviceAndSwapChain(
  void *, u32, void *, u32, const u32 *, u32, u32, void *, void **, void **, u32 *, void **);
__declspec(dllimport) HR D3D10CreateDevice(
  void *, u32, void *, u32, u32, void **);
__declspec(dllimport) HR D3D10CreateDeviceAndSwapChain(
  void *, u32, void *, u32, u32, void *, void **, void **);
__declspec(dllimport) u32 RegisterClassExW(void *);
__declspec(dllimport) void *CreateWindowExW(
  u32, const unsigned short *, const unsigned short *, u32, int, int, int, int,
  void *, void *, void *, void *);
__declspec(dllimport) int ShowWindow(void *, int);
__declspec(dllimport) int PeekMessageW(void *, void *, u32, u32, u32);
__declspec(dllimport) u32 GetTickCount(void);

#define VT(object) (*(void ***)(object))
#define OBJECT_COUNT 3000
#define FRAME_COUNT 600
#define VERTEX_COUNT 36
#define TEXTURE_SIZE 16

static Position positions[VERTEX_COUNT];
static Texcoord texcoords[VERTEX_COUNT];
static unsigned char texture_pixels[4][TEXTURE_SIZE * TEXTURE_SIZE * 4];
static const unsigned short class_name[] = { 'A', 'O', 'T', '1', '0', '0', 'C', 'u', 'b', 'e', 0 };
static const unsigned short semantic_position[] = { 'P', 'O', 'S', 'I', 'T', 'I', 'O', 'N', 0 };
static const unsigned short semantic_texcoord[] = { 'T', 'E', 'X', 'C', 'O', 'O', 'R', 'D', 0 };
static void *swapchain, *device, *context, *vertex_shader, *pixel_shader;
static void *input_layouts[2], *vertex_buffers[2], *constant_buffers[4];
static void *textures[4], *texture_views[4], *samplers[4], *backbuffer, *render_target;
static u32 frames_presented, rendering_errors;
static u64 draw_calls_submitted;
int _fltused;

static void fail(const char *message, HR hr);

static int release_object(void **object, const char *name) {
  if (!*object) return 1;
  u32 remaining = ((u32 (*)(void *))VT(*object)[2])(*object);
  *object = 0;
  if (remaining) {
    printf("cleanup: FAIL: %s has %u references\n", name, remaining);
    return 0;
  }
  return 1;
}

static void check_d3d10_entrypoints(void) {
  void *d3d10_device = 0;
  HR hr = D3D10CreateDevice(0, 1, 0, 0, 0x20, &d3d10_device);
  if (hr || !d3d10_device) fail("D3D10CreateDevice compatibility", hr);
  if (((HR (*)(void *))VT(d3d10_device)[63])(d3d10_device) != 0 ||
      ((u32 (*)(void *))VT(d3d10_device)[94])(d3d10_device) != 0)
    fail("D3D10 device vtable", -1);
  if (((u32 (*)(void *))VT(d3d10_device)[2])(d3d10_device))
    fail("D3D10 device cleanup", -1);

  u32 swap_desc[18] = { 0 };
  swap_desc[4] = 28;
  swap_desc[7] = 1;
  swap_desc[9] = 0x20;
  swap_desc[10] = 2;
  swap_desc[14] = 1;
  void *swapchain = 0;
  d3d10_device = 0;
  hr = D3D10CreateDeviceAndSwapChain(0, 1, 0, 0, 0x20, swap_desc,
      &swapchain, &d3d10_device);
  if (hr || !swapchain || !d3d10_device)
    fail("D3D10CreateDeviceAndSwapChain compatibility", hr);
  ((void (*)(void *))VT(d3d10_device)[69])(d3d10_device);
  ((void (*)(void *))VT(d3d10_device)[70])(d3d10_device);
  if (((u32 (*)(void *))VT(swapchain)[2])(swapchain))
    fail("D3D10 swapchain cleanup", -1);
  if (((u32 (*)(void *))VT(d3d10_device)[2])(d3d10_device))
    fail("D3D10 swapchain device cleanup", -1);
  printf("D3D10 compatibility: device and device/swapchain entrypoints PASS\n");
}

static void cleanup(void) {
  int clean = 1;
  if (context) ((void (*)(void *))VT(context)[110])(context);
  clean &= release_object(&render_target, "render target");
  clean &= release_object(&swapchain, "swapchain");
  clean &= release_object(&backbuffer, "backbuffer");
  clean &= release_object(&input_layouts[0], "input layout 0");
  clean &= release_object(&input_layouts[1], "input layout 1");
  clean &= release_object(&vertex_buffers[0], "position vertex buffer");
  clean &= release_object(&vertex_buffers[1], "texcoord vertex buffer");
  for (u32 i = 0; i < 4; i++) {
    clean &= release_object(&constant_buffers[i], "constant buffer");
    clean &= release_object(&texture_views[i], "texture SRV");
    clean &= release_object(&textures[i], "texture");
    clean &= release_object(&samplers[i], "sampler");
  }
  clean &= release_object(&vertex_shader, "vertex shader");
  clean &= release_object(&pixel_shader, "pixel shader");
  clean &= release_object(&device, "D3D11 device");
  clean &= release_object(&context, "D3D11 immediate context");
  printf("cleanup: %s\n", clean ? "PASS" : "FAIL");
}

static void fail(const char *message, HR hr) {
  printf("3000-cube test: FAIL: %s (HRESULT 0x%x), frames=%u errors=%u\n",
      message, (u32)hr, frames_presented, rendering_errors + 1);
  rendering_errors++;
  cleanup();
  exit(1);
}

static void identity(float matrix[16]) {
  for (u32 i = 0; i < 16; i++) matrix[i] = 0.0f;
  matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

static void multiply(float out[16], const float a[16], const float b[16]) {
  float result[16];
  for (u32 row = 0; row < 4; row++)
    for (u32 col = 0; col < 4; col++) {
      result[row * 4 + col] = 0.0f;
      for (u32 k = 0; k < 4; k++)
        result[row * 4 + col] += a[row * 4 + k] * b[k * 4 + col];
    }
  for (u32 i = 0; i < 16; i++) out[i] = result[i];
}

static void rotations(float out[16], float ax, float ay, float scale,
                      float tx, float ty, float tz) {
  float rx[16], ry[16], c, s;
  identity(rx);
  c = (float)cos(ax); s = (float)sin(ax);
  rx[5] = c; rx[6] = s; rx[9] = -s; rx[10] = c;
  identity(ry);
  c = (float)cos(ay); s = (float)sin(ay);
  ry[0] = c; ry[2] = -s; ry[8] = s; ry[10] = c;
  multiply(out, rx, ry);
  for (u32 row = 0; row < 3; row++)
    for (u32 col = 0; col < 3; col++)
      out[row * 4 + col] *= scale;
  out[12] = tx; out[13] = ty; out[14] = tz;
}

static void make_cube(void) {
  static const float face[6][4][3] = {
    { { -1,-1, 1 }, { 1,-1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } },
    { { 1,-1,-1 }, { -1,-1,-1 }, { -1, 1,-1 }, { 1, 1,-1 } },
    { { -1,-1,-1 }, { -1,-1, 1 }, { -1, 1, 1 }, { -1, 1,-1 } },
    { { 1,-1, 1 }, { 1,-1,-1 }, { 1, 1,-1 }, { 1, 1, 1 } },
    { { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1,-1 }, { -1, 1,-1 } },
    { { -1,-1,-1 }, { 1,-1,-1 }, { 1,-1, 1 }, { -1,-1, 1 } }
  };
  static const float uv[4][2] = { { 0,1 }, { 1,1 }, { 1,0 }, { 0,0 } };
  static const u32 corners[6] = { 0,1,2, 0,2,3 };
  u32 index = 0;
  for (u32 f = 0; f < 6; f++)
    for (u32 i = 0; i < 6; i++) {
      u32 corner = corners[i];
      positions[index].x = face[f][corner][0];
      positions[index].y = face[f][corner][1];
      positions[index].z = face[f][corner][2];
      texcoords[index].u = uv[corner][0];
      texcoords[index].v = uv[corner][1];
      index++;
    }
}

static void make_textures(void) {
  static const unsigned char colors[4][3] = {
    { 245, 60, 55 }, { 35, 205, 235 }, { 245, 205, 45 }, { 65, 75, 235 }
  };
  for (u32 texture = 0; texture < 4; texture++)
    for (u32 y = 0; y < TEXTURE_SIZE; y++)
      for (u32 x = 0; x < TEXTURE_SIZE; x++) {
        u32 offset = (y * TEXTURE_SIZE + x) * 4;
        unsigned char factor = ((x / 4 + y / 4 + texture) & 1) ? 255 : 125;
        for (u32 channel = 0; channel < 3; channel++)
          texture_pixels[texture][offset + channel] =
              (unsigned char)((colors[texture][channel] * factor) / 255);
        texture_pixels[texture][offset + 3] = 255;
      }
}

static HR create_buffer(u32 bytes, u32 bind, const void *initial, void **out) {
  BufferDesc desc = { bytes, 0, bind, 0, 0, 0 };
  InitialData data = { initial, 0, 0 };
  return ((HR (*)(void *, const BufferDesc *, const InitialData *, void **))VT(device)[3])(
      device, &desc, &data, out);
}

static HR update_buffer(void *buffer, const void *data) {
  ((void (*)(void *, void *, u32, const void *, const void *, u32, u32))VT(context)[48])(
      context, buffer, 0, 0, data, 0, 0);
  return 0;
}

int mainCRTStartup(void) {
  make_cube();
  make_textures();
  check_d3d10_entrypoints();
  unsigned char wc[80] = { 0 };
  *(u32 *)wc = sizeof(wc);
  *(const unsigned short **)(wc + 64) = class_name;
  RegisterClassExW(wc);
  void *window = CreateWindowExW(0, class_name, class_name, 0x00CF0000,
      0, 0, 1280, 720, 0, 0, 0, 0);
  ShowWindow(window, 5);
  u32 swap_desc[18] = { 0 };
  swap_desc[4] = 28; swap_desc[7] = 1; swap_desc[9] = 0x20;
  swap_desc[10] = 2; *(void **)&swap_desc[12] = window; swap_desc[14] = 1;
  const u32 levels[2] = { 0xB000, 0xA000 };
  u32 feature_level = 0;
  HR hr = D3D11CreateDeviceAndSwapChain(0, 1, 0, 0, levels, 2, 7,
      swap_desc, &swapchain, &device, &feature_level, &context);
  if (hr) fail("device/swapchain creation", hr);

  hr = ((HR (*)(void *, const void *, u64, void *, void **))VT(device)[12])(
      device, aot_multi_cube_vs, aot_multi_cube_vs_size, 0, &vertex_shader);
  if (hr) fail("vertex shader translation", hr);
  hr = ((HR (*)(void *, const void *, u64, void *, void **))VT(device)[15])(
      device, aot_multi_cube_ps, aot_multi_cube_ps_size, 0, &pixel_shader);
  if (hr) fail("pixel shader translation", hr);

  InputElement layout0[2] = {
    { semantic_position, 0, 6, 0, 0, 0, 0 },
    { semantic_texcoord, 0, 16, 1, 0, 0, 0 }
  };
  InputElement layout1[2] = {
    { semantic_position, 0, 6, 1, 0, 0, 0 },
    { semantic_texcoord, 0, 16, 0, 0, 0, 0 }
  };
  hr = ((HR (*)(void *, const InputElement *, u32, const void *, u64, void **))VT(device)[11])(
      device, layout0, 2, aot_multi_cube_vs, aot_multi_cube_vs_size, &input_layouts[0]);
  if (hr) fail("first split-stream input layout", hr);
  hr = ((HR (*)(void *, const InputElement *, u32, const void *, u64, void **))VT(device)[11])(
      device, layout1, 2, aot_multi_cube_vs, aot_multi_cube_vs_size, &input_layouts[1]);
  if (hr) fail("second split-stream input layout", hr);
  hr = create_buffer(sizeof positions, 1, positions, &vertex_buffers[0]);
  if (hr) fail("position vertex buffer", hr);
  hr = create_buffer(sizeof texcoords, 1, texcoords, &vertex_buffers[1]);
  if (hr) fail("texcoord vertex buffer", hr);
  MatrixConstants initial_matrix;
  identity(initial_matrix.value);
  for (u32 i = 0; i < 3; i++) {
    hr = create_buffer(sizeof(MatrixConstants), 4, &initial_matrix, &constant_buffers[i]);
    if (hr) fail("matrix constant buffer", hr);
  }
  MaterialConstants initial_material = { { 1,1,1,1, 0,0,0,0 } };
  hr = create_buffer(sizeof(MaterialConstants), 4, &initial_material, &constant_buffers[3]);
  if (hr) fail("material constant buffer", hr);

  for (u32 i = 0; i < 4; i++) {
    u32 texture_desc[11] = { TEXTURE_SIZE, TEXTURE_SIZE, 1, 1, 28, 1, 0, 0, 8, 0, 0 };
    InitialData initial_texture = { texture_pixels[i], TEXTURE_SIZE * 4,
        sizeof texture_pixels[i] };
    hr = ((HR (*)(void *, const u32 *, const InitialData *, void **))VT(device)[5])(
        device, texture_desc, &initial_texture, &textures[i]);
    if (hr) fail("checker texture creation", hr);
    u32 view_desc[6] = { 28, 3, 0, 1, 0, 0 };
    hr = ((HR (*)(void *, void *, const u32 *, void **))VT(device)[7])(
        device, textures[i], view_desc, &texture_views[i]);
    if (hr) fail("texture SRV creation", hr);
    u32 sampler_desc[13] = { i & 1 ? 0 : 0x15, i & 1 ? 2 : 1,
        i & 1 ? 2 : 1, i & 1 ? 2 : 1, 1, 8, 0, 0, 0, 0, 0, 0, 0 };
    hr = ((HR (*)(void *, const u32 *, void **))VT(device)[23])(
        device, sampler_desc, &samplers[i]);
    if (hr) fail("sampler creation", hr);
  }

  unsigned char iid_texture2d[16] = { 0xf2, 0xaa, 0x15, 0x6f };
  hr = ((HR (*)(void *, u32, const void *, void **))VT(swapchain)[9])(
      swapchain, 0, iid_texture2d, &backbuffer);
  if (hr) fail("backbuffer retrieval", hr);
  hr = ((HR (*)(void *, void *, void *, void **))VT(device)[9])(
      device, backbuffer, 0, &render_target);
  if (hr) fail("render-target creation", hr);

  ((void (*)(void *, u32, void **, void *))VT(context)[33])(
      context, 1, &render_target, 0);
  ((void (*)(void *, u32, u32, void **))VT(context)[7])(
      context, 0, 4, constant_buffers);
  ((void (*)(void *, u32, u32, void **))VT(context)[8])(
      context, 0, 4, texture_views);
  ((void (*)(void *, u32, u32, void **))VT(context)[10])(
      context, 0, 4, samplers);
  ((void (*)(void *, u32))VT(context)[24])(context, 4);
  ((void (*)(void *, void *, void *, u32))VT(context)[11])(
      context, vertex_shader, 0, 0);
  ((void (*)(void *, void *, void *, u32))VT(context)[9])(
      context, pixel_shader, 0, 0);

  MatrixConstants view, projection;
  identity(view.value);
  view.value[14] = 20.0f;
  identity(projection.value);
  float aspect = 16.0f / 9.0f, near_plane = 0.1f, far_plane = 100.0f;
  float y_scale = 1.0f / (float)tan(0.436332313f);
  for (u32 i = 0; i < 16; i++) projection.value[i] = 0.0f;
  projection.value[0] = y_scale / aspect;
  projection.value[5] = y_scale;
  projection.value[10] = far_plane / (far_plane - near_plane);
  projection.value[11] = 1.0f;
  projection.value[14] = -(near_plane * far_plane) / (far_plane - near_plane);

  printf("3000-cube test: draws/frame=%u frames=%u streams=2 layouts=2 constant slots=b0-b3 textures=4 samplers=4\n",
      OBJECT_COUNT, FRAME_COUNT);
  u32 start_time = GetTickCount();
  for (u32 frame = 0; frame < FRAME_COUNT; frame++) {
    view.value[12] = (float)sin((double)frame * 0.01) * 0.25f;
    view.value[13] = (float)cos((double)frame * 0.008) * 0.15f;
    update_buffer(constant_buffers[1], &view);
    update_buffer(constant_buffers[2], &projection);
    for (u32 object = 0; object < OBJECT_COUNT; object++) {
      float x = ((float)(object % 60) - 29.5f) * 0.32f;
      float y = (24.5f - (float)(object / 60)) * 0.32f;
      float phase = (float)frame * (0.012f + (float)(object % 7) * 0.001f);
      MatrixConstants world;
      rotations(world.value, phase, phase * 1.37f, 0.12f,
          x, y, (float)sin((double)phase * 0.7) * 0.6f);
      MaterialConstants material = { {
        0.75f + (float)(object % 4) * 0.08f,
        0.75f + (float)((object / 4) % 4) * 0.06f,
        0.80f, 1.0f, (float)(object % 4), 0, 0, 0
      } };
      update_buffer(constant_buffers[0], &world);
      update_buffer(constant_buffers[3], &material);
      u32 layout_index = object & 1;
      void *bound_buffers[2] = {
        vertex_buffers[layout_index ? 1 : 0],
        vertex_buffers[layout_index ? 0 : 1]
      };
      u32 strides[2] = { layout_index ? sizeof(Texcoord) : sizeof(Position),
        layout_index ? sizeof(Position) : sizeof(Texcoord) };
      u32 offsets[2] = { 0, 0 };
      ((void (*)(void *, void *))VT(context)[17])(
          context, input_layouts[layout_index]);
      ((void (*)(void *, u32, u32, void **, const u32 *, const u32 *))VT(context)[18])(
          context, 0, 2, bound_buffers, strides, offsets);
      ((void (*)(void *, u32, u32))VT(context)[13])(context, VERTEX_COUNT, 0);
      draw_calls_submitted++;
    }
    float clear_color[4] = { 0.025f, 0.035f, 0.06f, 1.0f };
    ((void (*)(void *, void *, const float *))VT(context)[50])(
        context, render_target, clear_color);
    unsigned char message[64];
    while (PeekMessageW(message, 0, 0, 0, 1)) {}
    hr = ((HR (*)(void *, u32, u32))VT(swapchain)[8])(swapchain, 1, 0);
    if (hr) {
      fail("3000 D3D11 Draw calls / Vulkan frame submission", hr);
    }
    frames_presented++;
  }
  u32 elapsed = GetTickCount() - start_time;
  if (draw_calls_submitted != (u64)FRAME_COUNT * OBJECT_COUNT)
    fail("submitted draw-call count mismatch", -1);
  printf("3000-cube test: PASS frames_presented=%u actual_draw_calls=%llu rendering_errors=%u elapsed_ms=%u average_fps=%.2f\n",
      frames_presented, draw_calls_submitted, rendering_errors,
      elapsed, elapsed ? (float)frames_presented * 1000.0f / (float)elapsed : 0.0f);
  printf("resource usage: 2 vertex buffers, 2 input layouts, 4 constant buffers, 4 textures/SRVs, 4 samplers; cleanup follows\n");
  cleanup();
  exit(0);
  return 0;
}
