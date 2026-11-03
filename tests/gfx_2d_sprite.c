#include <string.h>

typedef unsigned long long u64;
typedef unsigned u32;
typedef long HR;
typedef struct { float x, y, z, u, v, r, g, b, a; } SpriteVertex;
typedef struct { u32 byte_width, usage, bind_flags, cpu_access, misc_flags, stride; } BufferDesc;
typedef struct { const void *data; u32 pitch, slice_pitch; } InitialData;
typedef struct { void *data; u32 row_pitch, depth_pitch; } MappedSubresource;
typedef struct { float x, y, width, height, min_depth, max_depth; } Viewport;
typedef struct { long left, top, right, bottom; } ScissorRect;
typedef struct {
  const unsigned short *semantic;
  u32 semantic_index, format, slot, offset, slot_class, step_rate;
} InputElement;
typedef struct {
  u32 enabled, src_color, dst_color, color_op;
  u32 src_alpha, dst_alpha, alpha_op;
  unsigned char write_mask, padding[3];
} RenderTargetBlend;
typedef struct {
  u32 alpha_to_coverage, independent_blend;
  RenderTargetBlend targets[8];
} BlendDesc;

#include "gfx_2d_sprite_shaders.h"

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
__declspec(dllimport) int GetSystemMetrics(int);

#define VT(object) (*(void ***)(object))
#define FRAME_COUNT 600
#define VERTEX_COUNT 8
#define INDEX_COUNT 12

static const unsigned short class_name[] = { 'A', 'O', 'T', '2', 'D', 'S', 'p', 'r', 'i', 't', 'e', 0 };
static const unsigned short semantic_position[] = { 'P', 'O', 'S', 'I', 'T', 'I', 'O', 'N', 0 };
static const unsigned short semantic_texcoord[] = { 'T', 'E', 'X', 'C', 'O', 'O', 'R', 'D', 0 };
static const unsigned short semantic_color[] = { 'C', 'O', 'L', 'O', 'R', 0 };
static SpriteVertex vertices[VERTEX_COUNT] = {
  { -1,-1,0.8f, 0,1, 1,1,1,1 }, { 1,-1,0.8f, 1,1, 1,1,1,1 },
  { 1, 1,0.8f, 1,0, 1,1,1,1 }, { -1,1,0.8f, 0,0, 1,1,1,1 },
  { -0.55f,-0.55f,0.3f, 0,1, 1,1,1,1 }, { 0.55f,-0.55f,0.3f, 1,1, 1,1,1,1 },
  { 0.55f,0.55f,0.3f, 1,0, 1,1,1,1 }, { -0.55f,0.55f,0.3f, 0,0, 1,1,1,1 }
};
static const unsigned short indices[INDEX_COUNT] = {
  0,1,2, 0,2,3, 4,5,6, 4,6,7
};
static const unsigned char background_pixels[16] = {
  24, 50, 220, 255, 24, 50, 220, 255,
  24, 50, 220, 255, 24, 50, 220, 255
};
static const unsigned char foreground_pixels[16] = {
  255, 255, 255, 128, 255, 255, 255, 128,
  255, 255, 255, 128, 255, 255, 255, 128
};
static void *swapchain, *device, *context, *vertex_shader, *pixel_shader;
static void *input_layout, *vertex_buffer, *index_buffer, *constant_buffer;
static void *blend_state, *rasterizer_state, *textures[2], *texture_views[2], *sampler;
static void *backbuffer, *render_target;
static u32 frames_presented, rendering_errors, indexed_draw_calls;
int _fltused;

static int release_object(void **object, const char *name) {
  if (!*object) return 1;
  u32 remaining = ((u32 (*)(void *))VT(*object)[2])(*object);
  *object = 0;
  if (remaining) {
    printf("cleanup: FAIL: %s has %u references\n", name, remaining);
    return 0;
  }
  printf("cleanup: released %s\n", name);
  return 1;
}

static int cleanup(void) {
  int clean = 1;
  if (context) ((void (*)(void *))VT(context)[110])(context);
  clean &= release_object(&render_target, "render target");
  clean &= release_object(&swapchain, "swapchain");
  clean &= release_object(&backbuffer, "backbuffer");
  clean &= release_object(&input_layout, "input layout");
  clean &= release_object(&vertex_buffer, "vertex buffer");
  clean &= release_object(&index_buffer, "index buffer");
  clean &= release_object(&constant_buffer, "sprite constant buffer");
  clean &= release_object(&blend_state, "alpha blend state");
  clean &= release_object(&rasterizer_state, "scissor rasterizer state");
  for (u32 i = 0; i < 2; i++) {
    clean &= release_object(&texture_views[i], "texture SRV");
    clean &= release_object(&textures[i], "sprite texture");
  }
  clean &= release_object(&sampler, "sprite sampler");
  clean &= release_object(&vertex_shader, "vertex shader");
  clean &= release_object(&pixel_shader, "pixel shader");
  clean &= release_object(&device, "D3D11 device");
  clean &= release_object(&context, "D3D11 immediate context");
  printf("cleanup: %s\n", clean ? "PASS" : "FAIL");
  return clean;
}

static void fail(const char *message, HR hr) {
  printf("2D sprite test: FAIL: %s (HRESULT 0x%x), frames=%u indexed_draws=%u errors=%u\n",
      message, (u32)hr, frames_presented, indexed_draw_calls, rendering_errors + 1);
  rendering_errors++;
  cleanup();
  exit(1);
}

static HR create_buffer(u32 bytes, u32 bind, u32 usage, u32 cpu_access,
                        const void *initial, void **out) {
  BufferDesc desc = { bytes, usage, bind, cpu_access, 0, 0 };
  InitialData data = { initial, 0, 0 };
  return ((HR (*)(void *, const BufferDesc *, const InitialData *, void **))VT(device)[3])(
      device, &desc, initial ? &data : 0, out);
}

static void update_buffer(void *buffer, const void *data) {
  ((void (*)(void *, void *, u32, const void *, const void *, u32, u32))VT(context)[48])(
      context, buffer, 0, 0, data, 0, 0);
}

int mainCRTStartup(void) {
  unsigned char window_class[80] = { 0 };
  *(u32 *)window_class = sizeof window_class;
  *(const unsigned short **)(window_class + 64) = class_name;
  RegisterClassExW(window_class);
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
      device, aot_2d_sprite_vs, aot_2d_sprite_vs_size, 0, &vertex_shader);
  if (hr) fail("vertex shader translation", hr);
  hr = ((HR (*)(void *, const void *, u64, void *, void **))VT(device)[15])(
      device, aot_2d_sprite_ps, aot_2d_sprite_ps_size, 0, &pixel_shader);
  if (hr) fail("pixel shader translation", hr);

  InputElement elements[3] = {
    { semantic_position, 0, 6, 0, 0, 0, 0 },
    { semantic_texcoord, 0, 16, 0, 12, 0, 0 },
    { semantic_color, 0, 2, 0, 20, 0, 0 }
  };
  hr = ((HR (*)(void *, const InputElement *, u32, const void *, u64, void **))VT(device)[11])(
      device, elements, 3, aot_2d_sprite_vs, aot_2d_sprite_vs_size, &input_layout);
  if (hr) fail("sprite input layout", hr);
  hr = create_buffer(sizeof vertices, 1, 2, 0x10000, 0, &vertex_buffer);
  if (hr) fail("sprite vertex buffer", hr);
  hr = create_buffer(sizeof indices, 2, 2, 0x10000, 0, &index_buffer);
  if (hr) fail("sprite index buffer", hr);
  MappedSubresource mapped;
  hr = ((HR (*)(void *, void *, u32, u32, u32, MappedSubresource *))VT(context)[14])(
      context, vertex_buffer, 0, 4, 0, &mapped);
  if (hr) fail("map dynamic sprite vertex buffer", hr);
  memcpy(mapped.data, vertices, sizeof vertices);
  ((void (*)(void *, void *, u32))VT(context)[15])(context, vertex_buffer, 0);
  hr = ((HR (*)(void *, void *, u32, u32, u32, MappedSubresource *))VT(context)[14])(
      context, index_buffer, 0, 4, 0, &mapped);
  if (hr) fail("map dynamic sprite index buffer", hr);
  memcpy(mapped.data, indices, sizeof indices);
  ((void (*)(void *, void *, u32))VT(context)[15])(context, index_buffer, 0);
  float motion[4] = { 0, 0, 0, 0 };
  hr = create_buffer(sizeof motion, 4, 0, 0, motion, &constant_buffer);
  if (hr) fail("sprite constant buffer", hr);

  const unsigned char *pixels[2] = { background_pixels, foreground_pixels };
  for (u32 i = 0; i < 2; i++) {
    u32 texture_desc[11] = { 2, 2, 1, 1, 28, 1, 0, 0, 8, 0, 0 };
    InitialData initial_texture = { pixels[i], 8, 16 };
    hr = ((HR (*)(void *, const u32 *, const InitialData *, void **))VT(device)[5])(
        device, texture_desc, &initial_texture, &textures[i]);
    if (hr) fail("sprite texture creation", hr);
    u32 view_desc[6] = { 28, 3, 0, 1, 0, 0 };
    hr = ((HR (*)(void *, void *, const u32 *, void **))VT(device)[7])(
        device, textures[i], view_desc, &texture_views[i]);
    if (hr) fail("sprite texture SRV creation", hr);
  }
  u32 sampler_desc[13] = { 0, 3, 3, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0 };
  hr = ((HR (*)(void *, const u32 *, void **))VT(device)[23])(
      device, sampler_desc, &sampler);
  if (hr) fail("sprite sampler creation", hr);

  BlendDesc blend = { 0 };
  blend.targets[0].enabled = 1;
  blend.targets[0].src_color = 5;
  blend.targets[0].dst_color = 6;
  blend.targets[0].color_op = 1;
  blend.targets[0].src_alpha = 2;
  blend.targets[0].dst_alpha = 6;
  blend.targets[0].alpha_op = 1;
  blend.targets[0].write_mask = 0x0f;
  hr = ((HR (*)(void *, const BlendDesc *, void **))VT(device)[20])(
      device, &blend, &blend_state);
  if (hr) fail("standard alpha blend state creation", hr);
  u32 rasterizer_desc[10] = { 3, 1, 0, 0, 0, 0, 1, 1, 0, 0 };
  hr = ((HR (*)(void *, const u32 *, void **))VT(device)[22])(
      device, rasterizer_desc, &rasterizer_state);
  if (hr) fail("scissor rasterizer state creation", hr);

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
      context, 0, 1, &constant_buffer);
  ((void (*)(void *, u32))VT(context)[24])(context, 4);
  ((void (*)(void *, void *, void *, u32))VT(context)[11])(
      context, vertex_shader, 0, 0);
  ((void (*)(void *, void *, void *, u32))VT(context)[9])(
      context, pixel_shader, 0, 0);
  ((void (*)(void *, void *))VT(context)[17])(context, input_layout);
  void *bound_vertex_buffer = vertex_buffer;
  u32 stride = sizeof(SpriteVertex), vertex_offset = 0;
  ((void (*)(void *, u32, u32, void **, const u32 *, const u32 *))VT(context)[18])(
      context, 0, 1, &bound_vertex_buffer, &stride, &vertex_offset);
  ((void (*)(void *, void *, u32, u32))VT(context)[19])(
      context, index_buffer, 57, 0);
  ((void (*)(void *, u32, u32, void **))VT(context)[10])(
      context, 0, 1, &sampler);
  ((void (*)(void *, void *))VT(context)[43])(context, rasterizer_state);
  int width = GetSystemMetrics(0), height = GetSystemMetrics(1);
  Viewport viewport = { 0, 0, (float)width, (float)height, 0, 1 };
  ScissorRect scissor = { 0, 0, width, height };
  ((void (*)(void *, u32, const Viewport *))VT(context)[44])(
      context, 1, &viewport);
  ((void (*)(void *, u32, const ScissorRect *))VT(context)[45])(
      context, 1, &scissor);

  printf("2D sprite test: indexed draws/frame=2 frames=%u dynamic_vertex_index=MAP_WRITE viewport_scissor=PASS alpha_blend=SRC_ALPHA/INV_SRC_ALPHA\n",
      FRAME_COUNT);
  for (u32 frame = 0; frame < FRAME_COUNT; frame++) {
    motion[0] = ((float)(frame % 60) - 30.0f) * 0.0005f;
    update_buffer(constant_buffer, motion);
    float shift = motion[0];
    vertices[4].x = -0.55f + shift; vertices[5].x = 0.55f + shift;
    vertices[6].x = 0.55f + shift; vertices[7].x = -0.55f + shift;
    hr = ((HR (*)(void *, void *, u32, u32, u32, MappedSubresource *))VT(context)[14])(
        context, vertex_buffer, 0, 4, 0, &mapped);
    if (hr) fail("map dynamic sprite vertices", hr);
    memcpy(mapped.data, vertices, sizeof vertices);
    ((void (*)(void *, void *, u32))VT(context)[15])(context, vertex_buffer, 0);
    for (u32 sprite = 0; sprite < 2; sprite++) {
      ((void (*)(void *, void *, const float *, u32))VT(context)[35])(
          context, sprite ? blend_state : 0, 0, 0xffffffffu);
      ((void (*)(void *, u32, u32, void **))VT(context)[8])(
          context, 0, 1, &texture_views[sprite]);
      ((void (*)(void *, u32, u32, int))VT(context)[12])(
          context, 6, sprite ? 6 : 0, 0);
      indexed_draw_calls++;
    }
    float clear_color[4] = { 0.04f, 0.04f, 0.06f, 1.0f };
    ((void (*)(void *, void *, const float *))VT(context)[50])(
        context, render_target, clear_color);
    unsigned char message[64];
    while (PeekMessageW(message, 0, 0, 0, 1)) {}
    hr = ((HR (*)(void *, u32, u32))VT(swapchain)[8])(swapchain, 1, 0);
    if (hr) fail("indexed alpha-blended sprite frame", hr);
    frames_presented++;
  }
  printf("2D sprite test: PASS shader_translation=PASS frames_presented=%u indexed_draw_calls=%u rendering_errors=%u\n",
      frames_presented, indexed_draw_calls, rendering_errors);
  exit(cleanup() ? 0 : 1);
  return 0;
}
