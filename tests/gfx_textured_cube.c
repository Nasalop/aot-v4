typedef unsigned long long u64;
typedef unsigned u32;
typedef long HR;
typedef struct { float x, y, z, u, v; } CubeVertex;
typedef struct { u32 byte_width, usage, bind_flags, cpu_access, misc_flags, stride; } BufferDesc;
typedef struct { const void *data; u32 pitch, slice_pitch; } InitialData;
typedef struct {
  const unsigned short *semantic;
  u32 semantic_index, format, slot, offset, slot_class, step_rate;
} InputElement;
typedef struct { float world_view_projection[16]; } TransformConstants;

#include "gfx_cube_shaders.h"

__declspec(dllimport) int printf(const char *, ...);
__declspec(dllimport) void exit(int);
__declspec(dllimport) double sin(double);
__declspec(dllimport) double cos(double);
__declspec(dllimport) double sqrt(double);
__declspec(dllimport) double tan(double);
__declspec(dllimport) HR D3D11CreateDeviceAndSwapChain(
  void *, u32, void *, u32, const u32 *, u32, u32, void *, void **, void **, u32 *, void **);
__declspec(dllimport) u32 RegisterClassExW(void *);
__declspec(dllimport) void *CreateWindowExW(
  u32, const unsigned short *, const unsigned short *, u32, int, int, int, int,
  void *, void *, void *, void *);
__declspec(dllimport) int ShowWindow(void *, int);
__declspec(dllimport) int PeekMessageW(void *, void *, u32, u32, u32);

#define VT(object) (*(void ***)(object))
#define TEXTURE_SIZE 32
#define FRAME_COUNT 600
#define VERTEX_COUNT 36

static CubeVertex vertices[VERTEX_COUNT];
static unsigned char texture_pixels[TEXTURE_SIZE * TEXTURE_SIZE * 4];
static const unsigned short class_name[] = { 'A', 'O', 'T', 'C', 'u', 'b', 'e', 0 };
static const unsigned short semantic_position[] = { 'P', 'O', 'S', 'I', 'T', 'I', 'O', 'N', 0 };
static const unsigned short semantic_texcoord[] = { 'T', 'E', 'X', 'C', 'O', 'O', 'R', 'D', 0 };
static void *swapchain, *device, *context, *vertex_shader, *pixel_shader;
static void *input_layout, *vertex_buffer, *constant_buffer;
static void *texture, *texture_view, *sampler, *backbuffer, *render_target;
static u32 frames_presented, rendering_errors;
int _fltused;

static int release_object(void **object, const char *name) {
  if (!*object) return 1;
  u32 remaining = ((u32 (*)(void *))VT(*object)[2])(*object);
  *object = 0;
  if (remaining) {
    printf("resource cleanup: FAIL: %s still has %u references\n", name, remaining);
    return 0;
  }
  printf("resource cleanup: released %s\n", name);
  return 1;
}

static void cleanup(void) {
  int clean = 1;
  if (context) ((void (*)(void *))VT(context)[110])(context);
  clean &= release_object(&render_target, "render target");
  clean &= release_object(&swapchain, "swapchain");
  clean &= release_object(&backbuffer, "backbuffer");
  clean &= release_object(&input_layout, "input layout");
  clean &= release_object(&vertex_buffer, "vertex buffer");
  clean &= release_object(&constant_buffer, "transform constant buffer");
  clean &= release_object(&texture_view, "texture shader-resource view");
  clean &= release_object(&texture, "checker texture");
  clean &= release_object(&sampler, "texture sampler");
  clean &= release_object(&vertex_shader, "vertex shader");
  clean &= release_object(&pixel_shader, "pixel shader");
  clean &= release_object(&device, "D3D11 device");
  clean &= release_object(&context, "D3D11 immediate context");
  printf("resource cleanup: %s\n", clean ? "PASS" : "FAIL");
}

static void fail(const char *message, HR hr) {
  printf("textured cube: FAIL: %s (HRESULT 0x%x), frames presented=%u\n",
    message, (u32)hr, frames_presented);
  printf("rendering errors=%u\n", rendering_errors);
  cleanup();
  exit(1);
}

static void matrix_identity(float out[16]) {
  for (u32 i = 0; i < 16; i++) out[i] = 0.0f;
  out[0] = out[5] = out[10] = out[15] = 1.0f;
}

static void matrix_multiply(float out[16], const float a[16], const float b[16]) {
  float result[16];
  for (u32 row = 0; row < 4; row++)
    for (u32 column = 0; column < 4; column++) {
      result[row * 4 + column] = 0.0f;
      for (u32 k = 0; k < 4; k++)
        result[row * 4 + column] += a[row * 4 + k] * b[k * 4 + column];
    }
  for (u32 i = 0; i < 16; i++) out[i] = result[i];
}

static void matrix_rotation_x(float out[16], float angle) {
  matrix_identity(out);
  float c = (float)cos(angle), s = (float)sin(angle);
  out[5] = c; out[6] = s; out[9] = -s; out[10] = c;
}

static void matrix_rotation_y(float out[16], float angle) {
  matrix_identity(out);
  float c = (float)cos(angle), s = (float)sin(angle);
  out[0] = c; out[2] = -s; out[8] = s; out[10] = c;
}

static void matrix_look_at_lh(float out[16], float ex, float ey, float ez) {
  float zx = -ex, zy = -ey, zz = -ez;
  float length = (float)sqrt((double)(zx * zx + zy * zy + zz * zz));
  zx /= length; zy /= length; zz /= length;
  float xx = zz, xy = 0.0f, xz = -zx;
  length = (float)sqrt((double)(xx * xx + xy * xy + xz * xz));
  xx /= length; xy /= length; xz /= length;
  float yx = zy * xz - zz * xy;
  float yy = zz * xx - zx * xz;
  float yz = zx * xy - zy * xx;
  matrix_identity(out);
  out[0] = xx; out[1] = yx; out[2] = zx;
  out[4] = xy; out[5] = yy; out[6] = zy;
  out[8] = xz; out[9] = yz; out[10] = zz;
  out[12] = -(ex * xx + ey * xy + ez * xz);
  out[13] = -(ex * yx + ey * yy + ez * yz);
  out[14] = -(ex * zx + ey * zy + ez * zz);
}

static void matrix_perspective_lh(float out[16], float aspect) {
  const float near_plane = 0.1f, far_plane = 100.0f;
  const float y_scale = 1.0f / (float)tan(0.78539816339);
  const float x_scale = y_scale / aspect;
  for (u32 i = 0; i < 16; i++) out[i] = 0.0f;
  out[0] = x_scale; out[5] = y_scale;
  out[10] = far_plane / (far_plane - near_plane);
  out[11] = 1.0f;
  out[14] = -(near_plane * far_plane) / (far_plane - near_plane);
}

static void build_transform(TransformConstants *constants, u32 frame) {
  float rotation_x[16], rotation_y[16], world[16];
  float view[16], projection[16], world_view[16];
  float angle_x = (float)frame * 0.025f;
  float angle_y = (float)frame * 0.037f;
  float camera_phase = (float)frame * 0.012f;
  float camera_x = (float)sin(camera_phase) * 1.1f;
  float camera_y = (float)sin(camera_phase * 0.63f) * 0.45f;
  float camera_z = -6.0f + (float)cos(camera_phase * 0.5f) * 0.35f;
  matrix_rotation_x(rotation_x, angle_x);
  matrix_rotation_y(rotation_y, angle_y);
  matrix_multiply(world, rotation_x, rotation_y);
  matrix_look_at_lh(view, camera_x, camera_y, camera_z);
  matrix_perspective_lh(projection, 16.0f / 9.0f);
  matrix_multiply(world_view, world, view);
  matrix_multiply(constants->world_view_projection, world_view, projection);
}

static void build_cube(void) {
  static const float faces[6][4][3] = {
    { { -1, -1,  1 }, {  1, -1,  1 }, {  1,  1,  1 }, { -1,  1,  1 } },
    { {  1, -1, -1 }, { -1, -1, -1 }, { -1,  1, -1 }, {  1,  1, -1 } },
    { { -1, -1, -1 }, { -1, -1,  1 }, { -1,  1,  1 }, { -1,  1, -1 } },
    { {  1, -1,  1 }, {  1, -1, -1 }, {  1,  1, -1 }, {  1,  1,  1 } },
    { { -1,  1,  1 }, {  1,  1,  1 }, {  1,  1, -1 }, { -1,  1, -1 } },
    { { -1, -1, -1 }, {  1, -1, -1 }, {  1, -1,  1 }, { -1, -1,  1 } }
  };
  static const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
  static const u32 corners[6] = { 0, 1, 2, 0, 2, 3 };
  u32 vertex = 0;
  for (u32 face = 0; face < 6; face++)
    for (u32 i = 0; i < 6; i++) {
      u32 corner = corners[i];
      CubeVertex *out = &vertices[vertex++];
      out->x = faces[face][corner][0];
      out->y = faces[face][corner][1];
      out->z = faces[face][corner][2];
      out->u = uv[corner][0]; out->v = uv[corner][1];
    }
}

static void build_texture(void) {
  static const unsigned char colors[4][4] = {
    { 240, 65, 50, 255 }, { 35, 205, 230, 255 },
    { 250, 210, 45, 255 }, { 40, 70, 220, 255 }
  };
  for (u32 y = 0; y < TEXTURE_SIZE; y++)
    for (u32 x = 0; x < TEXTURE_SIZE; x++) {
      u32 cell = ((x / 4) + (y / 4)) % 4;
      u32 offset = (y * TEXTURE_SIZE + x) * 4;
      for (u32 channel = 0; channel < 4; channel++)
        texture_pixels[offset + channel] = colors[cell][channel];
    }
}

int mainCRTStartup(void) {
  build_cube();
  build_texture();
  unsigned char window_class[80] = { 0 };
  *(u32 *)window_class = sizeof(window_class);
  *(const unsigned short **)(window_class + 64) = class_name;
  RegisterClassExW(window_class);
  void *window = CreateWindowExW(0, class_name, class_name, 0x00CF0000,
    0, 0, 1280, 720, 0, 0, 0, 0);
  ShowWindow(window, 5);

  u32 swap_desc[18] = { 0 };
  swap_desc[4] = 28; swap_desc[7] = 1; swap_desc[9] = 0x20;
  swap_desc[10] = 2; *(void **)&swap_desc[12] = window; swap_desc[14] = 1;
  const u32 feature_levels[2] = { 0xB000, 0xA000 };
  u32 feature_level = 0;
  HR hr = D3D11CreateDeviceAndSwapChain(0, 1, 0, 0, feature_levels, 2, 7,
    swap_desc, &swapchain, &device, &feature_level, &context);
  if (hr) fail("device/swapchain creation", hr);

  hr = ((HR (*)(void *, const void *, u64, void *, void **))VT(device)[12])(
    device, aot_cube_vs, aot_cube_vs_size, 0, &vertex_shader);
  if (hr) {
    printf("shader compilation: FAIL (vertex DXBC-to-SPIR-V, HRESULT 0x%x)\n", (u32)hr);
    fail("vertex shader DXBC-to-SPIR-V compilation", hr);
  }
  hr = ((HR (*)(void *, const void *, u64, void *, void **))VT(device)[15])(
    device, aot_cube_ps, aot_cube_ps_size, 0, &pixel_shader);
  if (hr) {
    printf("shader compilation: FAIL (pixel DXBC-to-SPIR-V, HRESULT 0x%x)\n", (u32)hr);
    fail("pixel shader DXBC-to-SPIR-V compilation", hr);
  }
  printf("shader compilation: PASS (vertex and pixel DXBC translated to SPIR-V)\n");

  InputElement elements[2] = {
    { semantic_position, 0, 6, 0, 0, 0, 0 },
    { semantic_texcoord, 0, 16, 0, 12, 0, 0 }
  };
  hr = ((HR (*)(void *, const InputElement *, u32, const void *, u64, void **))VT(device)[11])(
    device, elements, 2, aot_cube_vs, aot_cube_vs_size, &input_layout);
  if (hr) fail("input layout creation", hr);

  BufferDesc vertex_desc = { sizeof(vertices), 0, 1, 0, 0, 0 };
  InitialData vertex_data = { vertices, 0, 0 };
  hr = ((HR (*)(void *, const BufferDesc *, const InitialData *, void **))VT(device)[3])(
    device, &vertex_desc, &vertex_data, &vertex_buffer);
  if (hr) fail("cube vertex buffer creation", hr);

  TransformConstants initial_transform;
  matrix_identity(initial_transform.world_view_projection);
  BufferDesc constants_desc = { sizeof(initial_transform), 0, 4, 0, 0, 0 };
  InitialData constants_data = { &initial_transform, 0, 0 };
  hr = ((HR (*)(void *, const BufferDesc *, const InitialData *, void **))VT(device)[3])(
    device, &constants_desc, &constants_data, &constant_buffer);
  if (hr) fail("transform constant-buffer creation", hr);

  u32 texture_desc[11] = { TEXTURE_SIZE, TEXTURE_SIZE, 1, 1, 28, 1, 0, 0, 8, 0, 0 };
  InitialData texture_data = { texture_pixels, TEXTURE_SIZE * 4, sizeof(texture_pixels) };
  hr = ((HR (*)(void *, const u32 *, const InitialData *, void **))VT(device)[5])(
    device, texture_desc, &texture_data, &texture);
  if (hr) fail("RGBA8 checker-texture creation", hr);
  u32 srv_desc[6] = { 28, 3, 0, 1, 0, 0 };
  hr = ((HR (*)(void *, void *, const u32 *, void **))VT(device)[7])(
    device, texture, srv_desc, &texture_view);
  if (hr) fail("texture shader-resource view creation", hr);
  u32 sampler_desc[13] = { 0x15, 1, 1, 1, 1, 8, 0, 0, 0, 0, 0, 0, 0 };
  hr = ((HR (*)(void *, const u32 *, void **))VT(device)[23])(
    device, sampler_desc, &sampler);
  if (hr) fail("linear wrap sampler creation", hr);

  unsigned char iid_texture2d[16] = { 0xf2, 0xaa, 0x15, 0x6f };
  hr = ((HR (*)(void *, u32, const void *, void **))VT(swapchain)[9])(
    swapchain, 0, iid_texture2d, &backbuffer);
  if (hr) fail("backbuffer retrieval", hr);
  hr = ((HR (*)(void *, void *, void *, void **))VT(device)[9])(
    device, backbuffer, 0, &render_target);
  if (hr) fail("render-target creation", hr);

  ((void (*)(void *, u32, void **, void *))VT(context)[33])(
    context, 1, &render_target, 0);
  ((void (*)(void *, void *))VT(context)[17])(context, input_layout);
  u32 stride = sizeof(CubeVertex), offset = 0;
  ((void (*)(void *, u32, u32, void **, const u32 *, const u32 *))VT(context)[18])(
    context, 0, 1, &vertex_buffer, &stride, &offset);
  ((void (*)(void *, u32))VT(context)[24])(context, 4);
  ((void (*)(void *, void *, void *, u32))VT(context)[11])(context, vertex_shader, 0, 0);
  ((void (*)(void *, void *, void *, u32))VT(context)[9])(context, pixel_shader, 0, 0);
  ((void (*)(void *, u32, u32, void **))VT(context)[8])(context, 0, 1, &texture_view);
  ((void (*)(void *, u32, u32, void **))VT(context)[10])(context, 0, 1, &sampler);
  ((void (*)(void *, u32, u32, void **))VT(context)[7])(context, 0, 1, &constant_buffer);

  printf("textured cube: six faces, %u vertices, %ux%u four-colour checker, frames requested=%u\n",
    VERTEX_COUNT, TEXTURE_SIZE, TEXTURE_SIZE, FRAME_COUNT);
  printf("camera: perspective projection with continuously moving orbit position\n");
  for (u32 frame = 0; frame < FRAME_COUNT; frame++) {
    TransformConstants transform;
    build_transform(&transform, frame);
    ((void (*)(void *, void *, u32, const void *, const void *, u32, u32))VT(context)[48])(
      context, constant_buffer, 0, 0, &transform, 0, 0);
    const float clear_color[4] = { 0.025f, 0.035f, 0.06f, 1.0f };
    ((void (*)(void *, void *, const float *))VT(context)[50])(
      context, render_target, clear_color);
    ((void (*)(void *, u32, u32))VT(context)[13])(context, VERTEX_COUNT, 0);
    unsigned char message[64];
    while (PeekMessageW(message, 0, 0, 0, 1)) {}
    hr = ((HR (*)(void *, u32, u32))VT(swapchain)[8])(swapchain, 1, 0);
    if (hr) {
      rendering_errors++;
      fail("D3D11 draw/Vulkan present", hr);
    }
    frames_presented++;
    if (frame == 0)
      printf("Vulkan graphics pipeline: PASS (DXBC shaders accepted for draw)\n");
  }
  printf("textured cube: PASS frames presented=%u rendering errors=%u\n",
    frames_presented, rendering_errors);
  cleanup();
  exit(0);
  return 0;
}
