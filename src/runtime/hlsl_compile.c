/* HLSL -> DXBC (SM4/5) through vkd3d-shader (Wine's bundled vkd3d), the backend behind D3D10CompileShader.
   libvkd3dshader.so is dlopen'd; host tests may link it directly with -DAOT_VKD3D_LINKED. */
#include "hlsl_compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vkd3d_shader.h>
#ifndef AOT_VKD3D_LINKED
#include <dlfcn.h>
static int (*p_compile)(const struct vkd3d_shader_compile_info *, struct vkd3d_shader_code *, char **);
static void (*p_free_code)(struct vkd3d_shader_code *); static void (*p_free_msg)(char *);
static int load(void) {
  static int st;
  if (st) return st > 0 ? 0 : -1; st = -1;
  const char *p = getenv("AOT_VKD3D_SHADER_LIB"); void *h = dlopen(p ? p : "libvkd3dshader.so", RTLD_NOW | RTLD_LOCAL);
  if (!h) { fprintf(stderr, "[hlsl] libvkd3dshader.so unavailable: %s\n", dlerror()); return -1; }
  p_compile = dlsym(h, "vkd3d_shader_compile"); p_free_code = dlsym(h, "vkd3d_shader_free_shader_code"); p_free_msg = dlsym(h, "vkd3d_shader_free_messages");
  if (!p_compile || !p_free_code || !p_free_msg) { fprintf(stderr, "[hlsl] libvkd3dshader.so is missing entry points\n"); return -1; }
  st = 1; return 0;
}
#else
#define p_compile vkd3d_shader_compile
#define p_free_code vkd3d_shader_free_shader_code
#define p_free_msg vkd3d_shader_free_messages
static int load(void) { return 0; }
#endif
#define F_ROW_MAJOR 0x8u
#define F_COL_MAJOR 0x10u
#define F_DEBUG 0x1u
int aot_hlsl_compile(const char *src, size_t len, const char *name, const AotHlslMacro *macros, size_t nmacros, const char *entry, const char *profile,
                     unsigned flags, void **code, size_t *size, char **log) {
  *code = NULL; *size = 0; *log = NULL;
  if (load()) return -2;
  if (!src || !entry || !profile) { *log = strdup("invalid arguments"); return -1; }
  /* #include needs a callback into guest code, which is not wired: fail loudly instead of compiling a different program */
  for (const char *p = src; p < src + len; p++) if (*p == '#') { const char *q = p + 1; while (q < src + len && (*q == ' ' || *q == '\t')) q++; if (!strncmp(q, "include", 7)) { *log = strdup("#include is not supported by this compiler backend"); return -1; } }
  struct vkd3d_shader_macro *vm = nmacros ? calloc(nmacros, sizeof *vm) : NULL;
  for (size_t i = 0; i < nmacros; i++) { vm[i].name = macros[i].name; vm[i].value = macros[i].value; }
  struct vkd3d_shader_preprocess_info pre = { .type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO, .macros = vm, .macro_count = (unsigned)nmacros };
  struct vkd3d_shader_hlsl_source_info hl = { .type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO, .next = &pre, .entry_point = entry, .profile = profile };
  struct vkd3d_shader_compile_option opt[3] = { { VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_2_1 } }; unsigned no = 1;
  if (flags & F_ROW_MAJOR) opt[no++] = (struct vkd3d_shader_compile_option){ VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER, VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ROW_MAJOR };
  else if (flags & F_COL_MAJOR) opt[no++] = (struct vkd3d_shader_compile_option){ VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER, VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_COLUMN_MAJOR };
  if (!(flags & F_DEBUG)) opt[no++] = (struct vkd3d_shader_compile_option){ VKD3D_SHADER_COMPILE_OPTION_STRIP_DEBUG, 1 };
  struct vkd3d_shader_compile_info ci = { .type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO, .next = &hl, .source = { src, len }, .source_type = VKD3D_SHADER_SOURCE_HLSL,
    .target_type = VKD3D_SHADER_TARGET_DXBC_TPF, .options = opt, .option_count = no, .log_level = VKD3D_SHADER_LOG_WARNING, .source_name = name ? name : "shader.hlsl" };
  struct vkd3d_shader_code out = { 0 }; char *msg = NULL;
  int rc = p_compile(&ci, &out, &msg);
  if (msg) { *log = strdup(msg); p_free_msg(msg); }
  free(vm);
  if (rc < 0) return rc;
  *code = malloc(out.size); memcpy(*code, out.code, out.size); *size = out.size; p_free_code(&out);
  return 0;
}
