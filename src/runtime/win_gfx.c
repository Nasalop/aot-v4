/* Minimal D3D11 + DXGI + user32 layer: device, swapchain, and a constrained draw batch per present.
   COM vtable slot numbers follow d3d11.h / dxgi.h; every slot not implemented aborts with its interface and number. */
#include "shims.h"
#include "gfx_backend.h"
#include "gfx_draw_batch.h"
#include "dxbc_translate.h"
#include <time.h>
typedef uint64_t (*fn_t)(CPU *);
/* PE32 guests: objects/vtables/buffers live below 4GB, guest pointers are 4 bytes, COM methods are stdcall (see win_x86.c) */
#define calloc aot_gcalloc
#define malloc aot_gmalloc
#define realloc aot_grealloc
#define free aot_gfree
#define PSZ (aot_is_pe32 ? 4 : 8)
#define STP(a, v) do { uintptr_t _pv = (uintptr_t)(v); if (aot_is_pe32) ST(uint32_t, (a), _pv); else ST(uint64_t, (a), _pv); } while (0)
#define LDP(a) (aot_is_pe32 ? (uint64_t)LD(uint32_t, (a)) : LD(uint64_t, (a)))
#define SCD_SIZE (aot_is_pe32 ? 60 : 72)    /* DXGI_SWAP_CHAIN_DESC */
void aot_thunk_set_pop(uint64_t addr, int pop);
#define ARGN(c, n) ((n) == 0 ? (c)->r[1] : (n) == 1 ? (c)->r[2] : (n) == 2 ? (c)->r[8] : (n) == 3 ? (c)->r[9] : LD(uint64_t, (c)->r[4] + 8 + 8 * (n)))
static inline uint64_t argn(CPU *c, int n) { return ARGN(c, n); }
#define A(n) argn(c, (n))
#define F(name) static uint64_t name(CPU *c)
#define S_OK 0
#define E_FAIL 0x80004005u
#define E_NOTIMPL 0x80004001u
#define E_INVALIDARG 0x80070057u
#define DXGI_NOT_FOUND 0x887A0002u

typedef enum { K_DEV = 1, K_CTX, K_DXDEV, K_ADAPTER, K_FACTORY, K_SC, K_TEX, K_RTV, K_VS, K_PS, K_BUF, K_LAYOUT, K_SRV, K_SAMPLER, K_BLEND, K_RASTERIZER, K_OUTPUT } Kind;
typedef struct Obj {
  void **vt; uint32_t refs; Kind kind; struct Obj *dev, *ctx, *res, *bb, *adapter, *factory, *dxdev, *cur_rtv;
  struct Obj *cur_vs, *cur_ps, *cur_layout, *cur_ib, *cur_blend, *cur_rasterizer;
  struct Obj *cur_vb[AOT_GFX_MAX_VERTEX_STREAMS];
  struct Obj *cur_vs_cb[AOT_GFX_MAX_CONSTANT_BUFFERS];
  struct Obj *cur_srv[AOT_GFX_MAX_TEXTURES], *cur_sampler[AOT_GFX_MAX_TEXTURES];
  float clear[4]; uint32_t tex[11]; uint8_t scdesc[72]; uint64_t frames;
  uint32_t *spirv; size_t spirv_size;
  uint8_t *data; size_t data_size; uint32_t stride[AOT_GFX_MAX_VERTEX_STREAMS];
  uint32_t vertex_offset[AOT_GFX_MAX_VERTEX_STREAMS], index_offset, index_format;
  AOT_PRESENT_TEXTURE *backend_texture; AOT_PRESENT_SAMPLER *backend_sampler;
  AOT_PRESENT_VERTEX_ATTRIBUTE attributes[16]; uint32_t attribute_count;
  AOT_GFX_DRAW_BATCH *draw_batch;
  uint32_t topology, draw_error, presented_frames, peak_draws;
  uint64_t presented_draws, uploaded_vertex_bytes, uploaded_constant_bytes;
  uint64_t uploaded_index_bytes, uploaded_descriptor_sets;
  uint8_t buffer_desc[24];
  uint8_t blend_desc[264];
  uint8_t rasterizer_desc[40];
  uint8_t alpha_blend;
  uint8_t mapped;
  uint8_t has_viewport, has_scissor;
  AOT_GFX_VIEWPORT viewport;
  AOT_GFX_SCISSOR scissor;
} Obj;
static Obj *mkv(Kind k, Obj *dev);
static Obj *mk(Kind k, Obj *dev) { Obj *o = calloc(1, sizeof *o); if (o) { o->kind = k; o->refs = 1; o->dev = dev; } return o; }
static void clear_context_state(Obj *ctx);
static const uint8_t IID_IDXGIDevice[16] = { 0xfa, 0x77, 0xec, 0x54, 0x77, 0x13, 0xe6, 0x44, 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c };
static uint32_t g_w, g_h; static int g_pinit;

typedef struct { int idx; fn_t fn; const char *name; } Slot;
/* parameter count INCLUDING `this` for every COM method this layer implements; -1 = unknown (PE32: bound as unimplemented, never guessed) */
static int method_argc(const char *iface, const char *m) {
  int d10 = !strncmp(iface, "ID3D10", 6), ctx = !strcmp(iface, "ID3D11DeviceContext"), dxgi = !strncmp(iface, "IDXGI", 5);
  static const struct { const char *n; int a; } t[] = {
    {"QueryInterface",3},{"AddRef",1},{"Release",1},{"CheckFormatSupport",3},{"CheckInterfaceSupport",3},{"CheckMultisampleQualityLevels",4},
    {"ClearRenderTargetView",3},{"ClearState",1},{"CreateBlendState",3},{"CreateBuffer",4},{"CreateInputLayout",6},{"CreateRasterizerState",3},
    {"CreateRenderTargetView",4},{"CreateSamplerState",3},{"CreateShaderResourceView",4},{"CreateSwapChain",4},{"CreateTexture2D",4},
    {"Draw",3},{"DrawIndexed",4},{"EnumAdapters",3},{"EnumAdapters1",3},{"EnumOutputs",3},{"Flush",1},{"GetAdapter",2},{"GetBuffer",4},{"GetCreationFlags",1},
    {"GetDesc",2},{"GetDeviceRemovedReason",1},{"GetEvictionPriority",1},{"GetFeatureLevel",1},{"GetFullscreenState",3},{"GetGPUThreadPriority",2},
    {"GetImmediateContext",2},{"GetParent",3},{"GetResource",2},{"GetType",2},{"IASetIndexBuffer",4},{"IASetInputLayout",2},{"IASetPrimitiveTopology",2},
    {"IASetVertexBuffers",6},{"IsCurrent",1},{"MakeWindowAssociation",3},{"OMSetBlendState",4},{"OMSetRenderTargets",4},{"PSSetSamplers",4},
    {"PSSetShaderResources",4},{"Present",3},{"RSSetScissorRects",3},{"RSSetState",2},{"RSSetViewports",3},{"ResizeBuffers",6},{"ResizeTarget",2},
    {"SetEvictionPriority",2},{"SetExceptionMode",2},{"SetFullscreenState",3},{"SetGPUThreadPriority",2},{"SetPrivateData",4},{"UpdateSubresource",7},
    {"VSSetConstantBuffers",4}, {"GetDisplayModeList",5},{"FindClosestMatchingMode",4},{"WaitForVBlank",1},{"TakeOwnership",3},{"ReleaseOwnership",1},{"GetGammaControlCapabilities",2},{"SetGammaControl",2},{"GetGammaControl",2},{"SetDisplaySurface",2},{"GetDisplaySurfaceData",2},{"GetFrameStatistics",2}, {0,0} };
  if (!strcmp(m, "GetDevice")) return dxgi ? 3 : 2;
  if (!strcmp(m, "CreateVertexShader") || !strcmp(m, "CreatePixelShader")) return d10 ? 4 : 5;
  if (!strcmp(m, "PSSetShader") || !strcmp(m, "VSSetShader")) return d10 ? 2 : 4;
  if (!strcmp(m, "Map")) return ctx ? 6 : 4;
  if (!strcmp(m, "Unmap")) return ctx ? 3 : 2;
  for (int i = 0; t[i].n; i++) if (!strcmp(t[i].n, m)) return t[i].a;
  return -1;
}
static void **build(const char *iface, int n, const Slot *s) {
  const int pad = n + 64;   /* guests may call interface methods we do not model (or a newer interface): slots past the end must hit a loud named thunk, never heap garbage */
  void **vt = calloc(pad, aot_is_pe32 ? 4 : sizeof(void *)); char nm[96];
  uint32_t *v32 = (uint32_t *)vt;
#define VSET(i, a) do { if (aot_is_pe32) v32[i] = (uint32_t)(a); else vt[i] = (void *)(uintptr_t)(a); } while (0)
  for (int i = 0; i < pad; i++) { snprintf(nm, sizeof nm, "%s::vtbl[%d] (not implemented)", iface, i); VSET(i, aot_register_thunk(NULL, nm)); }
  for (; s->fn; s++) {
    snprintf(nm, sizeof nm, "%s::%s", iface, s->name); int argc = aot_is_pe32 ? method_argc(iface, s->name) : 0;
    fn_t fn = (aot_is_pe32 && argc < 0) ? NULL : s->fn;   /* unknown stdcall size in PE32 -> unimplemented, loudly */
    uint64_t t = aot_register_thunk(fn, nm); if (aot_is_pe32 && argc >= 0) aot_thunk_set_pop(t, 4 * argc);
    VSET(s->idx, t);
  }
#undef VSET
  return vt;
}

/* ---- IUnknown / IDXGIObject / IDXGIDeviceSubObject / ID3D11DeviceChild shared ---- */
F(u_addref) { return ++((Obj *)A(0))->refs; }
static uint32_t release_obj(Obj *o) {
  if (!o || !o->refs) return 0;
  if (--o->refs == 0) {
    if (o->kind == K_VS || o->kind == K_PS)
      aot_present_graphics_cleanup();
    if (o->spirv) aot_dxbc_free_spirv(o->spirv);
    if (o->kind == K_TEX && o->backend_texture) aot_present_texture_destroy(o->backend_texture);
    if (o->kind == K_SAMPLER && o->backend_sampler) aot_present_sampler_destroy(o->backend_sampler);
    if (o->kind == K_SRV && o->res) release_obj(o->res);
    if (o->kind == K_RTV && o->res) release_obj(o->res);
    if (o->kind == K_SC && o->bb) release_obj(o->bb);
    if (o->kind == K_SC && o->dev) release_obj(o->dev);
    if (o->kind == K_DEV && o->ctx) release_obj(o->ctx);
    if (o->kind == K_CTX) {
      clear_context_state(o);
      aot_gfx_draw_batch_destroy(o->draw_batch);
      o->draw_batch = NULL;
    }
    free(o->data);
    o->spirv = NULL; o->data = NULL; o->backend_texture = NULL; o->backend_sampler = NULL;
  }
  return o->refs;
}
F(u_release) { return release_obj((Obj *)A(0)); }
F(u_ok) { return S_OK; }
/* PE32 (D3D10/DXGI guests): only answer for interfaces this layer actually implements; everything else is E_NOINTERFACE so the game falls back
 * (ID3D10Device1, debug/info-queue, multithread ... have different vtable layouts and must never alias our objects). */
static int iid_supported(const uint8_t *g) {
  static const uint8_t unk[16] = { 0,0,0,0, 0,0, 0,0, 0xC0,0,0,0,0,0,0,0x46 };
  static const uint8_t dxgi[][16] = {   /* IDXGIObject, IDXGIDeviceSubObject, IDXGIFactory, IDXGIAdapter, IDXGISwapChain */
    { 0xb8,0x2f,0xc2,0xae, 0xf3,0x76, 0x39,0x46, 0x9b,0xe0,0x28,0xeb,0x43,0xa6,0x7a,0x2e },
    { 0x79,0x03,0x3e,0x3d, 0xde,0xf9, 0x58,0x4d, 0xbb,0x6c,0x18,0xd6,0x29,0x92,0xf1,0xa6 },
    { 0xec,0x66,0x71,0x7b, 0xc7,0x21, 0xae,0x44, 0xb2,0x1a,0xc9,0xae,0x32,0x1a,0xe3,0x69 },
    { 0xe1,0xe7,0x11,0x24, 0xac,0x12, 0xcf,0x4c, 0xbd,0x14,0x97,0x98,0xe8,0x53,0x4d,0xc0 },
    { 0xa0,0x36,0x0d,0x31, 0xe4,0xd2, 0x4a,0x4b, 0xaa,0x04,0x6a,0x9d,0x23,0xb8,0x88,0x6a } };
  static const uint8_t d10tail[10] = { 0x2c,0x34, 0x06,0x41, 0xa1,0x9f,0x4f,0x27,0x04,0xf6 };   /* 9B7E4Cxx-342C-4106-A19F-4F2704F689F0 (base ID3D10 family) */
  if (!memcmp(g, unk, 16) || !memcmp(g, IID_IDXGIDevice, 16)) return 1;
  for (unsigned i = 0; i < sizeof dxgi / 16; i++) if (!memcmp(g, dxgi[i], 16)) return 1;
  return g[1] == 0x4c && g[2] == 0x7e && g[3] == 0x9b && g[0] <= 0x1f && !memcmp(g + 4, d10tail, 10) && g[14] == 0x89 && g[15] == 0xf0;
}
F(u_qi) {
  Obj *o = (Obj *)A(0), *r = o; const uint8_t *iid = (const uint8_t *)A(1);
  if (aot_is_pe32 && !iid_supported(iid)) { STP(A(2), NULL); return 0x80004002u; }   /* E_NOINTERFACE */
  if (o->kind == K_DEV && !memcmp(iid, IID_IDXGIDevice, 16)) { if (!o->dxdev) o->dxdev = mkv(K_DXDEV, o); r = o->dxdev; }
  STP(A(2), r); r->refs++; return S_OK;
}
F(c_getdevice) { Obj *o = (Obj *)A(0); STP(A(1), o->dev); o->dev->refs++; return 0; }          /* DeviceChild::GetDevice(ppDevice) */
F(sc_getdevice) { Obj *o = (Obj *)A(0); STP(A(2), o->dev); o->dev->refs++; return S_OK; }     /* DXGISubObject::GetDevice(riid, ppDevice) */

/* ---- factory / adapter / dxgi device ---- */
static Obj *get_adapter(Obj *any, Obj *fac) { Obj *d = any->dev ? any->dev : any; if (!d->adapter) { d->adapter = mkv(K_ADAPTER, d); d->adapter->factory = fac; } return d->adapter; }
static Obj *get_factory(void) { static Obj *f; if (!f) f = mkv(K_FACTORY, NULL); return f; }
F(f_enumadapters) { if ((uint32_t)A(1)) return DXGI_NOT_FOUND; Obj *a = get_adapter(get_factory(), get_factory()); STP(A(2), a); return S_OK; }
F(f_makeassoc) { return S_OK; }
F(f_iscurrent) { return 1; }
F(adapter_getparent) { STP(A(2), get_factory()); return S_OK; }
F(adapter_getdesc) { uint8_t *d = (uint8_t *)A(1); memset(d, 0, aot_is_pe32 ? 292 : 304); const char *n = "AOT Vulkan GPU"; for (int i = 0; n[i]; i++) ST(uint16_t, d + 2 * i, n[i]); ST(uint32_t, d + 256, 0x1AE0); if (aot_is_pe32) ST(uint32_t, d + 272, 3u << 30); else ST(uint64_t, d + 272, 4ULL << 30); return S_OK; }
/* IDXGIOutput: one primary output. The Vulkan surface scales to the window, so the output advertises a fixed set of standard modes (not a monitor query). */
static Obj *get_output(Obj *adapter) { static Obj *o; if (!o) o = mkv(K_OUTPUT, adapter->dev); return o; }
F(adapter_enumoutputs) { if ((uint32_t)A(1)) return DXGI_NOT_FOUND; Obj *o = get_output((Obj *)A(0)); STP(A(2), o); o->refs++; return S_OK; }
F(out_getdesc) { uint8_t *d = (uint8_t *)A(1); if (!d) return E_INVALIDARG; memset(d, 0, aot_is_pe32 ? 92 : 96); const char *n = "\\\\.\\DISPLAY1"; for (int i = 0; n[i]; i++) ST(uint16_t, d + 2 * i, n[i]);
  ST(int32_t, d + 72, 1920); ST(int32_t, d + 76, 1080); ST(uint32_t, d + 80, 1); ST(uint32_t, d + 84, 1);   /* RECT 0,0,1920,1080; attached; DXGI_MODE_ROTATION_IDENTITY */
  if (aot_is_pe32) ST(uint32_t, d + 88, 1); else ST(uint64_t, d + 88, 1); return S_OK; }
F(out_getmodes) {   /* GetDisplayModeList(format, flags, *count, *modes) */
  static const uint32_t m[][2] = { {640,480}, {800,600}, {1024,768}, {1280,720}, {1280,1024}, {1366,768}, {1600,900}, {1920,1080} }; const uint32_t n = sizeof m / sizeof m[0];
  uint32_t fmt = (uint32_t)A(1); uint32_t *cnt = (uint32_t *)A(3); uint8_t *out = (uint8_t *)A(4); if (!cnt) return E_INVALIDARG;
  if (fmt != 28 && fmt != 87 && fmt != 24 && fmt != 10) { *cnt = 0; return S_OK; }   /* only 32-bit RGB scanout formats */
  if (!out) { *cnt = n; return S_OK; }
  uint32_t k = *cnt < n ? *cnt : n; for (uint32_t i = 0; i < k; i++) { uint8_t *e = out + 28 * i; ST(uint32_t, e, m[i][0]); ST(uint32_t, e + 4, m[i][1]); ST(uint32_t, e + 8, 60); ST(uint32_t, e + 12, 1); ST(uint32_t, e + 16, fmt); ST(uint32_t, e + 20, 0); ST(uint32_t, e + 24, 0); }
  *cnt = k; return k < n ? 0x887A0003u : S_OK; }   /* DXGI_ERROR_MORE_DATA */
F(out_findclosest) { const uint8_t *want = (const uint8_t *)A(1); uint8_t *got = (uint8_t *)A(2); if (!want || !got) return E_INVALIDARG; memcpy(got, want, 28); if (!LD(uint32_t, got + 8)) { ST(uint32_t, got + 8, 60); ST(uint32_t, got + 12, 1); } return S_OK; }
F(out_unsupported) { return 0x887A0004u; }   /* DXGI_ERROR_UNSUPPORTED */
F(dxdev_getadapter) { Obj *o = (Obj *)A(0); STP(A(1), get_adapter(o, get_factory())); return S_OK; }
F(dxdev_getparent) { return dxdev_getadapter(c); }

/* ---- device ---- */
F(d_createtex2d) {
  if (A(3)) STP(A(3), 0);
  if (!A(1) || !A(3)) return E_INVALIDARG;
  const uint8_t *desc = (const uint8_t *)A(1);
  uint32_t width = LD(uint32_t, desc), height = LD(uint32_t, desc + 4);
  uint32_t mip_levels = LD(uint32_t, desc + 8), array_size = LD(uint32_t, desc + 12);
  uint32_t format = LD(uint32_t, desc + 16), samples = LD(uint32_t, desc + 20);
  uint32_t usage = LD(uint32_t, desc + 28), bind_flags = LD(uint32_t, desc + 32);
  if (!width || !height || width > 4096 || height > 4096 ||
      (size_t)width > SIZE_MAX / 4 / height || format != 28 ||
      mip_levels != 1 || array_size != 1 || samples != 1 ||
      usage != 0 || !(bind_flags & 8u) || (bind_flags & ~8u) ||
      LD(uint32_t, desc + 36) || LD(uint32_t, desc + 40))
    return E_NOTIMPL;
  Obj *texture = mkv(K_TEX, (Obj *)A(0));
  if (!texture) return E_FAIL;
  memcpy(texture->tex, desc, sizeof texture->tex);
  const uint8_t *initial = A(2) ? (const uint8_t *)A(2) : NULL;
  if (initial) {
    const uint8_t *pixels = (const uint8_t *)LDP(initial);
    uint32_t row_pitch = LD(uint32_t, initial + 8);
    size_t row_size = (size_t)width * 4;
    if (!pixels || row_pitch < row_size || row_pitch > SIZE_MAX / height) {
      release_obj(texture);
      return E_INVALIDARG;
    }
    texture->data_size = row_size * height;
    texture->data = malloc(texture->data_size);
    if (!texture->data) { release_obj(texture); return E_FAIL; }
    for (uint32_t y = 0; y < height; y++)
      memcpy(texture->data + (size_t)y * row_size, pixels + (size_t)y * row_pitch, row_size);
    if (aot_present_texture_create(width, height, texture->data, &texture->backend_texture)) {
      fprintf(stderr, "[gfx] failed to upload D3D11 texture to Vulkan\n");
      release_obj(texture);
      return E_FAIL;
    }
  }
  STP(A(3), texture);
  return S_OK;
}
F(d_createsrv) {
  if (A(3)) STP(A(3), 0);
  Obj *texture = (Obj *)A(1);
  if (!A(3) || !texture || texture->kind != K_TEX || !texture->backend_texture) return E_INVALIDARG;
  if (A(2)) {
    const uint8_t *desc = (const uint8_t *)A(2);
    if (LD(uint32_t, desc) != 28 || LD(uint32_t, desc + 4) != 3 ||
        LD(uint32_t, desc + 8) != 0 || LD(uint32_t, desc + 12) != 1)
      return E_NOTIMPL;
  }
  Obj *view = mkv(K_SRV, (Obj *)A(0));
  if (!view) return E_FAIL;
  view->res = texture;
  texture->refs++;
  STP(A(3), view);
  return S_OK;
}
F(d_createsampler) {
  if (A(2)) STP(A(2), 0);
  if (!A(1) || !A(2)) return E_INVALIDARG;
  const uint8_t *desc = (const uint8_t *)A(1);
  Obj *sampler = mkv(K_SAMPLER, (Obj *)A(0));
  if (!sampler) return E_FAIL;
  uint32_t filter = LD(uint32_t, desc), address_u = LD(uint32_t, desc + 4);
  uint32_t address_v = LD(uint32_t, desc + 8), address_w = LD(uint32_t, desc + 12);
  if (aot_present_sampler_create(filter, address_u, address_v, address_w,
                                 &sampler->backend_sampler)) {
    release_obj(sampler);
    return E_NOTIMPL;
  }
  STP(A(2), sampler);
  return S_OK;
}
F(d_createbuffer) {
  if (A(3)) STP(A(3), 0);
  if (!A(1) || !A(3)) return E_INVALIDARG;
  uint32_t byte_width = LD(uint32_t, A(1));
  if (!byte_width || byte_width > 64u * 1024u * 1024u) return E_INVALIDARG;
  uint32_t usage = LD(uint32_t, A(1) + 4), bind_flags = LD(uint32_t, A(1) + 8);
  uint32_t cpu_access = LD(uint32_t, A(1) + 12);
  if ((usage != 0 && usage != 2) ||
      (usage == 0 && cpu_access != 0) ||
      (usage == 2 && cpu_access != 0x10000u) ||
      (bind_flags & ~(1u | 2u | 4u))) return E_NOTIMPL;
  if ((bind_flags & 4u) && ((usage != 0 && usage != 2) ||
      bind_flags != 4u ||
      (byte_width & 15u) || byte_width > 65536u))
    return E_INVALIDARG;
  Obj *buffer = mkv(K_BUF, (Obj *)A(0));
  if (!buffer) return E_FAIL;
  buffer->data = calloc(1, byte_width);
  if (!buffer->data) { free(buffer); return E_FAIL; }
  buffer->data_size = byte_width;
  memcpy(buffer->buffer_desc, (const void *)A(1), sizeof buffer->buffer_desc);
  if (A(2)) {
    const void *initial = (const void *)LDP(A(2));
    if (!initial) { release_obj(buffer); return E_INVALIDARG; }
    memcpy(buffer->data, initial, byte_width);
  }
  STP(A(3), buffer);
  return S_OK;
}
F(d_createblendstate) {
  if (A(2)) STP(A(2), 0);
  if (!A(1) || !A(2)) return E_INVALIDARG;
  const uint8_t *desc = (const uint8_t *)A(1);
  uint32_t alpha_to_coverage = LD(uint32_t, desc);
  uint32_t independent_blend = LD(uint32_t, desc + 4);
  const uint8_t *target = desc + 8;
  uint32_t blend_enable = LD(uint32_t, target);
  uint32_t write_mask = target[28];
  if (alpha_to_coverage || independent_blend || write_mask != 0x0f)
    return E_NOTIMPL;
  if (blend_enable &&
      (LD(uint32_t, target + 4) != 5u ||
       LD(uint32_t, target + 8) != 6u ||
       LD(uint32_t, target + 12) != 1u ||
       LD(uint32_t, target + 16) != 2u ||
       LD(uint32_t, target + 20) != 6u ||
       LD(uint32_t, target + 24) != 1u))
    return E_NOTIMPL;
  Obj *state = mkv(K_BLEND, (Obj *)A(0));
  if (!state) return E_FAIL;
  memcpy(state->blend_desc, desc, sizeof state->blend_desc);
  state->alpha_blend = (uint8_t)(blend_enable != 0);
  STP(A(2), state);
  return S_OK;
}
F(d_createrasterizerstate) {
  if (A(2)) STP(A(2), 0);
  if (!A(1) || !A(2)) return E_INVALIDARG;
  const uint8_t *desc = (const uint8_t *)A(1);
  uint32_t fill_mode = LD(uint32_t, desc);
  uint32_t cull_mode = LD(uint32_t, desc + 4);
  uint32_t front_counter_clockwise = LD(uint32_t, desc + 8);
  uint32_t depth_clip = LD(uint32_t, desc + 24);
  uint32_t scissor_enable = LD(uint32_t, desc + 28);
  uint32_t multisample = LD(uint32_t, desc + 32);
  uint32_t antialiased_line = LD(uint32_t, desc + 36);
  if (front_counter_clockwise > 1 || depth_clip > 1 ||
      scissor_enable > 1 || multisample > 1 || antialiased_line > 1)
    return E_INVALIDARG;
  if (fill_mode != 3u || cull_mode < 1u || cull_mode > 3u ||
      LD(int32_t, desc + 12) != 0 || LD(float, desc + 16) != 0.0f ||
      LD(float, desc + 20) != 0.0f || !depth_clip || multisample ||
      antialiased_line)
    return E_NOTIMPL;
  Obj *state = mkv(K_RASTERIZER, (Obj *)A(0));
  if (!state) return E_FAIL;
  memcpy(state->rasterizer_desc, desc, sizeof state->rasterizer_desc);
  STP(A(2), state);
  return S_OK;
}
static uint32_t input_format(uint32_t format, uint32_t *size) {
  switch (format) {
    case 2: *size = 16; return AOT_PRESENT_FORMAT_R32G32B32A32_FLOAT;
    case 6: *size = 12; return AOT_PRESENT_FORMAT_R32G32B32_FLOAT;
    case 16: *size = 8; return AOT_PRESENT_FORMAT_R32G32_FLOAT;
    case 41: *size = 4; return AOT_PRESENT_FORMAT_R32_FLOAT;
    case 42: *size = 4; return AOT_PRESENT_FORMAT_R32_UINT;
    case 17: *size = 8; return AOT_PRESENT_FORMAT_R32G32_UINT;
    case 28: *size = 4; return AOT_PRESENT_FORMAT_R8G8B8A8_UNORM;
    default: *size = 0; return 0;
  }
}
F(d_createinputlayout) {
  if (A(5)) STP(A(5), 0);
  if (!A(1) || !A(2) || A(2) > 16 || !A(3) || !A(4) || !A(5)) return E_INVALIDARG;
  Obj *layout = mkv(K_LAYOUT, (Obj *)A(0));
  if (!layout) return E_FAIL;
  layout->attribute_count = (uint32_t)A(2);
  for (uint32_t i = 0; i < layout->attribute_count; i++) {
    const int eo = aot_is_pe32 ? -4 : 0; const uint8_t *element = (const uint8_t *)A(1) + (size_t)i * (aot_is_pe32 ? 28 : 32);
    uint32_t slot = LD(uint32_t, element + 16 + eo), offset = LD(uint32_t, element + 20 + eo);
    uint32_t slot_class = LD(uint32_t, element + 24 + eo), step_rate = LD(uint32_t, element + 28 + eo);
    uint32_t size = 0, format = input_format(LD(uint32_t, element + 12 + eo), &size);
    if (!LDP(element) || !format ||
        slot >= AOT_GFX_MAX_VERTEX_STREAMS || slot_class != 0 ||
        step_rate != 0 || offset > 4096 || size > 4096 - offset) {
      release_obj(layout);
      return E_NOTIMPL;
    }
    layout->attributes[i] = (AOT_PRESENT_VERTEX_ATTRIBUTE){ i, format, slot, offset };
  }
  STP(A(5), layout);
  return S_OK;
}
F(d_creatertv) { Obj *v = mkv(K_RTV, (Obj *)A(0)); if (!v) return E_FAIL; v->res = (Obj *)A(1); if (v->res) v->res->refs++; STP(A(3), v); return S_OK; }
F(d_getimmctx) { Obj *d = (Obj *)A(0); STP(A(1), d->ctx); d->ctx->refs++; return 0; }
F(d_featurelevel) { return 0xB000; }
F(d_zero) { return 0; }
F(d_checkformat) { ST(uint32_t, A(2), 0x7FFFFFFF); return S_OK; }
F(d_checkmsaa) { ST(uint32_t, A(3), (uint32_t)A(2) == 1 ? 1 : 0); return S_OK; }

/* ---- context ---- */
F(x_setrts) { Obj *x = (Obj *)A(0); x->cur_rtv = ((uint32_t)A(1) && A(2)) ? LD(Obj *, A(2)) : NULL; return 0; }
F(x_clearrtv) { Obj *v = (Obj *)A(1); memcpy(v->res->clear, (void *)A(2), 16); return 0; }
F(x_nop) { return 0; }
static int set_binding(Obj **slot, Obj *value, Kind expected) {
  if (value && value->kind != expected) {
    fprintf(stderr, "[gfx] refusing resource binding of the wrong type\n");
    return 0;
  }
  if (*slot == value) return 1;
  if (value) value->refs++;
  release_obj(*slot);
  *slot = value;
  return 1;
}
static void retain_resource(void *resource) { ((Obj *)resource)->refs++; }
static void release_resource(void *resource) { release_obj((Obj *)resource); }
F(x_setindexbuffer) {
  Obj *x = (Obj *)A(0);
  Obj *buffer = (Obj *)A(1);
  uint32_t format = (uint32_t)A(2), offset = (uint32_t)A(3);
  if (buffer && (buffer->kind != K_BUF ||
      !(LD(uint32_t, buffer->buffer_desc + 8) & 2u) ||
      (format != 57u && format != 42u) ||
      offset > buffer->data_size || (offset & (format == 57u ? 1u : 3u)))) {
    fprintf(stderr, "[gfx] refusing invalid index buffer binding\n");
    x->draw_error = 1;
    return 0;
  }
  if (x->cur_ib != buffer) {
    if (buffer) buffer->refs++;
    release_obj(x->cur_ib);
    x->cur_ib = buffer;
  }
  x->index_format = format;
  x->index_offset = offset;
  return 0;
}
F(x_setblendstate) {
  Obj *x = (Obj *)A(0);
  Obj *state = (Obj *)A(1);
  uint32_t sample_mask = (uint32_t)A(3);
  if ((state && state->kind != K_BLEND) || sample_mask != UINT32_MAX) {
    fprintf(stderr, "[gfx] unsupported output-merger blend state or sample mask\n");
    x->draw_error = 1;
    return 0;
  }
  if (x->cur_blend != state) {
    if (state) state->refs++;
    release_obj(x->cur_blend);
    x->cur_blend = state;
  }
  return 0;
}
F(x_setrasterizerstate) {
  Obj *context = (Obj *)A(0);
  if (!set_binding(&context->cur_rasterizer, (Obj *)A(1), K_RASTERIZER))
    context->draw_error = 1;
  return 0;
}
F(x_setvscbs) {
  Obj *x = (Obj *)A(0);
  uint32_t first = (uint32_t)A(1), count = (uint32_t)A(2);
  if (first > AOT_GFX_MAX_CONSTANT_BUFFERS ||
      count > AOT_GFX_MAX_CONSTANT_BUFFERS - first || (count && !A(3))) {
    fprintf(stderr, "[gfx] vertex constant-buffer slots b0-b3 are supported\n");
    x->draw_error = 1;
    return 0;
  }
  for (uint32_t i = 0; i < count; i++) {
    Obj *buffer = (Obj *)LDP(A(3) + i * PSZ);
    if (buffer && (buffer->kind != K_BUF ||
        LD(uint32_t, buffer->buffer_desc + 8) != 4u)) {
      fprintf(stderr, "[gfx] vertex slot b%u requires a constant buffer\n", first + i);
      x->draw_error = 1;
      return 0;
    }
  }
  for (uint32_t i = 0; i < count; i++)
    if (!set_binding(&x->cur_vs_cb[first + i],
        (Obj *)LDP(A(3) + i * PSZ), K_BUF))
      x->draw_error = 1;
  return 0;
}
F(x_updatesubresource) {
  Obj *context = (Obj *)A(0);
  Obj *resource = (Obj *)A(1);
  if (!resource || resource->kind != K_BUF || A(2) != 0 || A(3) || !A(4) ||
      resource->data_size != LD(uint32_t, resource->buffer_desc) ||
      LD(uint32_t, resource->buffer_desc + 4) != 0 ||
      !(LD(uint32_t, resource->buffer_desc + 8) & 4u)) {
    fprintf(stderr, "[gfx] UpdateSubresource only supports full constant-buffer updates\n");
    if (context && context->kind == K_CTX) context->draw_error = 1;
    return 0;
  }
  memcpy(resource->data, (const void *)A(4), resource->data_size);
  return S_OK;
}
F(x_map) {
  Obj *context = (Obj *)A(0);
  Obj *resource = (Obj *)A(1);
  uint32_t subresource = (uint32_t)A(2);
  uint32_t map_type = (uint32_t)A(3);
  uint32_t map_flags = (uint32_t)A(4);
  uint64_t mapped_out = A(5);
  if (!mapped_out) return E_INVALIDARG;
  memset((void *)mapped_out, 0, 16);
  if (!resource || resource->kind != K_BUF || subresource ||
      (map_type != 4u && map_type != 5u) || map_flags ||
      LD(uint32_t, resource->buffer_desc + 4) != 2u ||
      LD(uint32_t, resource->buffer_desc + 12) != 0x10000u ||
      resource->mapped) {
    fprintf(stderr, "[gfx] Map only supports one active WRITE_DISCARD/WRITE_NO_OVERWRITE map of a dynamic buffer\n");
    if (context && context->kind == K_CTX) context->draw_error = 1;
    return E_INVALIDARG;
  }
  resource->mapped = 1;
  STP(mapped_out, resource->data);
  ST(uint32_t, mapped_out + PSZ, 0);
  ST(uint32_t, mapped_out + PSZ + 4, 0);
  return S_OK;
}
F(x_unmap) {
  Obj *context = (Obj *)A(0);
  Obj *resource = (Obj *)A(1);
  if (!resource || resource->kind != K_BUF || A(2) ||
      !resource->mapped) {
    fprintf(stderr, "[gfx] Unmap requires a currently mapped buffer and subresource 0\n");
    if (context && context->kind == K_CTX) context->draw_error = 1;
    return 0;
  }
  resource->mapped = 0;
  return 0;
}
F(x_setviewports) {
  Obj *context = (Obj *)A(0);
  uint32_t count = (uint32_t)A(1);
  uint64_t viewports = A(2);
  if (count > 1 || (count && !viewports)) {
    fprintf(stderr, "[gfx] only one D3D11 viewport is supported\n");
    context->draw_error = 1;
    return 0;
  }
  context->has_viewport = (uint8_t)(count != 0);
  if (!count) return 0;
  AOT_GFX_VIEWPORT viewport = {
    LD(float, viewports), LD(float, viewports + 4),
    LD(float, viewports + 8), LD(float, viewports + 12),
    LD(float, viewports + 16), LD(float, viewports + 20)
  };
  if (!isfinite(viewport.x) || !isfinite(viewport.y) ||
      !isfinite(viewport.width) || !isfinite(viewport.height) ||
      !isfinite(viewport.min_depth) || !isfinite(viewport.max_depth) ||
      viewport.width <= 0.0f || viewport.height <= 0.0f ||
      viewport.min_depth < 0.0f || viewport.max_depth > 1.0f ||
      viewport.min_depth > viewport.max_depth) {
    fprintf(stderr, "[gfx] invalid D3D11 viewport\n");
    context->draw_error = 1;
    return 0;
  }
  context->viewport = viewport;
  return 0;
}
F(x_setscissors) {
  Obj *context = (Obj *)A(0);
  uint32_t count = (uint32_t)A(1);
  uint64_t rectangles = A(2);
  if (count > 1 || (count && !rectangles)) {
    fprintf(stderr, "[gfx] only one D3D11 scissor rectangle is supported\n");
    context->draw_error = 1;
    return 0;
  }
  context->has_scissor = (uint8_t)(count != 0);
  if (!count) return 0;
  AOT_GFX_SCISSOR scissor = {
    LD(int32_t, rectangles),
    LD(int32_t, rectangles + 4),
    LD(int32_t, rectangles + 8),
    LD(int32_t, rectangles + 12)
  };
  if (scissor.right < scissor.left || scissor.bottom < scissor.top) {
    fprintf(stderr, "[gfx] invalid D3D11 scissor rectangle\n");
    context->draw_error = 1;
    return 0;
  }
  context->scissor = scissor;
  return 0;
}
F(x_setsrv) {
  Obj *x = (Obj *)A(0);
  uint32_t first = (uint32_t)A(1), count = (uint32_t)A(2);
  if (first > AOT_GFX_MAX_TEXTURES ||
      count > AOT_GFX_MAX_TEXTURES - first || (count && !A(3))) {
    fprintf(stderr, "[gfx] pixel texture slots t0-t3 are supported\n");
    x->draw_error = 1;
    return 0;
  }
  for (uint32_t i = 0; i < count; i++) {
    Obj *view = (Obj *)LDP(A(3) + i * PSZ);
    if (view && view->kind != K_SRV) {
      fprintf(stderr, "[gfx] refusing invalid pixel shader resource at t%u\n", first + i);
      x->draw_error = 1;
      return 0;
    }
  }
  for (uint32_t i = 0; i < count; i++)
    if (!set_binding(&x->cur_srv[first + i],
        (Obj *)LDP(A(3) + i * PSZ), K_SRV))
      x->draw_error = 1;
  return 0;
}
F(x_setsampler) {
  Obj *x = (Obj *)A(0);
  uint32_t first = (uint32_t)A(1), count = (uint32_t)A(2);
  if (first > AOT_GFX_MAX_TEXTURES ||
      count > AOT_GFX_MAX_TEXTURES - first || (count && !A(3))) {
    fprintf(stderr, "[gfx] pixel sampler slots s0-s3 are supported\n");
    x->draw_error = 1;
    return 0;
  }
  for (uint32_t i = 0; i < count; i++) {
    Obj *sampler = (Obj *)LDP(A(3) + i * PSZ);
    if (sampler && sampler->kind != K_SAMPLER) {
      fprintf(stderr, "[gfx] refusing invalid pixel sampler at s%u\n", first + i);
      x->draw_error = 1;
      return 0;
    }
  }
  for (uint32_t i = 0; i < count; i++)
    if (!set_binding(&x->cur_sampler[first + i],
        (Obj *)LDP(A(3) + i * PSZ), K_SAMPLER))
      x->draw_error = 1;
  return 0;
}
static void set_shader(Obj **slot, Obj *shader, Kind expected) {
  if (shader && shader->kind != expected) {
    fprintf(stderr, "[gfx] refusing shader object of the wrong stage\n");
    return;
  }
  if (*slot == shader) return;
  if (shader) shader->refs++;
  release_obj(*slot);
  *slot = shader;
}
F(x_setvs) { Obj *x = (Obj *)A(0); if (A(2) || A(3)) { fprintf(stderr, "[gfx] shader class instances are unsupported\n"); set_shader(&x->cur_vs, NULL, K_VS); return 0; } set_shader(&x->cur_vs, (Obj *)A(1), K_VS); return 0; }
F(x_setps) { Obj *x = (Obj *)A(0); if (A(2) || A(3)) { fprintf(stderr, "[gfx] shader class instances are unsupported\n"); set_shader(&x->cur_ps, NULL, K_PS); return 0; } set_shader(&x->cur_ps, (Obj *)A(1), K_PS); return 0; }
F(x_setlayout) { Obj *x = (Obj *)A(0); Obj *layout = (Obj *)A(1); if (layout && layout->kind != K_LAYOUT) { fprintf(stderr, "[gfx] refusing invalid input layout\n"); return 0; } if (x->cur_layout != layout) { if (layout) layout->refs++; release_obj(x->cur_layout); x->cur_layout = layout; } return 0; }
F(x_setvb) {
  Obj *x = (Obj *)A(0);
  uint32_t first = (uint32_t)A(1), count = (uint32_t)A(2);
  if (first > AOT_GFX_MAX_VERTEX_STREAMS ||
      count > AOT_GFX_MAX_VERTEX_STREAMS - first ||
      (count && (!A(3) || !A(4) || !A(5)))) {
    fprintf(stderr, "[gfx] vertex-buffer slots 0-3 are supported\n");
    x->draw_error = 1;
    return 0;
  }
  for (uint32_t i = 0; i < count; i++) {
    Obj *buffer = (Obj *)LDP(A(3) + i * PSZ);
    if (buffer && (buffer->kind != K_BUF ||
        !(LD(uint32_t, buffer->buffer_desc + 8) & 1u) ||
        !LD(uint32_t, A(4) + i * sizeof(uint32_t)))) {
      fprintf(stderr, "[gfx] refusing invalid vertex buffer at slot %u\n", first + i);
      x->draw_error = 1;
      return 0;
    }
  }
  for (uint32_t i = 0; i < count; i++) {
    Obj *buffer = (Obj *)LDP(A(3) + i * PSZ);
    if (buffer != x->cur_vb[first + i]) {
      if (buffer) buffer->refs++;
      release_obj(x->cur_vb[first + i]);
      x->cur_vb[first + i] = buffer;
    }
    x->stride[first + i] = LD(uint32_t, A(4) + i * sizeof(uint32_t));
    x->vertex_offset[first + i] = LD(uint32_t, A(5) + i * sizeof(uint32_t));
  }
  return 0;
}
F(x_settopology) { Obj *x = (Obj *)A(0); x->topology = (uint32_t)A(1); if (x->topology != 4) fprintf(stderr, "[gfx] only triangle-list topology is supported for draws\n"); return 0; }
static uint32_t vertex_format_size(uint32_t format) {
  switch (format) {
    case AOT_PRESENT_FORMAT_R32_FLOAT:
    case AOT_PRESENT_FORMAT_R32_UINT:
    case AOT_PRESENT_FORMAT_R8G8B8A8_UNORM: return 4;
    case AOT_PRESENT_FORMAT_R32G32_FLOAT:
    case AOT_PRESENT_FORMAT_R32G32_UINT: return 8;
    case AOT_PRESENT_FORMAT_R32G32B32_FLOAT: return 12;
    case AOT_PRESENT_FORMAT_R32G32B32A32_FLOAT: return 16;
    default: return 0;
  }
}
static int build_draw_snapshot(Obj *ctx, uint32_t element_count,
                               uint32_t first_vertex, int indexed,
                               uint32_t first_index, int32_t base_vertex) {
  if (ctx->draw_error || !ctx->cur_vs || !ctx->cur_ps || !ctx->cur_layout ||
      ctx->topology != 4 || !ctx->cur_layout->attribute_count)
    return -1;
  AOT_GFX_DRAW_CALL draw;
  memset(&draw, 0, sizeof draw);
  draw.vertex_spirv = ctx->cur_vs->spirv;
  draw.vertex_spirv_size = ctx->cur_vs->spirv_size;
  draw.pixel_spirv = ctx->cur_ps->spirv;
  draw.pixel_spirv_size = ctx->cur_ps->spirv_size;
  draw.attributes = ctx->cur_layout->attributes;
  draw.attribute_count = ctx->cur_layout->attribute_count;
  draw.vertex_count = element_count;
  draw.first_vertex = indexed ? 0 : first_vertex;
  draw.indexed = (uint8_t)indexed;
  draw.alpha_blend = ctx->cur_blend ? ctx->cur_blend->alpha_blend : 0;
  draw.has_viewport = ctx->has_viewport;
  draw.has_scissor = (uint8_t)(ctx->has_scissor && ctx->cur_rasterizer &&
      LD(uint32_t, ctx->cur_rasterizer->rasterizer_desc + 28));
  uint32_t d3d_cull_mode = ctx->cur_rasterizer ?
      LD(uint32_t, ctx->cur_rasterizer->rasterizer_desc + 4) : 1;
  draw.cull_mode = d3d_cull_mode == 2 ? 1 : d3d_cull_mode == 3 ? 2 : 0;
  draw.front_counter_clockwise = ctx->cur_rasterizer ?
      (uint8_t)LD(uint32_t, ctx->cur_rasterizer->rasterizer_desc + 8) : 0;
  draw.viewport = ctx->viewport;
  draw.scissor = ctx->scissor;
  uint32_t minimum_index = 0, maximum_index = 0;
  if (indexed) {
    if (!ctx->cur_ib || ctx->cur_ib->mapped ||
        (ctx->index_format != 57u && ctx->index_format != 42u))
      return -1;
    uint32_t index_size = ctx->index_format == 57u ? 2u : 4u;
    if ((size_t)element_count > SIZE_MAX / index_size ||
        (size_t)first_index > SIZE_MAX / index_size)
      return -1;
    size_t index_bytes = (size_t)element_count * index_size;
    size_t first_index_bytes = (size_t)first_index * index_size;
    if (first_index_bytes > SIZE_MAX - ctx->index_offset) return -1;
    size_t index_start = (size_t)ctx->index_offset + first_index_bytes;
    if (index_start > ctx->cur_ib->data_size ||
        index_bytes > ctx->cur_ib->data_size - index_start)
      return -1;
    const uint8_t *index_data = ctx->cur_ib->data + index_start;
    for (uint32_t i = 0; i < element_count; i++) {
      uint32_t value;
      if (index_size == 2) {
        uint16_t short_value;
        memcpy(&short_value, index_data + (size_t)i * index_size,
            sizeof short_value);
        value = short_value;
      } else {
        memcpy(&value, index_data + (size_t)i * index_size, sizeof value);
      }
      if (!i) {
        minimum_index = maximum_index = value;
      } else {
        if (value < minimum_index) minimum_index = value;
        if (value > maximum_index) maximum_index = value;
      }
      int64_t effective = (int64_t)value + base_vertex;
      if (effective < 0 || effective > UINT32_MAX) return -1;
    }
    draw.indices = index_data;
    draw.index_data_size = index_bytes;
    draw.index_format = ctx->index_format;
    draw.base_vertex = base_vertex;
  }
  uint8_t used_streams[AOT_GFX_MAX_VERTEX_STREAMS] = { 0 };
  for (uint32_t i = 0; i < draw.attribute_count; i++) {
    AOT_PRESENT_VERTEX_ATTRIBUTE *attribute = &ctx->cur_layout->attributes[i];
    uint32_t size = vertex_format_size(attribute->format);
    uint32_t slot = attribute->binding;
    if (slot >= AOT_GFX_MAX_VERTEX_STREAMS || !ctx->cur_vb[slot] ||
        ctx->cur_vb[slot]->mapped ||
        !ctx->stride[slot] || !size || attribute->offset > ctx->stride[slot] ||
        size > ctx->stride[slot] - attribute->offset)
      return -1;
    used_streams[slot] = 1;
  }
  for (uint32_t i = 0; i < AOT_GFX_MAX_VERTEX_STREAMS; i++) {
    if (!used_streams[i]) continue;
    Obj *buffer = ctx->cur_vb[i];
    if (!buffer) continue;
    uint64_t vertices_start = first_vertex;
    uint64_t vertices_end = (uint64_t)first_vertex + element_count;
    if (indexed) {
      vertices_start = (uint64_t)((int64_t)minimum_index + base_vertex);
      vertices_end = (uint64_t)((int64_t)maximum_index + base_vertex) + 1;
    }
    if (vertices_end > SIZE_MAX / ctx->stride[i] ||
        ctx->vertex_offset[i] > buffer->data_size ||
        vertices_start > vertices_end ||
        vertices_end * ctx->stride[i] > buffer->data_size - ctx->vertex_offset[i])
      return -1;
    draw.vertex_streams[i] = (AOT_GFX_VERTEX_STREAM){
      buffer->data, buffer->data_size, ctx->stride[i], ctx->vertex_offset[i]
    };
  }
  for (uint32_t i = 0; i < AOT_GFX_MAX_CONSTANT_BUFFERS; i++) {
    Obj *buffer = ctx->cur_vs_cb[i];
    if (buffer && buffer->mapped) return -1;
    if (buffer)
      draw.constants[i] = (AOT_GFX_CONSTANT_DATA){ buffer->data, buffer->data_size };
  }
  void *resources[3 + AOT_GFX_MAX_TEXTURES * 2];
  size_t resource_count = 0;
  resources[resource_count++] = ctx->cur_vs;
  resources[resource_count++] = ctx->cur_ps;
  if (indexed) resources[resource_count++] = ctx->cur_ib;
  for (uint32_t i = 0; i < AOT_GFX_MAX_TEXTURES; i++) {
    Obj *view = ctx->cur_srv[i], *sampler = ctx->cur_sampler[i];
    if ((view == NULL) != (sampler == NULL)) return -1;
    if (view) {
      if (!view->res || !view->res->backend_texture || !sampler->backend_sampler)
        return -1;
      draw.textures[i] = view->res->backend_texture;
      draw.samplers[i] = sampler->backend_sampler;
      resources[resource_count++] = view;
      resources[resource_count++] = sampler;
    }
  }
  return aot_gfx_draw_batch_append(ctx->draw_batch, &draw, resources,
      resource_count, retain_resource, release_resource);
}
F(x_draw) {
  Obj *x = (Obj *)A(0);
  if (!A(1)) return 0;
  if (A(1) > UINT32_MAX || A(2) > UINT32_MAX ||
      build_draw_snapshot(x, (uint32_t)A(1), (uint32_t)A(2), 0, 0, 0)) {
    fprintf(stderr, "[gfx] Draw rejected: invalid state, range, or draw-batch allocation\n");
    x->draw_error = 1;
  }
  return 0;
}
F(x_drawindexed) {
  Obj *x = (Obj *)A(0);
  if (!A(1)) return 0;
  if (A(1) > UINT32_MAX || A(2) > UINT32_MAX ||
      build_draw_snapshot(x, (uint32_t)A(1), 0, 1, (uint32_t)A(2),
          (int32_t)A(3))) {
    fprintf(stderr, "[gfx] DrawIndexed rejected: invalid state, range, or draw-batch allocation\n");
    x->draw_error = 1;
  }
  return 0;
}
static void clear_context_state(Obj *x) {
  if (!x) return;
  x->cur_rtv = NULL;
  x->draw_error = 0;
  x->topology = 0;
  x->has_viewport = x->has_scissor = 0;
  memset(&x->viewport, 0, sizeof x->viewport);
  memset(&x->scissor, 0, sizeof x->scissor);
  set_shader(&x->cur_vs, NULL, K_VS);
  set_shader(&x->cur_ps, NULL, K_PS);
  set_binding(&x->cur_layout, NULL, K_LAYOUT);
  release_obj(x->cur_ib);
  x->cur_ib = NULL;
  x->index_format = x->index_offset = 0;
  set_binding(&x->cur_blend, NULL, K_BLEND);
  set_binding(&x->cur_rasterizer, NULL, K_RASTERIZER);
  for (uint32_t i = 0; i < AOT_GFX_MAX_VERTEX_STREAMS; i++) {
    release_obj(x->cur_vb[i]);
    x->cur_vb[i] = NULL;
    x->stride[i] = x->vertex_offset[i] = 0;
  }
  for (uint32_t i = 0; i < AOT_GFX_MAX_CONSTANT_BUFFERS; i++)
    set_binding(&x->cur_vs_cb[i], NULL, K_BUF);
  for (uint32_t i = 0; i < AOT_GFX_MAX_TEXTURES; i++) {
    set_binding(&x->cur_srv[i], NULL, K_SRV);
    set_binding(&x->cur_sampler[i], NULL, K_SAMPLER);
  }
}
F(x_clearstate) {
  Obj *x = (Obj *)A(0);
  size_t queued = aot_gfx_draw_batch_count(x->draw_batch);
  if (queued)
    fprintf(stderr, "[gfx] ClearState discarded %zu unpresented draw calls\n", queued);
  aot_gfx_draw_batch_clear(x->draw_batch);
  clear_context_state(x);
  if (x->presented_frames)
    fprintf(stderr, "[gfx] frame stats: frames=%u draw_calls=%llu peak_draws_per_frame=%u descriptor_sets=%llu vertex_bytes=%llu index_bytes=%llu constant_bytes=%llu\n",
        x->presented_frames, (unsigned long long)x->presented_draws, x->peak_draws,
        (unsigned long long)x->uploaded_descriptor_sets,
        (unsigned long long)x->uploaded_vertex_bytes,
        (unsigned long long)x->uploaded_index_bytes,
        (unsigned long long)x->uploaded_constant_bytes);
  return 0;
}

/* ---- swapchain / resources ---- */
F(sc_present) {
  Obj *s = (Obj *)A(0), *ctx = s->dev ? s->dev->ctx : NULL;
  AOT_PRESENT_FRAME_STATS stats = { 0 };
  int result = 0;
  { static unsigned nfr; if (nfr == 0 || nfr == 60 || nfr % 600 == 0) fprintf(stderr, "[gfx] Present #%u (queued draws=%zu)\n", nfr, ctx ? aot_gfx_draw_batch_count(ctx->draw_batch) : (size_t)0); nfr++; }
  if (ctx) {
    size_t queued = aot_gfx_draw_batch_count(ctx->draw_batch);
    if (ctx->draw_error) result = -1;
    else result = aot_present_draw_batch(s->bb->clear, ctx->draw_batch, &stats);
    aot_gfx_draw_batch_clear(ctx->draw_batch);
    ctx->draw_error = 0;
    if (!result) {
      ctx->presented_frames++;
      ctx->presented_draws += stats.draw_calls;
      ctx->uploaded_vertex_bytes += stats.vertex_bytes;
      ctx->uploaded_index_bytes += stats.index_bytes;
      ctx->uploaded_constant_bytes += stats.constant_bytes;
      ctx->uploaded_descriptor_sets += stats.descriptor_sets;
      if (queued > ctx->peak_draws) ctx->peak_draws = (uint32_t)queued;
    }
  } else {
    aot_present_clear(s->bb->clear);
  }
  if (!result) s->frames++;
  return result ? E_FAIL : S_OK;
}
F(sc_getbuffer) { Obj *s = (Obj *)A(0); STP(A(3), s->bb); s->bb->refs++; return S_OK; }
F(sc_getdesc) { memcpy((void *)A(1), ((Obj *)A(0))->scdesc, SCD_SIZE); return S_OK; }
F(sc_resize) { Obj *s = (Obj *)A(0); uint32_t w = (uint32_t)A(2), h = (uint32_t)A(3); if (w) s->bb->tex[0] = w; if (h) s->bb->tex[1] = h; return S_OK; }
F(sc_getparent) { STP(A(2), get_factory()); return S_OK; }
F(sc_getfs) { if (A(1)) ST(uint32_t, A(1), 0); return S_OK; }
F(tex_getdesc) { memcpy((void *)A(1), ((Obj *)A(0))->tex, 44); return 0; }
F(res_gettype) { ST(uint32_t, A(1), 3); return 0; }    /* D3D11_RESOURCE_DIMENSION_TEXTURE2D */
F(buf_gettype) { ST(uint32_t, A(1), 1); return 0; }     /* D3D11_RESOURCE_DIMENSION_BUFFER */
F(buf_getdesc) { memcpy((void *)A(1), ((Obj *)A(0))->buffer_desc, 24); return 0; }
F(blend_getdesc) { memcpy((void *)A(1), ((Obj *)A(0))->blend_desc, 264); return 0; }
F(rasterizer_getdesc) { memcpy((void *)A(1), ((Obj *)A(0))->rasterizer_desc, 40); return 0; }
F(rtv_getresource) { Obj *v = (Obj *)A(0); STP(A(1), v->res); v->res->refs++; return 0; }

static uint64_t f_createsc(CPU *c);
F(d_createvs);
F(d_createps);
F(rasterizer_getdesc);
static void **vt_dev, **vt_d10dev, **vt_ctx, **vt_dxdev, **vt_adp, **vt_out, **vt_fac, **vt_sc, **vt_tex, **vt_rtv, **vt_shader, **vt_buf, **vt_layout, **vt_srv, **vt_sampler, **vt_blend, **vt_rasterizer;
#define UNK { 0, u_qi, "QueryInterface" }, { 1, u_addref, "AddRef" }, { 2, u_release, "Release" }

#define D10_CONTEXT_FORWARD(name) \
  F(d10_##name) { \
    Obj *device = (Obj *)A(0); \
    if (!device || device->kind != K_DEV || !device->ctx) return E_INVALIDARG; \
    CPU copy = *c; \
    copy.r[1] = (uint64_t)device->ctx; \
    return name(&copy); \
  }
D10_CONTEXT_FORWARD(x_setvscbs)
D10_CONTEXT_FORWARD(x_setsrv)
D10_CONTEXT_FORWARD(x_setps)
D10_CONTEXT_FORWARD(x_setsampler)
D10_CONTEXT_FORWARD(x_setvs)
D10_CONTEXT_FORWARD(x_drawindexed)
D10_CONTEXT_FORWARD(x_draw)
D10_CONTEXT_FORWARD(x_setlayout)
D10_CONTEXT_FORWARD(x_setvb)
D10_CONTEXT_FORWARD(x_setindexbuffer)
D10_CONTEXT_FORWARD(x_settopology)
D10_CONTEXT_FORWARD(x_setrts)
D10_CONTEXT_FORWARD(x_setblendstate)
D10_CONTEXT_FORWARD(x_setrasterizerstate)
D10_CONTEXT_FORWARD(x_setviewports)
D10_CONTEXT_FORWARD(x_setscissors)
D10_CONTEXT_FORWARD(x_updatesubresource)
D10_CONTEXT_FORWARD(x_clearrtv)
D10_CONTEXT_FORWARD(x_clearstate)
D10_CONTEXT_FORWARD(x_nop)
#undef D10_CONTEXT_FORWARD

F(d10_createbuffer) {
  if (!A(1)) return E_INVALIDARG;
  uint8_t desc[24] = { 0 };
  memcpy(desc, (const void *)A(1), 20);
  CPU copy = *c;
  copy.r[2] = (uint64_t)(uintptr_t)desc;
  return d_createbuffer(&copy);
}
F(d10_createblendstate) {
  if (!A(1)) return E_INVALIDARG;
  uint8_t desc[264] = { 0 };
  memcpy(desc + 8, (const void *)(uintptr_t)(A(1) + 4), 32);
  CPU copy = *c;
  copy.r[2] = (uint64_t)(uintptr_t)desc;
  return d_createblendstate(&copy);
}
F(d10_createvs) {
  CPU copy = *c;
  copy.r[9] = 0;
  ST(uint64_t, copy.r[4] + 8 + 8 * 4, A(3));
  return d_createvs(&copy);
}
F(d10_createps) {
  CPU copy = *c;
  copy.r[9] = 0;
  ST(uint64_t, copy.r[4] + 8 + 8 * 4, A(3));
  return d_createps(&copy);
}

static void init_vts(void) {
  if (vt_dev) return;
  { Slot s[] = { UNK, { 3, d_createbuffer, "CreateBuffer" }, { 5, d_createtex2d, "CreateTexture2D" }, { 7, d_createsrv, "CreateShaderResourceView" }, { 9, d_creatertv, "CreateRenderTargetView" }, { 11, d_createinputlayout, "CreateInputLayout" }, { 12, d_createvs, "CreateVertexShader" }, { 15, d_createps, "CreatePixelShader" }, { 20, d_createblendstate, "CreateBlendState" }, { 22, d_createrasterizerstate, "CreateRasterizerState" }, { 23, d_createsampler, "CreateSamplerState" }, { 29, d_checkformat, "CheckFormatSupport" }, { 30, d_checkmsaa, "CheckMultisampleQualityLevels" },
      { 37, d_featurelevel, "GetFeatureLevel" }, { 38, d_zero, "GetCreationFlags" }, { 39, d_zero, "GetDeviceRemovedReason" }, { 40, d_getimmctx, "GetImmediateContext" }, { 41, u_ok, "SetExceptionMode" }, { 0, 0, 0 } };
    vt_dev = build("ID3D11Device", 43, s); }
  { Slot s[] = { UNK,
      { 3, d10_x_setvscbs, "VSSetConstantBuffers" },
      { 4, d10_x_setsrv, "PSSetShaderResources" },
      { 5, d10_x_setps, "PSSetShader" },
      { 6, d10_x_setsampler, "PSSetSamplers" },
      { 7, d10_x_setvs, "VSSetShader" },
      { 8, d10_x_drawindexed, "DrawIndexed" },
      { 9, d10_x_draw, "Draw" },
      { 11, d10_x_setlayout, "IASetInputLayout" },
      { 12, d10_x_setvb, "IASetVertexBuffers" },
      { 13, d10_x_setindexbuffer, "IASetIndexBuffer" },
      { 18, d10_x_settopology, "IASetPrimitiveTopology" },
      { 24, d10_x_setrts, "OMSetRenderTargets" },
      { 25, d10_x_setblendstate, "OMSetBlendState" },
      { 29, d10_x_setrasterizerstate, "RSSetState" },
      { 30, d10_x_setviewports, "RSSetViewports" },
      { 31, d10_x_setscissors, "RSSetScissorRects" },
      { 34, d10_x_updatesubresource, "UpdateSubresource" },
      { 35, d10_x_clearrtv, "ClearRenderTargetView" },
      { 63, d_zero, "GetDeviceRemovedReason" },
      { 69, d10_x_clearstate, "ClearState" },
      { 70, d10_x_nop, "Flush" },
      { 71, d10_createbuffer, "CreateBuffer" },
      { 73, d_createtex2d, "CreateTexture2D" },
      { 75, d_createsrv, "CreateShaderResourceView" },
      { 76, d_creatertv, "CreateRenderTargetView" },
      { 78, d_createinputlayout, "CreateInputLayout" },
      { 79, d10_createvs, "CreateVertexShader" },
      { 82, d10_createps, "CreatePixelShader" },
      { 83, d10_createblendstate, "CreateBlendState" },
      { 85, d_createrasterizerstate, "CreateRasterizerState" },
      { 86, d_createsampler, "CreateSamplerState" },
      { 90, d_checkformat, "CheckFormatSupport" },
      { 91, d_checkmsaa, "CheckMultisampleQualityLevels" },
      { 94, d_zero, "GetCreationFlags" },
      { 0, 0, 0 } };
    vt_d10dev = build("ID3D10Device", 98, s);  /* slots follow d3d10.h (CreateBuffer=71, CreateRenderTargetView=76, ...) */ }
  { Slot s[] = { UNK, { 3, c_getdevice, "GetDevice" }, { 7, x_setvscbs, "VSSetConstantBuffers" }, { 8, x_setsrv, "PSSetShaderResources" }, { 9, x_setps, "PSSetShader" }, { 10, x_setsampler, "PSSetSamplers" }, { 11, x_setvs, "VSSetShader" }, { 12, x_drawindexed, "DrawIndexed" }, { 13, x_draw, "Draw" }, { 14, x_map, "Map" }, { 15, x_unmap, "Unmap" }, { 17, x_setlayout, "IASetInputLayout" }, { 18, x_setvb, "IASetVertexBuffers" }, { 19, x_setindexbuffer, "IASetIndexBuffer" }, { 24, x_settopology, "IASetPrimitiveTopology" }, { 33, x_setrts, "OMSetRenderTargets" }, { 35, x_setblendstate, "OMSetBlendState" }, { 43, x_setrasterizerstate, "RSSetState" }, { 44, x_setviewports, "RSSetViewports" }, { 45, x_setscissors, "RSSetScissorRects" },
      { 48, x_updatesubresource, "UpdateSubresource" }, { 50, x_clearrtv, "ClearRenderTargetView" }, { 110, x_clearstate, "ClearState" }, { 111, x_nop, "Flush" }, { 0, 0, 0 } };
    vt_ctx = build("ID3D11DeviceContext", 128, s); }
  { Slot s[] = { UNK, { 3, u_ok, "SetPrivateData" }, { 6, dxdev_getparent, "GetParent" }, { 7, dxdev_getadapter, "GetAdapter" }, { 10, u_ok, "SetGPUThreadPriority" }, { 11, u_ok, "GetGPUThreadPriority" }, { 0, 0, 0 } };
    vt_dxdev = build("IDXGIDevice", 12, s); }
  { Slot s[] = { UNK, { 6, adapter_getparent, "GetParent" }, { 7, adapter_enumoutputs, "EnumOutputs" }, { 8, adapter_getdesc, "GetDesc" }, { 9, u_ok, "CheckInterfaceSupport" }, { 0, 0, 0 } };
    vt_adp = build("IDXGIAdapter", 10, s); }
  { Slot s[] = { UNK, { 3, u_ok, "SetPrivateData" }, { 6, adapter_getparent, "GetParent" }, { 7, out_getdesc, "GetDesc" }, { 8, out_getmodes, "GetDisplayModeList" }, { 9, out_findclosest, "FindClosestMatchingMode" }, { 10, u_ok, "WaitForVBlank" }, { 11, u_ok, "TakeOwnership" }, { 12, u_ok, "ReleaseOwnership" }, { 13, out_unsupported, "GetGammaControlCapabilities" }, { 14, out_unsupported, "SetGammaControl" }, { 15, out_unsupported, "GetGammaControl" }, { 16, out_unsupported, "SetDisplaySurface" }, { 17, out_unsupported, "GetDisplaySurfaceData" }, { 18, out_unsupported, "GetFrameStatistics" }, { 0, 0, 0 } };
    vt_out = build("IDXGIOutput", 19, s); }
  { Slot s[] = { UNK, { 7, f_enumadapters, "EnumAdapters" }, { 8, f_makeassoc, "MakeWindowAssociation" }, { 10, f_createsc, "CreateSwapChain" }, { 12, f_enumadapters, "EnumAdapters1" }, { 13, f_iscurrent, "IsCurrent" }, { 0, 0, 0 } };
    vt_fac = build("IDXGIFactory1", 14, s); }
  { Slot s[] = { UNK, { 6, sc_getparent, "GetParent" }, { 7, sc_getdevice, "GetDevice" }, { 8, sc_present, "Present" }, { 9, sc_getbuffer, "GetBuffer" }, { 10, u_ok, "SetFullscreenState" }, { 11, sc_getfs, "GetFullscreenState" },
      { 12, sc_getdesc, "GetDesc" }, { 13, sc_resize, "ResizeBuffers" }, { 14, u_ok, "ResizeTarget" }, { 0, 0, 0 } };
    vt_sc = build("IDXGISwapChain", 18, s); }
  { Slot s[] = { UNK, { 3, c_getdevice, "GetDevice" }, { 7, res_gettype, "GetType" }, { 8, u_ok, "SetEvictionPriority" }, { 9, d_zero, "GetEvictionPriority" }, { 10, tex_getdesc, "GetDesc" }, { 0, 0, 0 } };
    vt_tex = build("ID3D11Texture2D", 11, s); }
  { Slot s[] = { UNK, { 3, c_getdevice, "GetDevice" }, { 7, rtv_getresource, "GetResource" }, { 0, 0, 0 } };
    vt_rtv = build("ID3D11RenderTargetView", 9, s); }
  { Slot s[] = { UNK, { 3, c_getdevice, "GetDevice" }, { 0, 0, 0 } };
    vt_shader = build("ID3D11Shader", 7, s); }
  { Slot s[] = { UNK, { 3, c_getdevice, "GetDevice" }, { 7, buf_gettype, "GetType" }, { 10, buf_getdesc, "GetDesc" }, { 0, 0, 0 } };
    vt_buf = build("ID3D11Buffer", 11, s); }
  { Slot s[] = { UNK, { 3, c_getdevice, "GetDevice" }, { 0, 0, 0 } };
    vt_layout = build("ID3D11InputLayout", 7, s); }
  { Slot s[] = { UNK, { 3, c_getdevice, "GetDevice" }, { 0, 0, 0 } };
    vt_srv = build("ID3D11ShaderResourceView", 9, s); }
  { Slot s[] = { UNK, { 3, c_getdevice, "GetDevice" }, { 0, 0, 0 } };
    vt_sampler = build("ID3D11SamplerState", 7, s); }
  { Slot s[] = { UNK, { 3, c_getdevice, "GetDevice" }, { 7, blend_getdesc, "GetDesc" }, { 0, 0, 0 } };
    vt_blend = build("ID3D11BlendState", 8, s); }
  { Slot s[] = { UNK, { 3, c_getdevice, "GetDevice" }, { 7, rasterizer_getdesc, "GetDesc" }, { 0, 0, 0 } };
    vt_rasterizer = build("ID3D11RasterizerState", 8, s); }
}
static void *vt_for(Kind k) { switch (k) { case K_DEV: return vt_dev; case K_CTX: return vt_ctx; case K_DXDEV: return vt_dxdev; case K_ADAPTER: return vt_adp; case K_OUTPUT: return vt_out; case K_FACTORY: return vt_fac; case K_SC: return vt_sc; case K_TEX: return vt_tex; case K_VS: case K_PS: return vt_shader; case K_BUF: return vt_buf; case K_LAYOUT: return vt_layout; case K_SRV: return vt_srv; case K_SAMPLER: return vt_sampler; case K_BLEND: return vt_blend; case K_RASTERIZER: return vt_rasterizer; default: return vt_rtv; } }
static void bind(Obj *o) { o->vt = vt_for(o->kind); }
static Obj *mkv(Kind k, Obj *dev) { init_vts(); Obj *o = mk(k, dev); bind(o); return o; }
static int spirv_entry_matches(const uint32_t *words, size_t byte_count, uint32_t expected_stage) {
  if (!words || byte_count % sizeof(uint32_t) || byte_count < 5 * sizeof(uint32_t) || words[0] != 0x07230203u) return 0;
  size_t word_count = byte_count / sizeof(uint32_t), offset = 5;
  uint32_t entries = 0;
  while (offset < word_count) {
    uint32_t instruction = words[offset], count = instruction >> 16, opcode = instruction & 0xffffu;
    if (!count || count > word_count - offset) return 0;
    if (opcode == 15u) {
      if (count < 4 || words[offset + 1] != expected_stage) return 0;
      entries++;
    }
    offset += count;
  }
  return entries == 1;
}
static uint64_t create_shader(CPU *c, Kind kind, uint32_t stage, const char *name) {
  { static int n; if (n++ < 3) fprintf(stderr, "[gfx] %s called (bytecode %u bytes)\n", name, (unsigned)A(2)); }
  if (A(4)) STP(A(4), 0);
  if (!A(4) || !A(1) || !A(2)) return E_INVALIDARG;
  if (A(3)) return E_NOTIMPL;
  uint32_t *spirv = NULL;
  size_t spirv_size = 0;
  int result = aot_dxbc_to_spirv((const void *)A(1), (size_t)A(2), &spirv, &spirv_size);
  if (result != AOT_DXBC_OK) {
    fprintf(stderr, "[gfx] %s: DXBC translation failed (%d)\n", name, result);
    return result == AOT_DXBC_UNAVAILABLE ? E_NOTIMPL : E_FAIL;
  }
  if (!spirv_entry_matches(spirv, spirv_size, stage)) {
    fprintf(stderr, "[gfx] %s: translated shader has invalid or mismatched SPIR-V entry point\n", name);
    aot_dxbc_free_spirv(spirv);
    return E_FAIL;
  }
  Obj *shader = mkv(kind, (Obj *)A(0));
  if (!shader) { aot_dxbc_free_spirv(spirv); return E_FAIL; }
  shader->spirv = spirv;
  shader->spirv_size = spirv_size;
  STP(A(4), shader);
  return S_OK;
}
F(d_createvs) { return create_shader(c, K_VS, 0u, "CreateVertexShader"); }
F(d_createps) { return create_shader(c, K_PS, 4u, "CreatePixelShader"); }
static Obj *make_device(Obj **ctx_out) {
  init_vts(); Obj *d = mkv(K_DEV, NULL); Obj *x = mkv(K_CTX, d);
  if (!d || !x) { release_obj(d); release_obj(x); return NULL; }
  x->draw_batch = aot_gfx_draw_batch_create();
  if (!x->draw_batch) { release_obj(x); release_obj(d); return NULL; }
  d->ctx = x; if (ctx_out) *ctx_out = x;
  if (!g_pinit) { if (aot_present_init() != 0) fprintf(stderr, "[gfx] presentation backend failed to start\n"); g_pinit = 1; aot_present_size(&g_w, &g_h); }
  return d;
}
static Obj *make_swapchain(Obj *dev, const uint8_t *desc) {
  if (!dev || !desc) return NULL;
  if (dev->kind == K_DXDEV) dev = dev->dev;
  Obj *s = mkv(K_SC, dev);
  if (!s) return NULL;
  dev->refs++;
  memcpy(s->scdesc, desc, SCD_SIZE);
  uint32_t w = LD(uint32_t, desc), h = LD(uint32_t, desc + 4), fmt = LD(uint32_t, desc + 16); if (!w) w = g_w; if (!h) h = g_h; ST(uint32_t, s->scdesc, w); ST(uint32_t, s->scdesc + 4, h);
  Obj *bb = mkv(K_TEX, dev);
  if (!bb) {
    release_obj(s);
    return NULL;
  }
  bb->tex[0] = w; bb->tex[1] = h; bb->tex[2] = 1; bb->tex[3] = 1; bb->tex[4] = fmt; bb->tex[5] = 1; bb->tex[8] = 0x20; s->bb = bb; return s;
}
F(f_createsc) { Obj *s = make_swapchain((Obj *)A(1), (const uint8_t *)A(2)); STP(A(3), s); return S_OK; }

uint64_t aot_win_gfx_d3d10_create(uint64_t device_output,
                                  uint64_t swapchain_output,
                                  uint64_t swapchain_desc) {
  if (!device_output || (swapchain_output && !swapchain_desc))
    return E_INVALIDARG;
  Obj *device = make_device(NULL);
  if (!device) return E_FAIL;
  device->vt = vt_d10dev;
  Obj *swapchain = NULL;
  if (swapchain_output) {
    swapchain = make_swapchain(device, (const uint8_t *)swapchain_desc);
    if (!swapchain) {
      release_obj(device);
      return E_FAIL;
    }
    STP(swapchain_output, swapchain);
  }
  STP(device_output, device);
  return S_OK;
}

/* ---- exported entry points ---- */
F(sh_D3D11CreateDevice) {   /* (adapter, type, sw, flags, pFL, nFL, sdk, ppDevice, pFeatureLevel, ppContext) */
  Obj *x; Obj *d = make_device(&x);
  if (!d) return E_FAIL;
  if (A(7)) STP(A(7), d); if (A(8)) ST(uint32_t, A(8), 0xB000); if (A(9)) { STP(A(9), x); x->refs++; } return S_OK;
}
F(sh_D3D11CreateDeviceAndSwapChain) {   /* (adapter,type,sw,flags,pFL,nFL,sdk,pSCDesc,ppSC,ppDevice,pFeatureLevel,ppContext) */
  Obj *x; Obj *d = make_device(&x);
  if (!d) return E_FAIL;
  if (A(8) && A(7)) STP(A(8), make_swapchain(d, (const uint8_t *)A(7)));
  if (A(9)) STP(A(9), d); if (A(10)) ST(uint32_t, A(10), 0xB000); if (A(11)) { STP(A(11), x); x->refs++; } return S_OK;
}
F(sh_CreateDXGIFactory) { init_vts(); Obj *f = get_factory(); bind(f); STP(A(1), f); return S_OK; }

/* ---- user32 / winmm: just enough for a game to open a "window" and run a message loop ---- */
#define HWND_FAKE 0x10001
F(u_one) { return 1; }
F(u_zero) { return 0; }
F(u_hwnd) { return HWND_FAKE; }
void aot_win_client_size(uint32_t *pw, uint32_t *ph) { uint32_t w = g_w ? g_w : 1280, h = g_h ? g_h : 720; if (!g_pinit) aot_present_size(&w, &h); *pw = w; *ph = h; }
F(u_getclientrect) { uint32_t w = g_w ? g_w : 1280, h = g_h ? g_h : 720; if (!g_pinit) aot_present_size(&w, &h); uint8_t *r = (uint8_t *)A(1); ST(int32_t, r, 0); ST(int32_t, r + 4, 0); ST(int32_t, r + 8, w); ST(int32_t, r + 12, h); return 1; }
F(u_sysmetrics) { uint32_t w, h; if (g_pinit) { w = g_w; h = g_h; } else aot_present_size(&w, &h); int i = (int)A(0); return i == 0 || i == 16 ? w : i == 1 || i == 17 ? h : 0; }
F(u_msgbox) { fflush(stdout); fprintf(stderr, "[MessageBox] %s\n", (const char *)A(1)); return 1; }
F(u_getcursor) { memset((void *)A(0), 0, 8); return 1; }
F(u_tickcount) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
const ShimEnt aot_shim_table_gfx[] = {
  { "D3D11CreateDevice", sh_D3D11CreateDevice }, { "D3D11CreateDeviceAndSwapChain", sh_D3D11CreateDeviceAndSwapChain },
  { "CreateDXGIFactory", sh_CreateDXGIFactory }, { "CreateDXGIFactory1", sh_CreateDXGIFactory },
  { "RegisterClassExW", u_one }, { "RegisterClassExA", u_one }, { "RegisterClassW", u_one }, { "RegisterClassA", u_one }, { "UnregisterClassW", u_one }, { "UnregisterClassA", u_one },
  { "CreateWindowExW", u_hwnd }, { "CreateWindowExA", u_hwnd }, { "ShowWindow", u_zero }, { "UpdateWindow", u_one }, { "SetForegroundWindow", u_one }, { "SetFocus", u_hwnd }, { "DestroyWindow", u_one },
  { "GetClientRect", u_getclientrect }, { "GetWindowRect", u_getclientrect }, { "AdjustWindowRect", u_one }, { "AdjustWindowRectEx", u_one }, { "SetWindowPos", u_one }, { "MoveWindow", u_one },
  { "PeekMessageW", u_zero }, { "PeekMessageA", u_zero }, { "GetMessageW", u_zero }, { "GetMessageA", u_zero }, { "TranslateMessage", u_zero }, { "DispatchMessageW", u_zero }, { "DispatchMessageA", u_zero },
  { "PostQuitMessage", u_zero }, { "DefWindowProcW", u_zero }, { "DefWindowProcA", u_zero }, { "LoadCursorW", u_one }, { "LoadCursorA", u_one }, { "LoadIconW", u_one }, { "LoadIconA", u_one },
  { "GetSystemMetrics", u_sysmetrics }, { "SetWindowTextW", u_one }, { "SetWindowTextA", u_one }, { "ShowCursor", u_zero }, { "ClipCursor", u_one }, { "SetCursorPos", u_one }, { "GetCursorPos", u_getcursor },
  { "GetAsyncKeyState", u_zero }, { "GetKeyState", u_zero }, { "GetForegroundWindow", u_hwnd }, { "GetActiveWindow", u_hwnd }, { "IsWindow", u_one }, { "IsIconic", u_zero }, { "GetDC", u_one }, { "ReleaseDC", u_one },
  { "MessageBoxA", u_msgbox }, { "MessageBoxW", u_msgbox }, { "timeGetTime", u_tickcount }, { "timeBeginPeriod", u_zero }, { "timeEndPeriod", u_zero }, { "SetProcessDPIAware", u_one },
  { 0, 0 }
};
int aot_win_gfx_is_placeholder(shim_fn fn) {
  return fn == u_zero || fn == u_one;
}
