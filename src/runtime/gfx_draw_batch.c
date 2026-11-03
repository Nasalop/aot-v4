#include "gfx_draw_batch.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  AOT_GFX_DRAW_CALL draw;
  AOT_PRESENT_VERTEX_ATTRIBUTE attributes[AOT_GFX_MAX_DRAW_ATTRIBUTES];
  void *resources[AOT_GFX_MAX_VERTEX_STREAMS + AOT_GFX_MAX_CONSTANT_BUFFERS +
                  AOT_GFX_MAX_TEXTURES * 2 + 3];
  size_t resource_count;
  AOT_GFX_RESOURCE_REF release_resource;
} OwnedDraw;

struct AOT_GFX_DRAW_BATCH {
  OwnedDraw *draws;
  size_t count;
  size_t capacity;
};

static void release_draw(OwnedDraw *owned) {
  for (uint32_t slot = 0; slot < AOT_GFX_MAX_VERTEX_STREAMS; slot++)
    free((void *)owned->draw.vertex_streams[slot].data);
  for (uint32_t slot = 0; slot < AOT_GFX_MAX_CONSTANT_BUFFERS; slot++)
    free((void *)owned->draw.constants[slot].data);
  free((void *)owned->draw.indices);
  for (size_t i = 0; i < owned->resource_count; i++)
    owned->release_resource(owned->resources[i]);
  memset(owned, 0, sizeof *owned);
}

AOT_GFX_DRAW_BATCH *aot_gfx_draw_batch_create(void) {
  return calloc(1, sizeof(AOT_GFX_DRAW_BATCH));
}

void aot_gfx_draw_batch_clear(AOT_GFX_DRAW_BATCH *batch) {
  if (!batch) return;
  for (size_t i = 0; i < batch->count; i++)
    release_draw(&batch->draws[i]);
  batch->count = 0;
}

void aot_gfx_draw_batch_destroy(AOT_GFX_DRAW_BATCH *batch) {
  if (!batch) return;
  aot_gfx_draw_batch_clear(batch);
  free(batch->draws);
  free(batch);
}

int aot_gfx_draw_batch_append(AOT_GFX_DRAW_BATCH *batch,
                              const AOT_GFX_DRAW_CALL *draw,
                              void *const *resources,
                              size_t resource_count,
                              AOT_GFX_RESOURCE_REF retain_resource,
                              AOT_GFX_RESOURCE_REF release_resource) {
  if (!batch || !draw || !draw->vertex_spirv || !draw->pixel_spirv ||
      !draw->vertex_count || draw->attribute_count > AOT_GFX_MAX_DRAW_ATTRIBUTES ||
      (draw->attribute_count && !draw->attributes) ||
      (draw->indexed && (!draw->indices || !draw->index_data_size ||
          (draw->index_format != 57 && draw->index_format != 42))) ||
      (!draw->indexed && (draw->indices || draw->index_data_size)) ||
      resource_count > sizeof(((OwnedDraw *)0)->resources) /
                           sizeof(((OwnedDraw *)0)->resources[0]) ||
      (resource_count && (!resources || !retain_resource || !release_resource)) ||
      batch->count >= AOT_GFX_MAX_DRAWS_PER_FRAME)
    return -1;

  if (batch->count == batch->capacity) {
    size_t capacity = batch->capacity ? batch->capacity * 2 : 16;
    if (capacity > AOT_GFX_MAX_DRAWS_PER_FRAME)
      capacity = AOT_GFX_MAX_DRAWS_PER_FRAME;
    OwnedDraw *draws = realloc(batch->draws, capacity * sizeof *draws);
    if (!draws) return -1;
    batch->draws = draws;
    batch->capacity = capacity;
    for (size_t i = 0; i < batch->count; i++)
      batch->draws[i].draw.attributes = batch->draws[i].attributes;
  }

  OwnedDraw owned;
  memset(&owned, 0, sizeof owned);
  owned.draw = *draw;
  owned.draw.attributes = owned.attributes;
  owned.release_resource = release_resource;
  for (uint32_t slot = 0; slot < AOT_GFX_MAX_VERTEX_STREAMS; slot++)
    owned.draw.vertex_streams[slot].data = NULL;
  for (uint32_t slot = 0; slot < AOT_GFX_MAX_CONSTANT_BUFFERS; slot++)
    owned.draw.constants[slot].data = NULL;
  owned.draw.indices = NULL;
  if (draw->attribute_count)
    memcpy(owned.attributes, draw->attributes,
           draw->attribute_count * sizeof *draw->attributes);

  for (uint32_t slot = 0; slot < AOT_GFX_MAX_VERTEX_STREAMS; slot++) {
    const AOT_GFX_VERTEX_STREAM *source = &draw->vertex_streams[slot];
    if (!source->data_size) {
      owned.draw.vertex_streams[slot].data = NULL;
      continue;
    }
    if (!source->data) goto fail;
    void *copy = malloc(source->data_size);
    if (!copy) goto fail;
    memcpy(copy, source->data, source->data_size);
    owned.draw.vertex_streams[slot].data = copy;
  }
  for (uint32_t slot = 0; slot < AOT_GFX_MAX_CONSTANT_BUFFERS; slot++) {
    const AOT_GFX_CONSTANT_DATA *source = &draw->constants[slot];
    if (!source->data_size) {
      owned.draw.constants[slot].data = NULL;
      continue;
    }
    if (!source->data) goto fail;
    void *copy = malloc(source->data_size);
    if (!copy) goto fail;
    memcpy(copy, source->data, source->data_size);
    owned.draw.constants[slot].data = copy;
  }
  if (draw->index_data_size) {
    void *copy = malloc(draw->index_data_size);
    if (!copy) goto fail;
    memcpy(copy, draw->indices, draw->index_data_size);
    owned.draw.indices = copy;
  }
  for (size_t i = 0; i < resource_count; i++) {
    if (!resources[i]) continue;
    retain_resource(resources[i]);
    owned.resources[owned.resource_count++] = resources[i];
  }
  batch->draws[batch->count] = owned;
  batch->draws[batch->count].draw.attributes =
      batch->draws[batch->count].attributes;
  batch->count++;
  return 0;

fail:
  for (uint32_t slot = 0; slot < AOT_GFX_MAX_VERTEX_STREAMS; slot++)
    free((void *)owned.draw.vertex_streams[slot].data);
  for (uint32_t slot = 0; slot < AOT_GFX_MAX_CONSTANT_BUFFERS; slot++)
    free((void *)owned.draw.constants[slot].data);
  free((void *)owned.draw.indices);
  return -1;
}

const AOT_GFX_DRAW_CALL *aot_gfx_draw_batch_get(const AOT_GFX_DRAW_BATCH *batch,
                                                size_t index) {
  return batch && index < batch->count ? &batch->draws[index].draw : NULL;
}

size_t aot_gfx_draw_batch_count(const AOT_GFX_DRAW_BATCH *batch) {
  return batch ? batch->count : 0;
}
