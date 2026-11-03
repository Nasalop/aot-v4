#pragma once
#include "aot_runtime.h"
typedef uint64_t (*shim_fn)(CPU *);
typedef struct { const char *name; shim_fn fn; } ShimEnt;
extern const ShimEnt aot_shim_table_crt[];
extern const ShimEnt aot_shim_table_win32[];
extern const ShimEnt aot_shim_table_ntdll[];
extern const ShimEnt aot_shim_table_file[];
extern const ShimEnt aot_shim_table_audio[];
extern const ShimEnt aot_shim_table_d3d10[];
extern const ShimEnt aot_shim_table_gfx[];
extern const ShimEnt aot_shim_table_compat[];
shim_fn aot_resolve_import(const char *dll, const char *name);
int aot_import_is_placeholder(const char *dll, const char *name);
int aot_shim_is_placeholder(shim_fn fn);
int aot_win_gfx_is_placeholder(shim_fn fn);
uint64_t aot_module_load(const char *dll);
uint64_t aot_module_get_handle(const char *dll);
uint64_t aot_module_get_proc(uint64_t module, uint64_t name_or_ordinal);
void aot_win_audio_cleanup(void);
uint64_t aot_win_gfx_d3d10_create(uint64_t device_output,
                                  uint64_t swapchain_output,
                                  uint64_t swapchain_desc);
void shims_init(int argc, char **argv);
extern const char *g_exe_path; extern char g_cmdline[]; extern uint64_t g_image_base;
extern __thread uint32_t aot_last_error;
extern const ShimEnt aot_shim_table_sys[];
extern const ShimEnt aot_shim_table_input[];
extern __thread uint64_t aot_last_pc;
extern uint64_t g_peb_addr;
uint64_t aot_thread_run(uint64_t fn, uint64_t param, size_t ssz, uint32_t tid);
void aot_sys_init(void);
void aot_win_client_size(uint32_t *w, uint32_t *h);
void aot_win_file_init(void);
int aot_win_file_duplicate(uint64_t handle, int writing,
                           int require_overlapped);
int aot_win_async_close_handle(uint64_t handle);
uint64_t aot_file_async_CreateEventA(CPU *);
uint64_t aot_file_async_CreateEventW(CPU *);
uint64_t aot_file_async_SetEvent(CPU *);
uint64_t aot_file_async_ResetEvent(CPU *);
uint64_t aot_file_async_PulseEvent(CPU *);
uint64_t aot_file_async_WaitForSingleObject(CPU *);
uint64_t aot_file_async_GetOverlappedResult(CPU *);
uint64_t aot_file_async_ReadFile(CPU *);
uint64_t aot_file_async_WriteFile(CPU *);
/* PE32 support (win_x86.c) */
shim_fn aot_x86_resolve(const char *dll, const char *name, int *pop, int *fp);
uint64_t aot_x86_thunk(CPU *c, shim_fn fn, int pop, int fp);
uint64_t aot_x86_alias_in(uint64_t value);
uint64_t aot_x86_alias_out(uint64_t value);
void *aot_low_map(size_t n, uint64_t hint);
void *aot_gmalloc(size_t n); void *aot_gcalloc(size_t a, size_t b); void *aot_grealloc(void *p, size_t n); void aot_gfree(void *p); void *aot_map_anon(size_t n);
