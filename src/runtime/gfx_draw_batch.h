#ifndef AOT_GFX_DRAW_BATCH_H
#define AOT_GFX_DRAW_BATCH_H

#include "gfx_backend.h"

#define AOT_GFX_MAX_VERTEX_STREAMS 4
#define AOT_GFX_MAX_CONSTANT_BUFFERS 4
#define AOT_GFX_MAX_TEXTURES 4
#define AOT_GFX_MAX_DRAW_ATTRIBUTES 16
#define AOT_GFX_MAX_DRAWS_PER_FRAME 4096

typedef struct {
  const void *data;
  size_t data_size;
  uint32_t stride;
  uint32_t offset;
} AOT_GFX_VERTEX_STREAM;

typedef struct {
  const void *data;
  size_t data_size;
} AOT_GFX_CONSTANT_DATA;

typedef struct {
  float x, y, width, height, min_depth, max_depth;
} AOT_GFX_VIEWPORT;

typedef struct {
  int32_t left, top, right, bottom;
} AOT_GFX_SCISSOR;

typedef struct {
  const uint32_t *vertex_spirv;
  size_t vertex_spirv_size;
  const uint32_t *pixel_spirv;
  size_t pixel_spirv_size;
  AOT_GFX_VERTEX_STREAM vertex_streams[AOT_GFX_MAX_VERTEX_STREAMS];
  const AOT_PRESENT_VERTEX_ATTRIBUTE *attributes;
  uint32_t attribute_count;
  uint32_t vertex_count;
  uint32_t first_vertex;
  const void *indices;
  size_t index_data_size;
  uint32_t index_format;
  int32_t base_vertex;
  uint8_t indexed;
  uint8_t alpha_blend;
  uint8_t has_viewport;
  uint8_t has_scissor;
  uint8_t cull_mode;
  uint8_t front_counter_clockwise;
  AOT_GFX_VIEWPORT viewport;
  AOT_GFX_SCISSOR scissor;
  const AOT_PRESENT_TEXTURE *textures[AOT_GFX_MAX_TEXTURES];
  const AOT_PRESENT_SAMPLER *samplers[AOT_GFX_MAX_TEXTURES];
  AOT_GFX_CONSTANT_DATA constants[AOT_GFX_MAX_CONSTANT_BUFFERS];
} AOT_GFX_DRAW_CALL;

typedef struct AOT_GFX_DRAW_BATCH AOT_GFX_DRAW_BATCH;
typedef void (*AOT_GFX_RESOURCE_REF)(void *resource);

AOT_GFX_DRAW_BATCH *aot_gfx_draw_batch_create(void);
void aot_gfx_draw_batch_clear(AOT_GFX_DRAW_BATCH *batch);
void aot_gfx_draw_batch_destroy(AOT_GFX_DRAW_BATCH *batch);
int aot_gfx_draw_batch_append(AOT_GFX_DRAW_BATCH *batch,
                              const AOT_GFX_DRAW_CALL *draw,
                              void *const *resources,
                              size_t resource_count,
                              AOT_GFX_RESOURCE_REF retain_resource,
                              AOT_GFX_RESOURCE_REF release_resource);
const AOT_GFX_DRAW_CALL *aot_gfx_draw_batch_get(const AOT_GFX_DRAW_BATCH *batch,
                                                size_t index);
size_t aot_gfx_draw_batch_count(const AOT_GFX_DRAW_BATCH *batch);

#endif
