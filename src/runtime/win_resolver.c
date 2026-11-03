#include "shims.h"

typedef struct { char name[128]; int used; } RuntimeModule;
typedef struct { uint64_t module, thunk; char symbol[256]; } DynamicThunk;

static RuntimeModule modules[128];
static DynamicThunk dynamic_thunks[256];
static size_t dynamic_thunk_count;

static int table_find(const ShimEnt *table, const char *name, shim_fn *out) {
  for (const ShimEnt *entry = table; entry->name; entry++) {
    if (!strcmp(entry->name, name)) {
      *out = entry->fn;
      return 1;
    }
  }
  return 0;
}

static int canonical_dll(const char *input, char out[128]) {
  if (!input || !*input) return 0;
  const char *base = input;
  for (const char *p = input; *p; p++)
    if (*p == '/' || *p == '\\') base = p + 1;
  size_t length = strlen(base);
  if (!length || length >= 128) return 0;
  for (size_t i = 0; i < length; i++) {
    unsigned char ch = (unsigned char)base[i];
    out[i] = (char)(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
  }
  out[length] = 0;
  if (!strrchr(out, '.')) {
    if (length + 4 >= 128) return 0;
    memcpy(out + length, ".dll", 5);
  }
  return 1;
}

static int begins(const char *text, const char *prefix) {
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

static int is_crt_dll(const char *dll) {
  return !strcmp(dll, "msvcrt.dll") || !strcmp(dll, "ucrtbase.dll") ||
         begins(dll, "api-ms-win-crt-");
}

static int api_set_family(const char *dll, const char *family) {
  char prefix[64];
  snprintf(prefix, sizeof prefix, "api-ms-win-core-%s-", family);
  return begins(dll, prefix);
}

static int core_api_set_supported(const char *dll) {
  static const char *families[] = {
      "console", "errorhandling", "file", "fibers", "heap",
      "libraryloader", "localization", "memory", "processenvironment",
      "processthreads", "synch", "sysinfo", "datetime"};
  for (size_t i = 0; i < sizeof families / sizeof families[0]; i++)
    if (api_set_family(dll, families[i])) return 1;
  return 0;
}

static int core_api_set_lookup(const char *dll, const char *name,
                               shim_fn *out) {
  if (api_set_family(dll, "file"))
    return strcmp(name, "GetLastError") && strcmp(name, "SetLastError") &&
           table_find(aot_shim_table_file, name, out);
  if (api_set_family(dll, "heap"))
    return begins(name, "Heap") &&
           table_find(aot_shim_table_win32, name, out);
  if (api_set_family(dll, "memory"))
    return (!strcmp(name, "VirtualAlloc") || !strcmp(name, "VirtualFree") ||
            !strcmp(name, "VirtualProtect")) &&
           table_find(aot_shim_table_win32, name, out);
  if (api_set_family(dll, "libraryloader"))
    return (begins(name, "LoadLibrary") || begins(name, "GetModuleHandle") ||
            !strcmp(name, "GetProcAddress")) &&
           table_find(aot_shim_table_win32, name, out);
  if (api_set_family(dll, "processthreads"))
    return (begins(name, "GetCurrent") || begins(name, "Tls") ||
            !strcmp(name, "ExitProcess")) &&
           table_find(aot_shim_table_win32, name, out);
  if (api_set_family(dll, "fibers"))
    return begins(name, "Fls") &&
           table_find(aot_shim_table_win32, name, out);
  if (api_set_family(dll, "processenvironment"))
    return (begins(name, "GetCommandLine") || begins(name, "GetStartupInfo") ||
            begins(name, "GetEnvironmentStrings") ||
            begins(name, "FreeEnvironmentStrings")) &&
           table_find(aot_shim_table_win32, name, out);
  if (api_set_family(dll, "synch"))
    return !strcmp(name, "Sleep") || begins(name, "InitializeCriticalSection") ||
           begins(name, "EnterCriticalSection") ||
           begins(name, "LeaveCriticalSection") ||
           begins(name, "DeleteCriticalSection") ||
           begins(name, "TryEnterCriticalSection")
        ? table_find(aot_shim_table_win32, name, out) : 0;
  if (api_set_family(dll, "sysinfo") || api_set_family(dll, "datetime"))
    return begins(name, "GetTickCount") ||
           begins(name, "QueryPerformance") ||
           !strcmp(name, "GetSystemTimeAsFileTime") ||
           !strcmp(name, "GetSystemInfo")
        ? table_find(aot_shim_table_win32, name, out) : 0;
  if (api_set_family(dll, "console"))
    return !strcmp(name, "GetStdHandle") || !strcmp(name, "GetConsoleMode") ||
           begins(name, "SetConsole")
        ? table_find(aot_shim_table_win32, name, out) : 0;
  if (api_set_family(dll, "errorhandling"))
    return !strcmp(name, "GetLastError") || !strcmp(name, "SetLastError") ||
           begins(name, "SetUnhandledException") ||
           begins(name, "UnhandledException") ||
           !strcmp(name, "SetErrorMode") || !strcmp(name, "IsDebuggerPresent")
        ? (table_find(aot_shim_table_win32, name, out) ||
           table_find(aot_shim_table_file, name, out)) : 0;
  if (api_set_family(dll, "localization"))
    return !strcmp(name, "GetACP") &&
           table_find(aot_shim_table_win32, name, out);
  return 0;
}

static int find_graphics_export(const char *dll, const char *name,
                                shim_fn *out) {
  if (!strcmp(dll, "d3d11.dll"))
    return (!strcmp(name, "D3D11CreateDevice") ||
            !strcmp(name, "D3D11CreateDeviceAndSwapChain")) &&
           table_find(aot_shim_table_gfx, name, out);
  if (!strcmp(dll, "dxgi.dll"))
    return begins(name, "CreateDXGIFactory") &&
           table_find(aot_shim_table_gfx, name, out);
  if (!strcmp(dll, "user32.dll")) {
    if (begins(name, "D3D11Create") || begins(name, "CreateDXGI") ||
        begins(name, "time"))
      return 0;
    return table_find(aot_shim_table_gfx, name, out);
  }
  if (!strcmp(dll, "winmm.dll")) {
    if (!strcmp(name, "timeGetTime") || !strcmp(name, "timeBeginPeriod") ||
        !strcmp(name, "timeEndPeriod"))
      return table_find(aot_shim_table_gfx, name, out);
  }
  return 0;
}

shim_fn aot_resolve_import(const char *dll_name, const char *name) {
  char dll[128];
  if (!name || !canonical_dll(dll_name, dll)) return NULL;
  shim_fn fn = NULL;
  if (begins(dll, "xinput"))
    return table_find(aot_shim_table_input, name, &fn) ? fn : NULL;
  if (!strcmp(dll, "user32.dll") && table_find(aot_shim_table_input, name, &fn))
    return fn;
  if ((!strcmp(dll, "kernel32.dll") || !strcmp(dll, "kernelbase.dll") || !strcmp(dll, "ntdll.dll") ||
       begins(dll, "api-ms-win-core-")) && table_find(aot_shim_table_sys, name, &fn))
    return fn;
  if (is_crt_dll(dll))
    return table_find(aot_shim_table_crt, name, &fn) ? fn : NULL;
  if (!strcmp(dll, "kernel32.dll") || !strcmp(dll, "kernelbase.dll")) {
    if (table_find(aot_shim_table_win32, name, &fn) ||
        table_find(aot_shim_table_file, name, &fn) ||
        table_find(aot_shim_table_compat, name, &fn))
      return fn;
    return NULL;
  }
  if (begins(dll, "api-ms-win-core-"))
    return core_api_set_lookup(dll, name, &fn) ? fn : NULL;
  if (!strcmp(dll, "d3d10.dll"))
    return table_find(aot_shim_table_d3d10, name, &fn) ? fn : NULL;
  if (!strcmp(dll, "ntdll.dll"))
    return table_find(aot_shim_table_ntdll, name, &fn) ? fn : NULL;
  if (!strcmp(dll, "winmm.dll") &&
      table_find(aot_shim_table_audio, name, &fn))
    return fn;
  return find_graphics_export(dll, name, &fn) ? fn : NULL;
}

int aot_import_is_placeholder(const char *dll, const char *name) {
  shim_fn fn = aot_resolve_import(dll, name);
  return fn && (aot_shim_is_placeholder(fn) ||
                aot_win_gfx_is_placeholder(fn));
}

static int module_supported(const char *dll) {
  return is_crt_dll(dll) || core_api_set_supported(dll) ||
      !strcmp(dll, "kernel32.dll") || !strcmp(dll, "kernelbase.dll") ||
      !strcmp(dll, "user32.dll") || !strcmp(dll, "winmm.dll") ||
      !strcmp(dll, "d3d10.dll") || !strcmp(dll, "d3d11.dll") ||
      !strcmp(dll, "dxgi.dll") || !strcmp(dll, "ntdll.dll") || begins(dll, "xinput");
}

static RuntimeModule *find_module(const char *dll) {
  for (size_t i = 0; i < sizeof modules / sizeof modules[0]; i++)
    if (modules[i].used && !strcmp(modules[i].name, dll))
      return &modules[i];
  return NULL;
}

uint64_t aot_module_load(const char *dll_name) {
  char dll[128];
  if (!canonical_dll(dll_name, dll) || !module_supported(dll)) return 0;
  RuntimeModule *module = find_module(dll);
  if (!module) {
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; i++) {
      if (!modules[i].used) {
        module = &modules[i];
        snprintf(module->name, sizeof module->name, "%s", dll);
        module->used = 1;
        break;
      }
    }
  }
  return module ? (uint64_t)(uintptr_t)module : 0;
}

uint64_t aot_module_get_handle(const char *dll_name) {
  char dll[128];
  if (!canonical_dll(dll_name, dll)) return 0;
  RuntimeModule *module = find_module(dll);
  return module ? (uint64_t)(uintptr_t)module : 0;
}

uint64_t aot_module_get_proc(uint64_t module_handle,
                             uint64_t name_or_ordinal) {
  RuntimeModule *module = NULL;
  for (size_t i = 0; i < sizeof modules / sizeof modules[0]; i++) {
    if (modules[i].used &&
        module_handle == (uint64_t)(uintptr_t)&modules[i]) {
      module = &modules[i];
      break;
    }
  }
  if (!module) {
    if (getenv("AOT_VERBOSE"))
      fprintf(stderr,
              "[aot] dynamic GetProcAddress unresolved: invalid module handle 0x%llx\n",
              (unsigned long long)module_handle);
    return 0;
  }
  char ordinal_name[24];
  const char *symbol;
  if (name_or_ordinal <= 0xffff) {
    snprintf(ordinal_name, sizeof ordinal_name, "#%u",
             (unsigned)name_or_ordinal);
    symbol = ordinal_name;
  } else {
    symbol = (const char *)(uintptr_t)name_or_ordinal;
    if (!symbol || !*symbol) return 0;
  }
  shim_fn fn = aot_resolve_import(module->name, symbol);
  if (!fn) {
    if (getenv("AOT_VERBOSE"))
      fprintf(stderr, "[aot] dynamic GetProcAddress unresolved: %s!%s\n",
              module->name, symbol);
    return 0;
  }
  for (size_t i = 0; i < dynamic_thunk_count; i++)
    if (dynamic_thunks[i].module == module_handle &&
        !strcmp(dynamic_thunks[i].symbol, symbol))
      return dynamic_thunks[i].thunk;
  if (dynamic_thunk_count >=
      sizeof dynamic_thunks / sizeof dynamic_thunks[0]) {
    if (getenv("AOT_VERBOSE"))
      fprintf(stderr, "[aot] dynamic GetProcAddress unresolved: cache full\n");
    return 0;
  }
  char thunk_name[96];
  snprintf(thunk_name, sizeof thunk_name, "dynamic:%s!%.64s",
           module->name, symbol);
  uint64_t thunk = aot_register_thunk((aot_host_fn)fn, thunk_name);
  if (!thunk) {
    if (getenv("AOT_VERBOSE"))
      fprintf(stderr, "[aot] dynamic GetProcAddress unresolved: thunk table full\n");
    return 0;
  }
  DynamicThunk *entry = &dynamic_thunks[dynamic_thunk_count++];
  entry->module = module_handle;
  entry->thunk = thunk;
  snprintf(entry->symbol, sizeof entry->symbol, "%s", symbol);
  if (getenv("AOT_VERBOSE")) {
    if (aot_import_is_placeholder(module->name, symbol))
      fprintf(stderr, "[aot] dynamic GetProcAddress uses placeholder shim: %s!%s\n",
              module->name, symbol);
    else
      fprintf(stderr, "[aot] dynamic GetProcAddress resolved: %s!%s\n",
              module->name, symbol);
  }
  return thunk;
}
