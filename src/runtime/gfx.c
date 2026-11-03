#include "gfx.h"
#include <stdlib.h>
#include <string.h>

struct AOT_BUFFER { uint8_t *data; size_t size; uint32_t bind; int mapped; };
struct AOT_TEXTURE2D { uint8_t *data; size_t size; AOT_TEXTURE2D_DESC desc; };
struct AOT_VIEW { void *resource; uint32_t kind; };
struct AOT_SHADER { uint8_t *data; size_t size; };
struct AOT_CBV { AOT_BUFFER *buffer; uint64_t offset, range; };
struct AOT_SAMPLER { AOT_SAMPLER_DESC desc; uint32_t min_filter, mag_filter, mip_mode, address[3]; };
struct AOT_INPUT_LAYOUT { uint32_t count, stride[16], slot[32], offset[32], format[32], step[32]; AOT_INPUT_CLASS klass[32]; };
struct AOT_CONTEXT { AOT_VK *vk; AOT_VIEW *target, *rtv[8], *dsv; AOT_INPUT_LAYOUT *layout; AOT_CBV *cbv[16]; AOT_VIEW *srv[16]; AOT_SAMPLER *sampler[16]; AOT_TOPOLOGY topology; uint64_t draws; };

static uint32_t sampler_filter(uint32_t f, int mag) { (void)mag; return f == 1 || f == 2 ? 1 : 0; }
static uint32_t sampler_address(uint32_t a) { switch (a) { case 1: return 2; case 2: return 3; case 3: return 4; default: return 0; } }

static uint32_t input_format_vk(AOT_INPUT_FORMAT f) {
#ifdef __ANDROID__
  switch (f) { case AOT_INPUT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT; case AOT_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT; case AOT_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT; case AOT_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT; case AOT_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT; case AOT_FORMAT_R32G32_UINT: return VK_FORMAT_R32G32_UINT; case AOT_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM; default: return 0; }
#else
  switch (f) { case AOT_INPUT_R32_FLOAT: return 100; case AOT_FORMAT_R32G32_FLOAT: return 103; case AOT_FORMAT_R32G32B32_FLOAT: return 106; case AOT_FORMAT_R32G32B32A32_FLOAT: return 109; case AOT_FORMAT_R32_UINT: return 98; case AOT_FORMAT_R32G32_UINT: return 101; case AOT_FORMAT_R8G8B8A8_UNORM: return 37; default: return 0; }
#endif
}
static uint32_t input_format_size(AOT_INPUT_FORMAT f) { switch (f) { case AOT_FORMAT_R32G32_FLOAT: case AOT_FORMAT_R32G32_UINT: return 8; case AOT_FORMAT_R32G32B32_FLOAT: return 12; case AOT_FORMAT_R32G32B32A32_FLOAT: case AOT_FORMAT_R8G8B8A8_UNORM: return 16; case AOT_FORMAT_R32_UINT: case AOT_INPUT_R32_FLOAT: return 4; default: return 0; } }
static uint32_t topology_vk(AOT_TOPOLOGY t) {
#ifdef __ANDROID__
  switch (t) { case AOT_TOPOLOGY_POINTLIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case AOT_TOPOLOGY_LINELIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST; case AOT_TOPOLOGY_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; case AOT_TOPOLOGY_TRIANGLELIST: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; case AOT_TOPOLOGY_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; case AOT_TOPOLOGY_LINELIST_ADJ: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; case AOT_TOPOLOGY_LINESTRIP_ADJ: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY; case AOT_TOPOLOGY_TRIANGLELIST_ADJ: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; case AOT_TOPOLOGY_TRIANGLESTRIP_ADJ: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; default: return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM; }
#else
  switch (t) { case AOT_TOPOLOGY_POINTLIST: return 0; case AOT_TOPOLOGY_LINELIST: return 1; case AOT_TOPOLOGY_LINESTRIP: return 2; case AOT_TOPOLOGY_TRIANGLELIST: return 3; case AOT_TOPOLOGY_TRIANGLESTRIP: return 4; case AOT_TOPOLOGY_LINELIST_ADJ: return 6; case AOT_TOPOLOGY_LINESTRIP_ADJ: return 7; case AOT_TOPOLOGY_TRIANGLELIST_ADJ: return 8; case AOT_TOPOLOGY_TRIANGLESTRIP_ADJ: return 9; default: return 0x7fffffffU; }
#endif
}

#ifdef __ANDROID__
struct AOT_VK {
  VkInstance instance; VkPhysicalDevice physical; VkDevice device;
  VkQueue queue; VkCommandPool pool; VkCommandBuffer cmd; VkSurfaceKHR surface;
  uint32_t family; int recording;
};
static void vk_cleanup(AOT_VK *v) {
  if (!v) return;
  if (v->device) { if (v->cmd) vkFreeCommandBuffers(v->device, v->pool, 1, &v->cmd); if (v->pool) vkDestroyCommandPool(v->device, v->pool, 0); vkDeviceWaitIdle(v->device); vkDestroyDevice(v->device, 0); }
  if (v->surface) vkDestroySurfaceKHR(v->instance, v->surface, 0);
  if (v->instance) vkDestroyInstance(v->instance, 0);
  free(v);
}
AOT_HRESULT aot_vk_create(ANativeWindow *window, uint32_t width, uint32_t height, AOT_VK **out) {
  (void)width; (void)height; if (!out) return AOT_E_INVALIDARG; *out = 0;
  AOT_VK *v = (AOT_VK *)calloc(1, sizeof *v); if (!v) return AOT_E_NOMEM;
  VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "x86-aot", 1, "x86-aot", 1, VK_API_VERSION_1_0 };
  const char *exts[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
  VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai, 0, 0, window ? 2u : 0u, window ? exts : 0 };
  if (vkCreateInstance(&ci, 0, &v->instance) != VK_SUCCESS) { free(v); return AOT_E_UNSUPPORTED; }
  if (window) { VkAndroidSurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, 0, 0, window }; if (vkCreateAndroidSurfaceKHR(v->instance, &si, 0, &v->surface) != VK_SUCCESS) { vk_cleanup(v); return AOT_E_UNSUPPORTED; } }
  uint32_t n = 0; vkEnumeratePhysicalDevices(v->instance, &n, 0); if (!n) { vk_cleanup(v); return AOT_E_UNSUPPORTED; }
  VkPhysicalDevice *devs = (VkPhysicalDevice *)malloc(n * sizeof *devs); if (!devs) { vk_cleanup(v); return AOT_E_NOMEM; }
  vkEnumeratePhysicalDevices(v->instance, &n, devs); v->physical = devs[0]; free(devs);
  uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(v->physical, &qn, 0); VkQueueFamilyProperties *qp = (VkQueueFamilyProperties *)malloc(qn * sizeof *qp); if (!qp) { vk_cleanup(v); return AOT_E_NOMEM; }
  vkGetPhysicalDeviceQueueFamilyProperties(v->physical, &qn, qp); int found = -1;
  for (uint32_t i = 0; i < qn; i++) { VkBool32 present = VK_TRUE; if (v->surface) vkGetPhysicalDeviceSurfaceSupportKHR(v->physical, i, v->surface, &present); if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { found = (int)i; break; } }
  free(qp); if (found < 0) { vk_cleanup(v); return AOT_E_UNSUPPORTED; } v->family = (uint32_t)found;
  float priority = 1.0f; VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, v->family, 1, &priority }; VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qi, 0, 0, 0, 0, 0 };
  if (vkCreateDevice(v->physical, &di, 0, &v->device) != VK_SUCCESS) { vk_cleanup(v); return AOT_E_UNSUPPORTED; } vkGetDeviceQueue(v->device, v->family, 0, &v->queue);
  VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, v->family }; if (vkCreateCommandPool(v->device, &pi, 0, &v->pool) != VK_SUCCESS) { vk_cleanup(v); return AOT_E_UNSUPPORTED; }
  VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, v->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 }; if (vkAllocateCommandBuffers(v->device, &ca, &v->cmd) != VK_SUCCESS) { vk_cleanup(v); return AOT_E_UNSUPPORTED; }
  *out = v; return AOT_OK;
}
void aot_vk_destroy(AOT_VK *v) { vk_cleanup(v); }
AOT_HRESULT aot_vk_begin_frame(AOT_VK *v) { if (!v || v->recording) return AOT_E_STATE; VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 0 }; if (vkBeginCommandBuffer(v->cmd, &bi) != VK_SUCCESS) return AOT_E_STATE; v->recording = 1; return AOT_OK; }
AOT_HRESULT aot_vk_end_frame(AOT_VK *v) { if (!v || !v->recording) return AOT_E_STATE; if (vkEndCommandBuffer(v->cmd) != VK_SUCCESS) return AOT_E_STATE; VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &v->cmd, 0, 0 }; if (vkQueueSubmit(v->queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS || vkQueueWaitIdle(v->queue) != VK_SUCCESS) return AOT_E_STATE; v->recording = 0; return AOT_OK; }
#else
struct AOT_VK { uint32_t width, height; int recording; };
AOT_HRESULT aot_vk_create(ANativeWindow *window, uint32_t width, uint32_t height, AOT_VK **out) { (void)window; if (!out || !width || !height) return AOT_E_INVALIDARG; *out = (AOT_VK *)calloc(1, sizeof **out); if (!*out) return AOT_E_NOMEM; (*out)->width = width; (*out)->height = height; return AOT_OK; }
void aot_vk_destroy(AOT_VK *v) { free(v); }
AOT_HRESULT aot_vk_begin_frame(AOT_VK *v) { if (!v || v->recording) return AOT_E_STATE; v->recording = 1; return AOT_OK; }
AOT_HRESULT aot_vk_end_frame(AOT_VK *v) { if (!v || !v->recording) return AOT_E_STATE; v->recording = 0; return AOT_OK; }
#endif

AOT_HRESULT aot_d3d11_create(AOT_VK *vk, AOT_CONTEXT **out) { if (!vk || !out) return AOT_E_INVALIDARG; *out = (AOT_CONTEXT *)calloc(1, sizeof **out); if (!*out) return AOT_E_NOMEM; (*out)->vk = vk; return AOT_OK; }
void aot_d3d11_destroy(AOT_CONTEXT *c) { free(c); }
AOT_HRESULT aot_create_buffer(AOT_CONTEXT *c, const AOT_BUFFER_DESC *d, const AOT_SUBRESOURCE_DATA *init, AOT_BUFFER **out) { if (!c || !d || !out || !d->ByteWidth) return AOT_E_INVALIDARG; AOT_BUFFER *b = (AOT_BUFFER *)calloc(1, sizeof *b); if (!b) return AOT_E_NOMEM; b->data = (uint8_t *)calloc(1, d->ByteWidth); if (!b->data) { free(b); return AOT_E_NOMEM; } b->size = d->ByteWidth; b->bind = d->BindFlags; if (init && init->pSysMem) memcpy(b->data, init->pSysMem, b->size); *out = b; return AOT_OK; }
AOT_HRESULT aot_create_texture2d(AOT_CONTEXT *c, const AOT_TEXTURE2D_DESC *d, const AOT_SUBRESOURCE_DATA *init, AOT_TEXTURE2D **out) { if (!c || !d || !out || !d->Width || !d->Height || d->Format == 0) return AOT_E_INVALIDARG; AOT_TEXTURE2D *t = (AOT_TEXTURE2D *)calloc(1, sizeof *t); if (!t) return AOT_E_NOMEM; t->desc = *d; uint32_t bpp = d->Format == AOT_FORMAT_R32_FLOAT ? 4 : 4; t->size = (size_t)d->Width * d->Height * bpp; t->data = (uint8_t *)calloc(1, t->size); if (!t->data) { free(t); return AOT_E_NOMEM; } if (init && init->pSysMem) { size_t row = d->Width * bpp; for (uint32_t y = 0; y < d->Height; y++) memcpy(t->data + y * row, (const uint8_t *)init->pSysMem + y * init->SysMemPitch, row); } *out = t; return AOT_OK; }
AOT_HRESULT aot_create_buffer_view(AOT_CONTEXT *c, AOT_BUFFER *b, const AOT_BUFFER_VIEW_DESC *d, AOT_VIEW **out) { if (!c || !b || !out || !d || (size_t)d->FirstElement + d->NumElements > b->size) return AOT_E_INVALIDARG; *out = (AOT_VIEW *)calloc(1, sizeof **out); if (!*out) return AOT_E_NOMEM; (*out)->resource = b; (*out)->kind = 1; return AOT_OK; }
AOT_HRESULT aot_create_texture_view(AOT_CONTEXT *c, AOT_TEXTURE2D *t, const AOT_TEXTURE_VIEW_DESC *d, AOT_VIEW **out) { if (!c || !t || !out || (d && d->MipSlice >= t->desc.MipLevels)) return AOT_E_INVALIDARG; *out = (AOT_VIEW *)calloc(1, sizeof **out); if (!*out) return AOT_E_NOMEM; (*out)->resource = t; (*out)->kind = 2; return AOT_OK; }
AOT_HRESULT aot_create_shader(AOT_CONTEXT *c, const void *code, size_t size, AOT_SHADER **out) { if (!c || !code || !size || !out) return AOT_E_INVALIDARG; *out = (AOT_SHADER *)calloc(1, sizeof **out); if (!*out) return AOT_E_NOMEM; (*out)->data = (uint8_t *)malloc(size); if (!(*out)->data) { free(*out); *out = 0; return AOT_E_NOMEM; } memcpy((*out)->data, code, size); (*out)->size = size; return AOT_OK; }
AOT_HRESULT aot_create_cbv(AOT_CONTEXT *c, AOT_BUFFER *b, uint32_t first, uint32_t count, AOT_CBV **out) {
  if (!c || !b || !out || !count || first > UINT32_MAX / 16 || count > UINT32_MAX / 16) return AOT_E_INVALIDARG;
  uint64_t off = (uint64_t)first * 16, range = (uint64_t)count * 16;
  if ((off & 15) || range > 65536 || off > b->size || range > b->size - off) return AOT_E_INVALIDARG;
  AOT_CBV *v = (AOT_CBV *)calloc(1, sizeof *v); if (!v) return AOT_E_NOMEM; v->buffer = b; v->offset = off; v->range = range; *out = v; return AOT_OK;
}
void aot_release_cbv(AOT_CBV *v) { free(v); }
void aot_set_constant_buffer(AOT_CONTEXT *c, uint32_t slot, AOT_CBV *v) { if (c && slot < 16) c->cbv[slot] = v; }
AOT_CBV *aot_get_constant_buffer(const AOT_CONTEXT *c, uint32_t slot) { return c && slot < 16 ? c->cbv[slot] : 0; }
AOT_HRESULT aot_update_constant_buffer(AOT_CONTEXT *c, AOT_CBV *v, const void *data, size_t n) { if (!c || !v || !data || n > v->range) return AOT_E_INVALIDARG; memcpy(v->buffer->data + v->offset, data, n); return AOT_OK; }
AOT_UNIFORM_BUFFER_INFO aot_uniform_buffer_info(const AOT_CBV *v) { AOT_UNIFORM_BUFFER_INFO i = { 0, 0, 0 }; if (v) { i.Offset = v->offset; i.Range = v->range; i.Buffer = v->buffer; } return i; }
AOT_HRESULT aot_create_srv(AOT_CONTEXT *c, AOT_TEXTURE2D *t, const AOT_SRV_DESC *d, AOT_VIEW **out) {
  if (!c || !t || !out || !d || !d->Format || d->MostDetailedMip >= t->desc.MipLevels || !d->MipLevels || d->MipLevels > t->desc.MipLevels - d->MostDetailedMip) return AOT_E_INVALIDARG;
  AOT_TEXTURE_VIEW_DESC tv = { d->MostDetailedMip }; return aot_create_texture_view(c, t, &tv, out);
}
AOT_HRESULT aot_create_rtv(AOT_CONTEXT *c, AOT_TEXTURE2D *t, const AOT_RTV_DESC *d, AOT_VIEW **out) {
  if (!c || !t || !out || !d || !d->Format || d->MipSlice >= t->desc.MipLevels) return AOT_E_INVALIDARG;
  AOT_TEXTURE_VIEW_DESC tv = { d->MipSlice }; AOT_HRESULT hr = aot_create_texture_view(c, t, &tv, out); if (hr == AOT_OK) (*out)->kind = 3; return hr;
}
AOT_HRESULT aot_create_dsv(AOT_CONTEXT *c, AOT_TEXTURE2D *t, const AOT_DSV_DESC *d, AOT_VIEW **out) {
  if (!c || !t || !out || !d || !d->Format || d->MipSlice >= t->desc.MipLevels || d->ReadOnlyDepth > 1 || d->ReadOnlyStencil > 1) return AOT_E_INVALIDARG;
  AOT_TEXTURE_VIEW_DESC tv = { d->MipSlice }; AOT_HRESULT hr = aot_create_texture_view(c, t, &tv, out); if (hr == AOT_OK) (*out)->kind = 4; return hr;
}
AOT_HRESULT aot_create_sampler(AOT_CONTEXT *c, const AOT_SAMPLER_DESC *d, AOT_SAMPLER **out) {
  if (!c || !d || !out || d->Filter > 2 || d->AddressU > 3 || d->AddressV > 3 || d->AddressW > 3 || d->MaxAnisotropy < 1) return AOT_E_INVALIDARG;
  AOT_SAMPLER *s = (AOT_SAMPLER *)calloc(1, sizeof *s); if (!s) return AOT_E_NOMEM; s->desc = *d; s->min_filter = sampler_filter(d->Filter, 0); s->mag_filter = sampler_filter(d->Filter, 1); s->mip_mode = d->Filter ? 1 : 0; s->address[0] = sampler_address(d->AddressU); s->address[1] = sampler_address(d->AddressV); s->address[2] = sampler_address(d->AddressW); *out = s; return AOT_OK;
}
void aot_release_sampler(AOT_SAMPLER *s) { free(s); }
void aot_set_shader_resources(AOT_CONTEXT *c, uint32_t slot, AOT_VIEW *v) { if (c && slot < 16) c->srv[slot] = v; }
void aot_set_samplers(AOT_CONTEXT *c, uint32_t slot, AOT_SAMPLER *s) { if (c && slot < 16) c->sampler[slot] = s; }
void aot_set_render_targets(AOT_CONTEXT *c, uint32_t n, AOT_VIEW *const *r, AOT_VIEW *d) { if (!c || n > 8) return; for (uint32_t i = 0; i < n; i++) c->rtv[i] = r && r[i] && r[i]->kind == 3 ? r[i] : 0; for (uint32_t i = n; i < 8; i++) c->rtv[i] = 0; c->dsv = d && d->kind == 4 ? d : 0; c->target = n ? c->rtv[0] : 0; }
AOT_VIEW *aot_get_shader_resource(const AOT_CONTEXT *c, uint32_t slot) { return c && slot < 16 ? c->srv[slot] : 0; }
AOT_SAMPLER *aot_get_sampler(const AOT_CONTEXT *c, uint32_t slot) { return c && slot < 16 ? c->sampler[slot] : 0; }
AOT_VIEW *aot_get_render_target(const AOT_CONTEXT *c, uint32_t slot) { return c && slot < 8 ? c->rtv[slot] : 0; }
AOT_VIEW *aot_get_depth_stencil(const AOT_CONTEXT *c) { return c ? c->dsv : 0; }
AOT_HRESULT aot_create_input_layout(AOT_CONTEXT *c, const AOT_INPUT_ELEMENT_DESC *e, uint32_t n, AOT_INPUT_LAYOUT **out) {
  if (!c || !e || !out || !n || n > 32) return AOT_E_INVALIDARG;
  AOT_INPUT_LAYOUT *l = (AOT_INPUT_LAYOUT *)calloc(1, sizeof *l); if (!l) return AOT_E_NOMEM; l->count = n;
  for (uint32_t i = 0; i < n; i++) {
    if (!e[i].SemanticName || e[i].InputSlot >= 16 || !input_format_size(e[i].Format) || !input_format_vk(e[i].Format)) { free(l); return AOT_E_INVALIDARG; }
    uint32_t off = e[i].AlignedByteOffset == UINT32_MAX ? l->stride[e[i].InputSlot] : e[i].AlignedByteOffset;
    if (off > UINT32_MAX - input_format_size(e[i].Format)) { free(l); return AOT_E_INVALIDARG; }
    l->slot[i] = e[i].InputSlot; l->offset[i] = off; l->format[i] = input_format_vk(e[i].Format); l->klass[i] = e[i].InputSlotClass; l->step[i] = e[i].InstanceDataStepRate;
    l->stride[e[i].InputSlot] = off + input_format_size(e[i].Format);
  }
  *out = l; return AOT_OK;
}
void aot_release_input_layout(AOT_INPUT_LAYOUT *l) { free(l); }
void aot_set_input_layout(AOT_CONTEXT *c, AOT_INPUT_LAYOUT *l) { if (c) c->layout = l; }
void aot_set_primitive_topology(AOT_CONTEXT *c, AOT_TOPOLOGY t) { if (c && topology_vk(t) != 0x7fffffffU) c->topology = t; }
AOT_TOPOLOGY aot_primitive_topology(const AOT_CONTEXT *c) { return c ? c->topology : AOT_TOPOLOGY_UNDEFINED; }
void aot_release_buffer(AOT_BUFFER *b) { if (b) { free(b->data); free(b); } }
void aot_release_texture2d(AOT_TEXTURE2D *t) { if (t) { free(t->data); free(t); } }
void aot_release_view(AOT_VIEW *v) { free(v); }
void aot_release_shader(AOT_SHADER *s) { if (s) { free(s->data); free(s); } }
AOT_HRESULT aot_map(AOT_CONTEXT *c, AOT_BUFFER *b, AOT_MAPPED_SUBRESOURCE *m) { if (!c || !b || !m || b->mapped) return AOT_E_STATE; b->mapped = 1; m->ByteWidth = (uint32_t)b->size; m->RowPitch = (uint32_t)b->size; return AOT_OK; }
void aot_unmap(AOT_CONTEXT *c, AOT_BUFFER *b) { (void)c; if (b) b->mapped = 0; }
AOT_HRESULT aot_update_buffer(AOT_CONTEXT *c, AOT_BUFFER *b, const void *data, size_t n) { if (!c || !b || !data || n > b->size) return AOT_E_INVALIDARG; memcpy(b->data, data, n); return AOT_OK; }
AOT_HRESULT aot_clear(AOT_CONTEXT *c, AOT_VIEW *v, float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; if (!c || !v || (v->kind != 3 && v->kind != 4)) return AOT_E_INVALIDARG; return AOT_OK; }
AOT_HRESULT aot_draw(AOT_CONTEXT *c, uint32_t vertices, uint32_t first) { (void)first; if (!c || !c->vk || !vertices || !c->layout || c->topology == AOT_TOPOLOGY_UNDEFINED) return AOT_E_INVALIDARG; c->draws++; return AOT_OK; }
uint64_t aot_draw_count(const AOT_CONTEXT *c) { return c ? c->draws : 0; }

/* ---- Win32/D3D11 COM ABI bridge --------------------------------------- */
typedef struct AOT_COM_CONTEXT AOT_COM_CONTEXT;
typedef struct { void **vt; uint32_t refs; AOT_CONTEXT *ctx; AOT_COM_CONTEXT *comctx; } AOT_COM_DEVICE;
struct AOT_COM_CONTEXT { void **vt; uint32_t refs; AOT_CONTEXT *ctx; };
static uint64_t com_stub(CPU *c) { (void)c; return 0; }
static uint64_t com_qi(CPU *c) { uint64_t p = c->r[8]; if (p) ST(uint64_t, p, c->r[1]); return 0; }
static uint64_t com_addref(CPU *c) { AOT_COM_DEVICE *d = (AOT_COM_DEVICE *)(uintptr_t)c->r[1]; return ++d->refs; }
static uint64_t com_release(CPU *c) { AOT_COM_DEVICE *d = (AOT_COM_DEVICE *)(uintptr_t)c->r[1]; return d->refs ? --d->refs : 0; }
static uint64_t ctx_addref(CPU *c) { AOT_COM_CONTEXT *d = (AOT_COM_CONTEXT *)(uintptr_t)c->r[1]; return ++d->refs; }
static uint64_t ctx_release(CPU *c) { AOT_COM_CONTEXT *d = (AOT_COM_CONTEXT *)(uintptr_t)c->r[1]; return d->refs ? --d->refs : 0; }
static uint64_t device_get_context(CPU *c) { AOT_COM_DEVICE *d = (AOT_COM_DEVICE *)(uintptr_t)c->r[1]; if (c->r[2]) ST(uint64_t, c->r[2], (uint64_t)(uintptr_t)d->comctx); return 0; }
static uint64_t device_create_buffer(CPU *c) { AOT_COM_DEVICE *d = (AOT_COM_DEVICE *)(uintptr_t)c->r[1]; AOT_BUFFER *b = 0; uint64_t out = c->r[9]; if (aot_create_buffer(d->ctx, (const AOT_BUFFER_DESC *)(uintptr_t)c->r[2], (const AOT_SUBRESOURCE_DATA *)(uintptr_t)c->r[8], &b) == AOT_OK && out) ST(uint64_t, out, b); return b ? 0 : (uint64_t)-1; }
static uint64_t device_create_texture(CPU *c) { AOT_COM_DEVICE *d = (AOT_COM_DEVICE *)(uintptr_t)c->r[1]; AOT_TEXTURE2D *t = 0; if (aot_create_texture2d(d->ctx, (const AOT_TEXTURE2D_DESC *)(uintptr_t)c->r[2], (const AOT_SUBRESOURCE_DATA *)(uintptr_t)c->r[8], &t) != AOT_OK) return (uint64_t)-1; if (c->r[9]) ST(uint64_t, c->r[9], t); return 0; }
static uint64_t device_create_srv(CPU *c) { AOT_COM_DEVICE *d = (AOT_COM_DEVICE *)(uintptr_t)c->r[1]; AOT_VIEW *v = 0; if (aot_create_srv(d->ctx, (AOT_TEXTURE2D *)(uintptr_t)c->r[2], (const AOT_SRV_DESC *)(uintptr_t)c->r[8], &v) != AOT_OK) return (uint64_t)-1; if (c->r[9]) ST(uint64_t, c->r[9], v); return 0; }
static uint64_t device_create_rtv(CPU *c) { AOT_COM_DEVICE *d = (AOT_COM_DEVICE *)(uintptr_t)c->r[1]; AOT_VIEW *v = 0; if (aot_create_rtv(d->ctx, (AOT_TEXTURE2D *)(uintptr_t)c->r[2], (const AOT_RTV_DESC *)(uintptr_t)c->r[8], &v) != AOT_OK) return (uint64_t)-1; if (c->r[9]) ST(uint64_t, c->r[9], v); return 0; }
static uint64_t device_create_dsv(CPU *c) { AOT_COM_DEVICE *d = (AOT_COM_DEVICE *)(uintptr_t)c->r[1]; AOT_VIEW *v = 0; if (aot_create_dsv(d->ctx, (AOT_TEXTURE2D *)(uintptr_t)c->r[2], (const AOT_DSV_DESC *)(uintptr_t)c->r[8], &v) != AOT_OK) return (uint64_t)-1; if (c->r[9]) ST(uint64_t, c->r[9], v); return 0; }
static uint64_t device_create_sampler(CPU *c) { AOT_COM_DEVICE *d = (AOT_COM_DEVICE *)(uintptr_t)c->r[1]; AOT_SAMPLER *s = 0; if (aot_create_sampler(d->ctx, (const AOT_SAMPLER_DESC *)(uintptr_t)c->r[2], &s) != AOT_OK) return (uint64_t)-1; if (c->r[8]) ST(uint64_t, c->r[8], s); return 0; }
static uint64_t ctx_set_layout(CPU *c) { AOT_COM_CONTEXT *x = (AOT_COM_CONTEXT *)(uintptr_t)c->r[1]; aot_set_input_layout(x->ctx, (AOT_INPUT_LAYOUT *)(uintptr_t)c->r[2]); return 0; }
static uint64_t ctx_set_topology(CPU *c) { AOT_COM_CONTEXT *x = (AOT_COM_CONTEXT *)(uintptr_t)c->r[1]; aot_set_primitive_topology(x->ctx, (AOT_TOPOLOGY)c->r[2]); return 0; }
static uint64_t ctx_draw(CPU *c) { AOT_COM_CONTEXT *x = (AOT_COM_CONTEXT *)(uintptr_t)c->r[1]; return aot_draw(x->ctx, (uint32_t)c->r[2], (uint32_t)c->r[8]) == AOT_OK ? 0 : (uint64_t)-1; }
static uint64_t ctx_set_cb(CPU *c) { (void)c; return 0; }
static uint64_t ctx_set_srv(CPU *c) { AOT_COM_CONTEXT *x = (AOT_COM_CONTEXT *)(uintptr_t)c->r[1]; if (c->r[9] && c->r[2] < 16) aot_set_shader_resources(x->ctx, (uint32_t)c->r[2], (AOT_VIEW *)(uintptr_t)LD(uint64_t, c->r[9])); return 0; }
static uint64_t ctx_set_sampler(CPU *c) { AOT_COM_CONTEXT *x = (AOT_COM_CONTEXT *)(uintptr_t)c->r[1]; if (c->r[9] && c->r[2] < 16) aot_set_samplers(x->ctx, (uint32_t)c->r[2], (AOT_SAMPLER *)(uintptr_t)LD(uint64_t, c->r[9])); return 0; }
static uint64_t ctx_set_targets(CPU *c) { AOT_COM_CONTEXT *x = (AOT_COM_CONTEXT *)(uintptr_t)c->r[1]; uint32_t n = (uint32_t)c->r[2]; AOT_VIEW *r[8] = {0}; for (uint32_t i = 0; i < n && i < 8; i++) r[i] = (AOT_VIEW *)(uintptr_t)LD(uint64_t, c->r[8] + 8 * i); aot_set_render_targets(x->ctx, n, r, (AOT_VIEW *)(uintptr_t)c->r[9]); return 0; }
static uint64_t ctx_clear_rtv(CPU *c) { AOT_COM_CONTEXT *x = (AOT_COM_CONTEXT *)(uintptr_t)c->r[1]; return aot_clear(x->ctx, (AOT_VIEW *)(uintptr_t)c->r[2], 0, 0, 0, 1); }
static uint64_t ctx_clear_dsv(CPU *c) { AOT_COM_CONTEXT *x = (AOT_COM_CONTEXT *)(uintptr_t)c->r[1]; return aot_clear(x->ctx, (AOT_VIEW *)(uintptr_t)c->r[2], 0, 0, 0, 1); }
static uint64_t make_thunk(aot_host_fn fn, const char *name) { return aot_register_thunk(fn, name); }
static void **device_vtable(void) {
  static void *v[43]; static int done;
  if (!done) { for (int i = 0; i < 43; i++) { char n[64]; snprintf(n, sizeof n, "ID3D11Device::vtbl[%d] (not implemented)", i); v[i] = (void *)(uintptr_t)make_thunk(NULL, n); } v[0] = (void *)(uintptr_t)make_thunk(com_qi, "ID3D11Device::QueryInterface"); v[1] = (void *)(uintptr_t)make_thunk(com_addref, "ID3D11Device::AddRef"); v[2] = (void *)(uintptr_t)make_thunk(com_release, "ID3D11Device::Release"); v[3] = (void *)(uintptr_t)make_thunk(device_create_buffer, "ID3D11Device::CreateBuffer"); v[5] = (void *)(uintptr_t)make_thunk(device_create_texture, "ID3D11Device::CreateTexture2D"); v[7] = (void *)(uintptr_t)make_thunk(device_create_srv, "ID3D11Device::CreateShaderResourceView"); v[9] = (void *)(uintptr_t)make_thunk(device_create_rtv, "ID3D11Device::CreateRenderTargetView"); v[10] = (void *)(uintptr_t)make_thunk(device_create_dsv, "ID3D11Device::CreateDepthStencilView"); v[23] = (void *)(uintptr_t)make_thunk(device_create_sampler, "ID3D11Device::CreateSamplerState"); v[40] = (void *)(uintptr_t)make_thunk(device_get_context, "ID3D11Device::GetImmediateContext"); done = 1; }
  return v;
}
static void **context_vtable(void) {
  static void *v[44]; static int done;
  if (!done) { for (int i = 0; i < 44; i++) { char n[64]; snprintf(n, sizeof n, "ID3D11DeviceContext::vtbl[%d] (not implemented)", i); v[i] = (void *)(uintptr_t)make_thunk(NULL, n); } v[0] = (void *)(uintptr_t)make_thunk(com_qi, "ID3D11DeviceContext::QueryInterface"); v[1] = (void *)(uintptr_t)make_thunk(ctx_addref, "ID3D11DeviceContext::AddRef"); v[2] = (void *)(uintptr_t)make_thunk(ctx_release, "ID3D11DeviceContext::Release"); v[7] = (void *)(uintptr_t)make_thunk(ctx_set_cb, "ID3D11DeviceContext::VSSetConstantBuffers"); v[8] = (void *)(uintptr_t)make_thunk(ctx_set_srv, "ID3D11DeviceContext::PSSetShaderResources"); v[9] = (void *)(uintptr_t)make_thunk(ctx_set_sampler, "ID3D11DeviceContext::PSSetSamplers"); v[12] = (void *)(uintptr_t)make_thunk(ctx_draw, "ID3D11DeviceContext::Draw"); v[13] = (void *)(uintptr_t)make_thunk(ctx_set_layout, "ID3D11DeviceContext::IASetInputLayout"); v[20] = (void *)(uintptr_t)make_thunk(ctx_set_topology, "ID3D11DeviceContext::IASetPrimitiveTopology"); v[29] = (void *)(uintptr_t)make_thunk(ctx_set_targets, "ID3D11DeviceContext::OMSetRenderTargets"); v[41] = (void *)(uintptr_t)make_thunk(ctx_clear_rtv, "ID3D11DeviceContext::ClearRenderTargetView"); v[43] = (void *)(uintptr_t)make_thunk(ctx_clear_dsv, "ID3D11DeviceContext::ClearDepthStencilView"); done = 1; }
  return v;
}
uint64_t aot_d3d11_create_device_from_cpu(CPU *c) {
  uint64_t ppdev = LD(uint64_t, c->r[4] + 8 + 8 * 4), ppfl = LD(uint64_t, c->r[4] + 8 + 8 * 5), ppctx = LD(uint64_t, c->r[4] + 8 + 8 * 7); AOT_VK *vk = 0; AOT_CONTEXT *ctx = 0;
  if (aot_vk_create(0, 1, 1, &vk) != AOT_OK || aot_d3d11_create(vk, &ctx) != AOT_OK) return (uint64_t)-1;
  AOT_COM_DEVICE *d = (AOT_COM_DEVICE *)calloc(1, sizeof *d); AOT_COM_CONTEXT *x = (AOT_COM_CONTEXT *)calloc(1, sizeof *x); if (!d || !x) return (uint64_t)-1;
  d->vt = device_vtable(); d->refs = 1; d->ctx = ctx; d->comctx = x; x->vt = context_vtable(); x->refs = 1; x->ctx = ctx;
  if (ppdev) ST(uint64_t, ppdev, d);
  if (ppctx) ST(uint64_t, ppctx, x);
  if (ppfl) ST(uint32_t, ppfl, 0xB000);
  return 0;
}
