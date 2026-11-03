#define _GNU_SOURCE
#include "../src/runtime/win_file.c"

#include <assert.h>
#include <stdlib.h>

const char *g_exe_path = "game.exe";
char g_cmdline[1024] = "game.exe";
uint64_t g_image_base;
uint32_t aot_last_error;

static shim_fn find_shim(const char *name) {
  for (const ShimEnt *shim = aot_shim_table_file; shim->name; shim++)
    if (!strcmp(shim->name, name)) return shim->fn;
  return NULL;
}

static uint64_t invoke(const char *name, const uint64_t *arguments,
                       size_t count) {
  CPU cpu = { 0 };
  uint64_t stack[8] = { 0 };
  cpu.r[4] = (uint64_t)(uintptr_t)stack;
  if (count > 0) cpu.r[1] = arguments[0];
  if (count > 1) cpu.r[2] = arguments[1];
  if (count > 2) cpu.r[8] = arguments[2];
  if (count > 3) cpu.r[9] = arguments[3];
  for (size_t i = 4; i < count; i++) stack[1 + i] = arguments[i];
  shim_fn shim = find_shim(name);
  assert(shim);
  return shim(&cpu);
}

static uint64_t find_first(const char *pattern, void *data) {
  uint64_t arguments[] = {
    (uint64_t)(uintptr_t)pattern, (uint64_t)(uintptr_t)data
  };
  return invoke("FindFirstFileA", arguments, 2);
}

int main(void) {
  char root[] = "/tmp/aot-winfile-XXXXXX";
  assert(mkdtemp(root));
  assert(chdir(root) == 0);
  assert(mkdir("Assets", 0700) == 0);
  FILE *file = fopen("Assets/Save01.DAT", "wb");
  assert(file);
  assert(fwrite("save", 1, 4, file) == 4);
  assert(fclose(file) == 0);
  file = fopen("Assets/portrait.png", "wb");
  assert(file);
  assert(fwrite("image", 1, 5, file) == 5);
  assert(fclose(file) == 0);

  aot_win_file_init();
  uint8_t data[592];
  memset(data, 0, sizeof data);
  uint64_t search = find_first("assets\\*.dat", data);
  assert(search != UINT64_MAX);
  assert(!strcmp((const char *)data + 44, "Save01.DAT"));
  assert(LD(uint32_t, data + 32) == 4);
  assert(invoke("FindNextFileA", (uint64_t[]){ search,
      (uint64_t)(uintptr_t)data }, 2) == 0);
  assert(aot_last_error == 18);
  assert(invoke("FindClose", (uint64_t[]){ search }, 1) == 1);
  assert(invoke("FindClose", (uint64_t[]){ search }, 1) == 0);
  assert(aot_last_error == 6);

  memset(data, 0, sizeof data);
  uint64_t all_search = find_first("Assets\\*.*", data);
  assert(all_search != UINT64_MAX);
  unsigned matches = 1;
  while (invoke("FindNextFileA", (uint64_t[]){ all_search,
      (uint64_t)(uintptr_t)data }, 2))
    matches++;
  assert(matches == 2);
  assert(aot_last_error == 18);
  assert(invoke("FindClose", (uint64_t[]){ all_search }, 1) == 1);

  assert(find_first("Assets/*.missing", data) == UINT64_MAX);
  assert(aot_last_error == 2);

  const uint16_t wide_pattern[] = {
    'A','s','s','e','t','s','/','p','o','r','t','r','a','i','t','.',
    'p','n','g',0
  };
  memset(data, 0, sizeof data);
  uint64_t wide_search = invoke("FindFirstFileW",
      (uint64_t[]){ (uint64_t)(uintptr_t)wide_pattern,
        (uint64_t)(uintptr_t)data }, 2);
  assert(wide_search != UINT64_MAX);
  assert(LD(uint16_t, data + 44) == 'p');
  assert(LD(uint16_t, data + 44 + 16) == '.');
  assert(invoke("FindClose", (uint64_t[]){ wide_search }, 1) == 1);

  const char save_path[] = "Assets/Save01.DAT";
  uint64_t file_handle = invoke("CreateFileA",
      (uint64_t[]){ (uint64_t)(uintptr_t)save_path, 0xc0000000u, 0, 0, 3, 0, 0 },
      7);
  assert(file_handle != UINT64_MAX);
  uint64_t mapping_handle = invoke("CreateFileMappingA",
      (uint64_t[]){ file_handle, 0, PAGE_READWRITE, 0, 0, 0 }, 6);
  assert(mapping_handle);
  uint64_t mapped = invoke("MapViewOfFile",
      (uint64_t[]){ mapping_handle, FILE_MAP_WRITE, 0, 0, 0 }, 5);
  assert(mapped);
  assert(invoke("MapViewOfFile",
      (uint64_t[]){ mapping_handle, FILE_MAP_READ, 0, 1, 4 }, 5) == 0);
  assert(aot_last_error == ERROR_MAPPED_ALIGNMENT);
  assert(!memcmp((const void *)(uintptr_t)mapped, "save", 4));
  memcpy((void *)(uintptr_t)mapped, "SAVE", 4);
  assert(invoke("FlushViewOfFile",
      (uint64_t[]){ mapped, 4 }, 2) == 1);
  assert(invoke("UnmapViewOfFile", (uint64_t[]){ mapped }, 1) == 1);
  assert(invoke("UnmapViewOfFile", (uint64_t[]){ mapped }, 1) == 0);
  assert(aot_last_error == 87);
  assert(invoke("CloseHandle", (uint64_t[]){ mapping_handle }, 1) == 1);
  assert(invoke("CloseHandle", (uint64_t[]){ file_handle }, 1) == 1);
  file = fopen("Assets/Save01.DAT", "rb");
  assert(file);
  char updated[4];
  assert(fread(updated, 1, sizeof updated, file) == sizeof updated);
  assert(fclose(file) == 0);
  assert(!memcmp(updated, "SAVE", 4));

  uint64_t anonymous_mapping = invoke("CreateFileMappingW",
      (uint64_t[]){ UINT64_MAX, 0, PAGE_READWRITE | SEC_COMMIT, 0, 65536, 0 }, 6);
  assert(anonymous_mapping);
  assert(invoke("MapViewOfFileEx",
      (uint64_t[]){ anonymous_mapping, FILE_MAP_READ, 0, 0, 32, 1 }, 6) == 0);
  assert(aot_last_error == ERROR_INVALID_PARAMETER);
  uint64_t anonymous_view1 = invoke("MapViewOfFile",
      (uint64_t[]){ anonymous_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 32 }, 5);
  uint64_t anonymous_view2 = invoke("MapViewOfFile",
      (uint64_t[]){ anonymous_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 32 }, 5);
  assert(anonymous_view1 && anonymous_view2);
  memcpy((void *)(uintptr_t)anonymous_view1, "shared", 7);
  assert(invoke("CloseHandle", (uint64_t[]){ anonymous_mapping }, 1) == 1);
  assert(!memcmp((const void *)(uintptr_t)anonymous_view2, "shared", 7));
  assert(invoke("UnmapViewOfFile", (uint64_t[]){ anonymous_view1 }, 1) == 1);
  assert(invoke("UnmapViewOfFile", (uint64_t[]){ anonymous_view2 }, 1) == 1);

  const char async_path[] = "Assets/Async.bin";
  uint64_t async_file = invoke("CreateFileA",
      (uint64_t[]){ (uint64_t)(uintptr_t)async_path, 0xc0000000u, 0, 0, 2,
        0x40000000u, 0 }, 7);
  assert(async_file != UINT64_MAX);
  uint64_t event = invoke("CreateEventA",
      (uint64_t[]){ 0, 0, 0, 0 }, 4);
  assert(event);
  uint8_t overlapped[32] = { 0 };
  ST(uint64_t, overlapped + 24, event);
  const char async_write[] = "ASYNC";
  uint32_t transferred = UINT32_MAX;
  assert(invoke("WriteFile",
      (uint64_t[]){ async_file, (uint64_t)(uintptr_t)async_write,
        sizeof async_write - 1, (uint64_t)(uintptr_t)&transferred,
        (uint64_t)(uintptr_t)overlapped }, 5) == 0);
  assert(aot_last_error == ERROR_IO_PENDING);
  assert(invoke("WaitForSingleObject", (uint64_t[]){ event, 5000 }, 2) == 0);
  assert(invoke("GetOverlappedResult",
      (uint64_t[]){ async_file, (uint64_t)(uintptr_t)overlapped,
        (uint64_t)(uintptr_t)&transferred, 0 }, 4) == 1);
  assert(transferred == sizeof async_write - 1);
  assert(invoke("WaitForSingleObject", (uint64_t[]){ event, 0 }, 2) == 258);
  assert(invoke("ResetEvent", (uint64_t[]){ event }, 1) == 1);

  memset(overlapped, 0, sizeof overlapped);
  ST(uint32_t, overlapped + 16, 0);
  ST(uint64_t, overlapped + 24, event);
  char async_read[sizeof async_write] = { 0 };
  assert(invoke("ReadFile",
      (uint64_t[]){ async_file, (uint64_t)(uintptr_t)async_read,
        sizeof async_read - 1, 0, (uint64_t)(uintptr_t)overlapped }, 5) == 0);
  assert(aot_last_error == ERROR_IO_PENDING);
  assert(invoke("WaitForSingleObject", (uint64_t[]){ event, 5000 }, 2) == 0);
  assert(invoke("GetOverlappedResult",
      (uint64_t[]){ async_file, (uint64_t)(uintptr_t)overlapped,
        (uint64_t)(uintptr_t)&transferred, 1 }, 4) == 1);
  assert(transferred == sizeof async_write - 1);
  assert(!memcmp(async_read, async_write, sizeof async_write - 1));
  assert(invoke("CloseHandle", (uint64_t[]){ event }, 1) == 1);
  assert(invoke("CloseHandle", (uint64_t[]){ async_file }, 1) == 1);

  uint8_t traversal_data[592];
  assert(find_first("../*", traversal_data) == UINT64_MAX);
  assert(aot_last_error == 87);
  close(game_root_fd);
  game_root_fd = -1;
  assert(unlink("Assets/Save01.DAT") == 0);
  assert(unlink("Assets/portrait.png") == 0);
  assert(unlink("Assets/Async.bin") == 0);
  assert(rmdir("Assets") == 0);
  assert(chdir("/tmp") == 0);
  assert(rmdir(root) == 0);
  puts("win_file enumeration, mapping, and async I/O: PASS");
  return 0;
}
