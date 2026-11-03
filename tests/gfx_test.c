#include <assert.h>
#include <string.h>
#include "../src/runtime/gfx.h"
uint64_t aot_register_thunk(aot_host_fn fn, const char *name) { static uint64_t p = 0x6fff00000020ULL; (void)fn; (void)name; return p += 16; }
int main(void) {
  AOT_VK *vk = 0; AOT_CONTEXT *c = 0; AOT_BUFFER *b = 0; AOT_TEXTURE2D *t = 0; AOT_VIEW *v = 0; AOT_VIEW *srv = 0; AOT_VIEW *rtv = 0; AOT_VIEW *dsv = 0; AOT_SAMPLER *sampler = 0; AOT_INPUT_LAYOUT *l = 0;
  AOT_BUFFER_DESC bd = { 64, 1, 0, AOT_USAGE_DYNAMIC }; unsigned char init[64] = { 1, 2, 3 };
  AOT_SUBRESOURCE_DATA sd = { init, 64, 64 }; AOT_TEXTURE2D_DESC td = { 2, 2, 1, 1, 1, AOT_FORMAT_RGBA8, AOT_USAGE_DEFAULT };
  assert(aot_vk_create((ANativeWindow *)1, 320, 200, &vk) == AOT_OK); assert(aot_d3d11_create(vk, &c) == AOT_OK);
  assert(aot_create_buffer(c, &bd, &sd, &b) == AOT_OK); assert(aot_create_texture2d(c, &td, &sd, &t) == AOT_OK);
  AOT_SRV_DESC srvd = { AOT_FORMAT_RGBA8, 0, 1 }; assert(aot_create_srv(c, t, &srvd, &srv) == AOT_OK); aot_set_shader_resources(c, 2, srv); assert(aot_get_shader_resource(c, 2) == srv);
  AOT_RTV_DESC rtvd = { AOT_FORMAT_RGBA8, 0 }; AOT_DSV_DESC dsvd = { AOT_FORMAT_R32_FLOAT, 0, 0, 0 }; assert(aot_create_rtv(c, t, &rtvd, &rtv) == AOT_OK); assert(aot_create_dsv(c, t, &dsvd, &dsv) == AOT_OK); AOT_VIEW *targets[1] = { rtv }; aot_set_render_targets(c, 1, targets, dsv); assert(aot_get_render_target(c, 0) == rtv && aot_get_depth_stencil(c) == dsv); assert(aot_clear(c, rtv, 0, 0, 0, 1) == AOT_OK && aot_clear(c, dsv, 0, 0, 0, 1) == AOT_OK);
  AOT_SAMPLER_DESC smpd = { 1, 1, 1, 1, 0, 1, 0, 1000 }; assert(aot_create_sampler(c, &smpd, &sampler) == AOT_OK); aot_set_samplers(c, 2, sampler); assert(aot_get_sampler(c, 2) == sampler);
  AOT_CBV *cbv = 0; unsigned char constants[32] = { 9 }; assert(aot_create_cbv(c, b, 1, 2, &cbv) == AOT_OK); aot_set_constant_buffer(c, 3, cbv); assert(aot_get_constant_buffer(c, 3) == cbv); AOT_UNIFORM_BUFFER_INFO ui = aot_uniform_buffer_info(cbv); assert(ui.Offset == 16 && ui.Range == 32 && ui.Buffer == b); assert(aot_update_constant_buffer(c, cbv, constants, sizeof constants) == AOT_OK);
  AOT_INPUT_ELEMENT_DESC elems[2] = {{"POSITION", 0, AOT_FORMAT_R32G32B32_FLOAT, 0, UINT32_MAX, AOT_PER_VERTEX_DATA, 0}, {"TEXCOORD", 0, AOT_FORMAT_R32G32_FLOAT, 0, UINT32_MAX, AOT_PER_VERTEX_DATA, 0}};
  assert(aot_create_input_layout(c, elems, 2, &l) == AOT_OK); aot_set_input_layout(c, l); aot_set_primitive_topology(c, AOT_TOPOLOGY_TRIANGLELIST); assert(aot_primitive_topology(c) == AOT_TOPOLOGY_TRIANGLELIST);
  assert(aot_create_texture_view(c, t, 0, &v) == AOT_OK); assert(aot_vk_begin_frame(vk) == AOT_OK);
  assert(aot_draw(c, 3, 0) == AOT_OK); assert(aot_draw_count(c) == 1); assert(aot_vk_end_frame(vk) == AOT_OK);
  aot_release_view(v); aot_release_view(srv); aot_release_view(rtv); aot_release_view(dsv); aot_release_sampler(sampler); aot_release_input_layout(l); aot_release_cbv(cbv); aot_release_texture2d(t); aot_release_buffer(b); aot_d3d11_destroy(c); aot_vk_destroy(vk); return 0;
}
