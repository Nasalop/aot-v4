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

/* ---- ID3D10ShaderReflection: parsed directly from the DXBC container (RDEF / ISGN / OSGN / SHDR / STAT chunks) ---- */
typedef struct Refl Refl;
typedef struct { void **vt; uint32_t refs; Refl *r; uint32_t a, b; } RObj;   /* generic sub-object: a/b select the entry */
struct Refl { void **vt; uint32_t refs; uint8_t *d; size_t n; const uint8_t *rdef, *isgn, *osgn, *shdr, *stat; uint32_t rdef_n; RObj *cb[64]; RObj *var[64][128]; RObj *ty[256]; uint32_t tyoff[256]; int nty; };
static void **robj_vt(const char *iface, int n, const shim_fn *f, const int *argc, void ***slot) {
  if (*slot) return *slot;
  void **v = aot_gcalloc(n, PSZ); char nm[64];
  for (int i = 0; i < n; i++) { snprintf(nm, sizeof nm, "%s::%d", iface, i); uint64_t t = aot_register_thunk((aot_host_fn)f[i], nm); if (aot_is_pe32) { aot_thunk_set_pop(t, 4 * argc[i]); ((uint32_t *)v)[i] = (uint32_t)t; } else v[i] = (void *)(uintptr_t)t; }
  return *slot = v;
}
#define U32(p) (*(const uint32_t *)(p))
#define U16(p) (*(const uint16_t *)(p))
static uint64_t r_ok(CPU *c) { return 0; }
static uint64_t r_addref(CPU *c) { return ++((RObj *)(uintptr_t)A(0))->refs; }
static uint64_t r_release_sub(CPU *c) { RObj *o = (RObj *)(uintptr_t)A(0); return o->refs ? o->refs : 1; }   /* sub-objects live as long as the reflection object */
static uint64_t r_qi(CPU *c) { if (A(2)) put_ptr(A(2), 0); return 0x80004002u; }
static const uint8_t *rp(Refl *r, uint32_t off) { return r->rdef + off; }
static const char *rs(Refl *r, uint32_t off) { return (const char *)r->rdef + off; }
static int rdef_major(Refl *r) { return (U32(r->rdef + 16) >> 8) & 0xff; }   /* version dword: minor | major<<8 | program<<16 (byte order of the DXBC) */
static int rdef_varsz(Refl *r) { return rdef_major(r) >= 5 ? 40 : 24; }
/* --- types --- */
static void **g_vt_type; static void **g_vt_var; static void **g_vt_cb; static void **g_vt_refl;
static uint64_t t_getdesc(CPU *c) {
  RObj *o = (RObj *)(uintptr_t)A(0); Refl *r = o->r; const uint8_t *t = rp(r, o->a); uint8_t *d = (uint8_t *)(uintptr_t)A(1);
  ST(uint32_t, d + 0, U16(t)); ST(uint32_t, d + 4, U16(t + 2)); ST(uint32_t, d + 8, U16(t + 4)); ST(uint32_t, d + 12, U16(t + 6)); ST(uint32_t, d + 16, U16(t + 8)); ST(uint32_t, d + 20, U16(t + 10)); ST(uint32_t, d + 24, o->b); return 0;
}
static RObj *mk_type(Refl *r, uint32_t off, uint32_t memoff) {
  for (int i = 0; i < r->nty; i++) if (r->tyoff[i] == off && r->ty[i]->b == memoff) return r->ty[i];
  if (r->nty >= 256) return NULL;
    extern uint64_t t_member_idx(CPU *), t_member_name(CPU *), t_member_tname(CPU *);
  static const shim_fn fn[4] = { t_getdesc, t_member_idx, t_member_name, t_member_tname }; static const int ac[4] = { 2, 2, 2, 2 };
  RObj *o = aot_gcalloc(1, sizeof *o); if (!o) return NULL; o->vt = robj_vt("ID3D10ShaderReflectionType", 4, fn, ac, &g_vt_type); o->refs = 1; o->r = r; o->a = off; o->b = memoff;
  r->tyoff[r->nty] = off; r->ty[r->nty++] = o; return o;
}
uint64_t t_member_idx(CPU *c) {
  RObj *o = (RObj *)(uintptr_t)A(0); Refl *r = o->r; const uint8_t *t = rp(r, o->a); uint32_t i = (uint32_t)A(1);
  if (i >= U16(t + 10)) return 0; const uint8_t *m = rp(r, U32(t + 12)) + 12 * i; return (uint64_t)(uintptr_t)mk_type(r, U32(m + 4), U32(m + 8));
}
uint64_t t_member_name(CPU *c) {
  RObj *o = (RObj *)(uintptr_t)A(0); Refl *r = o->r; const uint8_t *t = rp(r, o->a); const char *nm = (const char *)(uintptr_t)A(1);
  for (uint32_t i = 0; i < U16(t + 10); i++) { const uint8_t *m = rp(r, U32(t + 12)) + 12 * i; if (nm && !strcmp(rs(r, U32(m)), nm)) return (uint64_t)(uintptr_t)mk_type(r, U32(m + 4), U32(m + 8)); }
  return 0;
}
uint64_t t_member_tname(CPU *c) {
  RObj *o = (RObj *)(uintptr_t)A(0); Refl *r = o->r; const uint8_t *t = rp(r, o->a); uint32_t i = (uint32_t)A(1);
  if (i >= U16(t + 10)) return 0; return (uint64_t)(uintptr_t)rs(r, U32(rp(r, U32(t + 12)) + 12 * i));
}
/* --- variables --- */
static uint64_t v_getdesc(CPU *c) {
  RObj *o = (RObj *)(uintptr_t)A(0); Refl *r = o->r; const uint8_t *v = rp(r, o->a); uint8_t *d = (uint8_t *)(uintptr_t)A(1);
  put_ptr((uint64_t)(uintptr_t)d, (uint64_t)(uintptr_t)rs(r, U32(v))); ST(uint32_t, d + 4, U32(v + 4)); ST(uint32_t, d + 8, U32(v + 8)); ST(uint32_t, d + 12, U32(v + 12));
  uint32_t dv = U32(v + 20); put_ptr((uint64_t)(uintptr_t)d + 16, dv ? (uint64_t)(uintptr_t)rp(r, dv) : 0); return 0;
}
static uint64_t v_gettype(CPU *c) { RObj *o = (RObj *)(uintptr_t)A(0); return (uint64_t)(uintptr_t)mk_type(o->r, U32(rp(o->r, o->a) + 16), 0); }
/* --- constant buffers --- */
static RObj *mk_var(Refl *r, int cb, int i) {
  if (cb >= 64 || i >= 128) return NULL; if (r->var[cb][i]) return r->var[cb][i];
  const uint8_t *b = rp(r, U32(rp(r, 0) + 4) + 24 * cb); static const shim_fn f[2] = { v_getdesc, v_gettype }; static const int ac[2] = { 2, 1 };
  RObj *o = aot_gcalloc(1, sizeof *o); if (!o) return NULL; o->vt = robj_vt("ID3D10ShaderReflectionVariable", 2, f, ac, &g_vt_var); o->refs = 1; o->r = r;
  o->a = U32(b + 8) + (uint32_t)(rdef_varsz(r) * i); return r->var[cb][i] = o;
}
static const uint8_t *cb_rec(Refl *r, int cb) { return rp(r, U32(rp(r, 0) + 4) + 24 * cb); }
static uint64_t cb_getdesc(CPU *c) {
  RObj *o = (RObj *)(uintptr_t)A(0); Refl *r = o->r; const uint8_t *b = cb_rec(r, o->a); uint8_t *d = (uint8_t *)(uintptr_t)A(1);
  put_ptr((uint64_t)(uintptr_t)d, (uint64_t)(uintptr_t)rs(r, U32(b))); ST(uint32_t, d + 4, U32(b + 20)); ST(uint32_t, d + 8, U32(b + 4)); uint32_t sz = U32(b + 12);
  if (!sz) { const uint8_t *v = rp(r, U32(b + 8)); for (uint32_t i = 0; i < U32(b + 4); i++, v += rdef_varsz(r)) { uint32_t e = U32(v + 4) + U32(v + 8); if (e > sz) sz = e; } sz = (sz + 15) & ~15u; }
  ST(uint32_t, d + 12, sz); ST(uint32_t, d + 16, U32(b + 16)); if (!sz) fprintf(stderr, "[aot] reflection: constant buffer '%s' has size 0 (vars=%u)\n", rs(r, U32(b)), U32(b + 4)); return 0;
}
static uint64_t cb_varidx(CPU *c) { RObj *o = (RObj *)(uintptr_t)A(0); uint32_t i = (uint32_t)A(1); if (i >= U32(cb_rec(o->r, o->a) + 4)) return 0; return (uint64_t)(uintptr_t)mk_var(o->r, o->a, i); }
static uint64_t cb_varname(CPU *c) {
  RObj *o = (RObj *)(uintptr_t)A(0); Refl *r = o->r; const uint8_t *b = cb_rec(r, o->a); const char *nm = (const char *)(uintptr_t)A(1);
  for (uint32_t i = 0; nm && i < U32(b + 4); i++) if (!strcmp(rs(r, U32(rp(r, U32(b + 8)) + (size_t)rdef_varsz(r) * i)), nm)) return (uint64_t)(uintptr_t)mk_var(r, o->a, i);
  return 0;
}
static RObj *mk_cb(Refl *r, int i) {
  if (i < 0 || i >= 64 || (uint32_t)i >= U32(rp(r, 0))) return NULL; if (r->cb[i]) return r->cb[i];
  static const shim_fn f[3] = { cb_getdesc, cb_varidx, cb_varname }; static const int ac[3] = { 2, 2, 2 };
  RObj *o = aot_gcalloc(1, sizeof *o); if (!o) return NULL; o->vt = robj_vt("ID3D10ShaderReflectionConstantBuffer", 3, f, ac, &g_vt_cb); o->refs = 1; o->r = r; o->a = i; return r->cb[i] = o;
}
/* --- signature --- */
static const uint8_t *sig_rec(const uint8_t *s, uint32_t i, int *esz) { *esz = (s && U32(s + 4) == 8) ? 24 : 24; return s + 8 + (size_t)*esz * i; }
static uint64_t sig_desc(CPU *c, const uint8_t *s) {
  uint32_t i = (uint32_t)A(1); uint8_t *d = (uint8_t *)(uintptr_t)A(2); if (!s || i >= U32(s)) return 0x80070057u;
  int esz; const uint8_t *e = sig_rec(s, i, &esz); put_ptr((uint64_t)(uintptr_t)d, (uint64_t)(uintptr_t)(s + U32(e)));
  ST(uint32_t, d + 4, U32(e + 4)); ST(uint32_t, d + 8, U32(e + 16)); ST(uint32_t, d + 12, U32(e + 8)); ST(uint32_t, d + 16, U32(e + 12)); ST(uint8_t, d + 20, e[20]); ST(uint8_t, d + 21, e[21]); return 0;
}
/* --- the reflection object --- */
static uint64_t rf_release(CPU *c) { Refl *r = (Refl *)(uintptr_t)A(0); uint32_t n = r->refs ? --r->refs : 0; return n; }   /* objects are tiny and shared with sub-objects; reclaimed at process exit */
static uint64_t rf_getdesc(CPU *c) {
  Refl *r = (Refl *)(uintptr_t)A(0); uint8_t *d = (uint8_t *)(uintptr_t)A(1); memset(d, 0, 112);
  ST(uint32_t, d, r->shdr ? U32(r->shdr) : 0); put_ptr((uint64_t)(uintptr_t)d + 4, (uint64_t)(uintptr_t)rs(r, U32(r->rdef + 24))); ST(uint32_t, d + 8, U32(r->rdef + 20));
  ST(uint32_t, d + 12, U32(r->rdef)); ST(uint32_t, d + 16, U32(r->rdef + 8)); ST(uint32_t, d + 20, r->isgn ? U32(r->isgn) : 0); ST(uint32_t, d + 24, r->osgn ? U32(r->osgn) : 0);
  if (r->stat && r->n) { const uint32_t *s = (const uint32_t *)r->stat; ST(uint32_t, d + 28, s[0]); ST(uint32_t, d + 32, s[1]); ST(uint32_t, d + 36, s[10]); ST(uint32_t, d + 40, s[2]); ST(uint32_t, d + 44, s[3]);
    ST(uint32_t, d + 48, s[14]); ST(uint32_t, d + 52, s[15]); ST(uint32_t, d + 56, s[16]); ST(uint32_t, d + 60, s[17]); ST(uint32_t, d + 64, s[18]); ST(uint32_t, d + 68, s[4]); ST(uint32_t, d + 72, s[5]);
    ST(uint32_t, d + 76, s[6]); ST(uint32_t, d + 80, s[7]); ST(uint32_t, d + 84, s[8]); ST(uint32_t, d + 88, s[9]); ST(uint32_t, d + 92, s[11]); ST(uint32_t, d + 96, s[12]); ST(uint32_t, d + 100, s[13]); }
  return 0;
}
static uint64_t rf_cbidx(CPU *c) { return (uint64_t)(uintptr_t)mk_cb((Refl *)(uintptr_t)A(0), (int)A(1)); }
static uint64_t rf_cbname(CPU *c) {
  Refl *r = (Refl *)(uintptr_t)A(0); const char *nm = (const char *)(uintptr_t)A(1);
  for (uint32_t i = 0; nm && i < U32(r->rdef) && i < 64; i++) if (!strcmp(rs(r, U32(cb_rec(r, i))), nm)) return (uint64_t)(uintptr_t)mk_cb(r, i);
  return 0;
}
static uint64_t rf_resdesc(CPU *c) {
  Refl *r = (Refl *)(uintptr_t)A(0); uint32_t i = (uint32_t)A(1); uint8_t *d = (uint8_t *)(uintptr_t)A(2); if (i >= U32(r->rdef + 8)) return 0x80070057u;
  const uint8_t *b = rp(r, U32(r->rdef + 12)) + 32 * (size_t)i; put_ptr((uint64_t)(uintptr_t)d, (uint64_t)(uintptr_t)rs(r, U32(b)));
  ST(uint32_t, d + 4, U32(b + 4)); ST(uint32_t, d + 8, U32(b + 20)); ST(uint32_t, d + 12, U32(b + 24)); ST(uint32_t, d + 16, U32(b + 28)); ST(uint32_t, d + 20, U32(b + 8)); ST(uint32_t, d + 24, U32(b + 12)); ST(uint32_t, d + 28, U32(b + 16)); return 0;
}
static uint64_t rf_in(CPU *c) { Refl *r = (Refl *)(uintptr_t)A(0); return sig_desc(c, r->isgn); }
static uint64_t rf_out(CPU *c) { Refl *r = (Refl *)(uintptr_t)A(0); return sig_desc(c, r->osgn); }
static const uint8_t *dxbc_chunk(const uint8_t *d, size_t n, const char *tag, uint32_t *sz) {
  if (n < 32 || memcmp(d, "DXBC", 4)) return NULL; uint32_t cnt = U32(d + 28);
  for (uint32_t i = 0; i < cnt && 32 + 4 * i + 4 <= n; i++) { uint32_t off = U32(d + 32 + 4 * i); if (off + 8 > n) return NULL; if (!memcmp(d + off, tag, 4)) { *sz = U32(d + off + 4); return d + off + 8; } }
  return NULL;
}
static uint64_t d3d10_reflect_shader(CPU *c) {
  /* (pShaderBytecode, BytecodeLength, ppReflector) */
  clear_guest_pointer(ARG(2)); const uint8_t *src = (const uint8_t *)(uintptr_t)ARG(0); size_t n = (size_t)(uint32_t)ARG(1);
  if (!src || !ARG(2)) return 0x80070057u;
  Refl *r = aot_gcalloc(1, sizeof *r); uint8_t *copy = aot_gmalloc(n ? n : 1); if (!r || !copy) return 0x8007000Eu; memcpy(copy, src, n);
  uint32_t sz; r->d = copy; r->n = n; r->rdef = dxbc_chunk(copy, n, "RDEF", &sz); r->rdef_n = sz;
  r->isgn = dxbc_chunk(copy, n, "ISGN", &sz); r->osgn = dxbc_chunk(copy, n, "OSGN", &sz); if (!r->osgn) r->osgn = dxbc_chunk(copy, n, "OSG5", &sz);
  r->shdr = dxbc_chunk(copy, n, "SHDR", &sz); if (!r->shdr) r->shdr = dxbc_chunk(copy, n, "SHEX", &sz); r->stat = dxbc_chunk(copy, n, "STAT", &sz);
  if (!r->rdef) { fprintf(stderr, "[aot] D3D10ReflectShader: bytecode has no RDEF chunk (%zu bytes)\n", n); return 0x80004005u; }
  static const shim_fn f[9] = { r_qi, r_addref, rf_release, rf_getdesc, rf_cbidx, rf_cbname, rf_resdesc, rf_in, rf_out }; static const int ac[9] = { 3, 1, 1, 2, 2, 2, 3, 3, 3 };
  r->vt = robj_vt("ID3D10ShaderReflection", 9, f, ac, &g_vt_refl); r->refs = 1; put_ptr(ARG(2), (uint64_t)(uintptr_t)r); return 0;
}

const ShimEnt aot_shim_table_d3d10[] = {
  { "D3D10CreateDevice", d3d10_create_device },
  { "D3D10CreateDeviceAndSwapChain", d3d10_create_device_and_swapchain },
  { "D3D10CompileShader", d3d10_compile_shader },
  { "D3D10ReflectShader", d3d10_reflect_shader },
  { 0, 0 }
};

#undef ARG
