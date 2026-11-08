#include "shims.h"

#define ARG(n) ((n) == 0 ? c->r[1] : (n) == 1 ? c->r[2] : \
    (n) == 2 ? c->r[8] : (n) == 3 ? c->r[9] : \
    LD(uint64_t, c->r[4] + 8 + 8 * (n)))

static uint64_t d3d10_create_device(CPU *c) {
  return aot_win_gfx_d3d10_create(ARG(5), 0, 0);
}

static uint64_t d3d10_create_device_and_swapchain(CPU *c) {
  return aot_win_gfx_d3d10_create(ARG(7), ARG(6), ARG(5));
}

static void clear_guest_pointer(uint64_t address) {
  if (!address) return;
  if (aot_is_pe32) ST(uint32_t, address, 0);
  else ST(uint64_t, address, 0);
}

/* ---- ID3D10Blob (guest-visible COM object, memory below 4 GB for PE32 guests) ---- */
#include "hlsl_compile.h"
#define PSZ (aot_is_pe32 ? 4 : 8)
#define A(n) ARG(n)
void aot_thunk_set_pop(uint64_t addr, int pop);
typedef struct { void **vt; uint32_t refs; void *data; size_t size; } Blob;
static const uint8_t IID_BLOB[16] = { 0x08, 0xfb, 0xa5, 0x8b, 0x95, 0x51, 0xe2, 0x40, 0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02 };
static const uint8_t IID_UNK[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46 };
static void put_ptr(uint64_t a, uint64_t v) { if (aot_is_pe32) ST(uint32_t, a, (uint32_t)v); else ST(uint64_t, a, v); }
static uint64_t blob_qi(CPU *c) {
  Blob *b = (Blob *)(uintptr_t)A(0); const uint8_t *id = (const uint8_t *)(uintptr_t)A(1); uint64_t out = A(2);
  if (!out) return 0x80004003u;   /* E_POINTER */
  if (id && (!memcmp(id, IID_BLOB, 16) || !memcmp(id, IID_UNK, 16))) { b->refs++; put_ptr(out, (uint64_t)(uintptr_t)b); return 0; }
  put_ptr(out, 0); return 0x80004002u;   /* E_NOINTERFACE */
}
static uint64_t blob_addref(CPU *c) { return ++((Blob *)(uintptr_t)A(0))->refs; }
static uint64_t blob_release(CPU *c) { Blob *b = (Blob *)(uintptr_t)A(0); uint32_t r = --b->refs; if (!r) { aot_gfree(b->data); aot_gfree(b); } return r; }
static uint64_t blob_ptr(CPU *c) { return (uint64_t)(uintptr_t)((Blob *)(uintptr_t)A(0))->data; }
static uint64_t blob_size(CPU *c) { return ((Blob *)(uintptr_t)A(0))->size; }
static void **blob_vtable(void) {
  static void **vt; if (vt) return vt;
  static const struct { const char *n; shim_fn f; int argc; } m[5] = { { "QueryInterface", blob_qi, 3 }, { "AddRef", blob_addref, 1 }, { "Release", blob_release, 1 }, { "GetBufferPointer", blob_ptr, 1 }, { "GetBufferSize", blob_size, 1 } };
  void **v = aot_gcalloc(5, PSZ); char nm[48];
  for (int i = 0; i < 5; i++) { snprintf(nm, sizeof nm, "ID3D10Blob::%s", m[i].n); uint64_t t = aot_register_thunk((aot_host_fn)m[i].f, nm); if (aot_is_pe32) aot_thunk_set_pop(t, 4 * m[i].argc); if (aot_is_pe32) ((uint32_t *)v)[i] = (uint32_t)t; else v[i] = (void *)(uintptr_t)t; }
  return vt = v;
}
static uint64_t make_blob(const void *data, size_t n) {
  Blob *b = aot_gcalloc(1, sizeof *b); void *d = aot_gmalloc(n ? n : 1); if (!b || !d) return 0;
  memcpy(d, data, n); b->vt = blob_vtable(); b->refs = 1; b->data = d; b->size = n; return (uint64_t)(uintptr_t)b;
}

static uint64_t d3d10_compile_shader(CPU *c) {
  /* (pSrcData, SrcDataLen, pFileName, pDefines, pInclude, pFunctionName, pProfile, Flags, ppShader, ppErrorMsgs) */
  clear_guest_pointer(ARG(8)); clear_guest_pointer(ARG(9));
  const char *src = (const char *)(uintptr_t)ARG(0); size_t len = (size_t)(uint32_t)ARG(1);
  const char *entry = (const char *)(uintptr_t)ARG(5), *prof = (const char *)(uintptr_t)ARG(6);
  AotHlslMacro mac[64]; size_t nm = 0;
  for (uint64_t d = ARG(3); d && nm < 63; d += 2 * PSZ) {
    uint64_t np = aot_is_pe32 ? LD(uint32_t, d) : LD(uint64_t, d), vp = aot_is_pe32 ? LD(uint32_t, d + 4) : LD(uint64_t, d + 8);
    if (!np) break; mac[nm].name = (const char *)(uintptr_t)np; mac[nm].value = vp ? (const char *)(uintptr_t)vp : "1"; nm++;
  }
  void *code; size_t size; char *log;
  int rc = aot_hlsl_compile(src, len, (const char *)(uintptr_t)ARG(2), mac, nm, entry, prof, (unsigned)ARG(7), &code, &size, &log);
  if (log && *log) { fflush(stdout); fprintf(stderr, "[hlsl] %s(%s): %s%s", entry ? entry : "?", prof ? prof : "?", log, log[strlen(log) - 1] == '\n' ? "" : "\n"); if (ARG(9)) { uint64_t e = make_blob(log, strlen(log) + 1); if (e) put_ptr(ARG(9), e); } }
  free(log);
  if (rc) { fprintf(stderr, "[hlsl] D3D10CompileShader failed (%d) for %s/%s\n", rc, entry ? entry : "?", prof ? prof : "?"); return rc == -2 ? 0x80004001u : 0x80004005u; }
  uint64_t b = make_blob(code, size); free(code);
  if (!b) return 0x8007000Eu;
  if (ARG(8)) put_ptr(ARG(8), b);
  return 0;
}

static uint64_t d3d10_reflect_shader(CPU *c) {
  clear_guest_pointer(ARG(2));
  fprintf(stderr, "[aot] D3D10ReflectShader is unavailable: no shader reflection backend\n");
  return 0x80004001u; /* E_NOTIMPL */
}

const ShimEnt aot_shim_table_d3d10[] = {
  { "D3D10CreateDevice", d3d10_create_device },
  { "D3D10CreateDeviceAndSwapChain", d3d10_create_device_and_swapchain },
  { "D3D10CompileShader", d3d10_compile_shader },
  { "D3D10ReflectShader", d3d10_reflect_shader },
  { 0, 0 }
};

#undef ARG
