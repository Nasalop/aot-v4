#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t location;
  uint32_t format;
  uint32_t binding;
  uint32_t offset;
} AOT_PRESENT_VERTEX_ATTRIBUTE;

typedef struct AOT_PRESENT_TEXTURE AOT_PRESENT_TEXTURE;
typedef struct AOT_PRESENT_SAMPLER AOT_PRESENT_SAMPLER;
typedef struct AOT_GFX_DRAW_BATCH AOT_GFX_DRAW_BATCH;

typedef struct {
  uint32_t draw_calls;
  uint32_t descriptor_sets;
  size_t vertex_bytes;
  size_t index_bytes;
  size_t constant_bytes;
} AOT_PRESENT_FRAME_STATS;

enum {
  AOT_PRESENT_FORMAT_R32_FLOAT = 1,
  AOT_PRESENT_FORMAT_R32G32_FLOAT,
  AOT_PRESENT_FORMAT_R32G32B32_FLOAT,
  AOT_PRESENT_FORMAT_R32G32B32A32_FLOAT,
  AOT_PRESENT_FORMAT_R32_UINT,
  AOT_PRESENT_FORMAT_R32G32_UINT,
  AOT_PRESENT_FORMAT_R8G8B8A8_UNORM
};

/* Android uses a Vulkan swapchain; the host backend logs clears and rejects shader draws. */
int  aot_present_init(void);                 /* 0 ok */
void aot_present_clear(const float rgba[4]); /* present one frame filled with a solid colour */
int aot_present_texture_create(uint32_t width, uint32_t height, const uint8_t *rgba,
                               AOT_PRESENT_TEXTURE **out);
void aot_present_texture_destroy(AOT_PRESENT_TEXTURE *texture);
void aot_present_graphics_cleanup(void);
int aot_present_sampler_create(uint32_t filter, uint32_t address_u, uint32_t address_v,
                               uint32_t address_w, AOT_PRESENT_SAMPLER **out);
void aot_present_sampler_destroy(AOT_PRESENT_SAMPLER *sampler);
int aot_present_draw(const float rgba[4], const uint32_t *vertex_spirv, size_t vertex_spirv_size,
                     const uint32_t *pixel_spirv, size_t pixel_spirv_size, const void *vertices,
                     uint32_t vertex_stride, const AOT_PRESENT_VERTEX_ATTRIBUTE *attributes,
                     uint32_t attribute_count, uint32_t vertex_count, uint32_t first_vertex,
                     const AOT_PRESENT_TEXTURE *texture, const AOT_PRESENT_SAMPLER *sampler,
                     const void *vertex_constants, size_t vertex_constants_size);
int aot_present_draw_batch(const float rgba[4], const AOT_GFX_DRAW_BATCH *batch,
                           AOT_PRESENT_FRAME_STATS *stats);
void aot_present_size(uint32_t *w, uint32_t *h);
#ifdef AOT_ANDROID_APP
#include <android/native_window.h>
void aot_android_set_window(ANativeWindow *w);   /* call from android_main on INIT_WINDOW (NULL on TERM_WINDOW) */
#endif
