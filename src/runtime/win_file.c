#define _GNU_SOURCE
#include "shims.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define ARG(n) ((n) == 0 ? c->r[1] : (n) == 1 ? c->r[2] : \
    (n) == 2 ? c->r[8] : (n) == 3 ? c->r[9] : \
    LD(uint64_t, c->r[4] + 8 + 8 * (n)))
#define SH(name) static uint64_t sh_##name(CPU *c)
#define A(n) ARG(n)
#define STR(p) ((const char *)(uintptr_t)(p))
#define STPTR(address, value) do { if (aot_is_pe32) ST(uint32_t, (address), (uint32_t)(value)); else ST(uint64_t, (address), (uint64_t)(value)); } while (0)

#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_DATA 13u
#define ERROR_NOT_SAME_DEVICE 17u
#define ERROR_WRITE_PROTECT 19u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_DISK_FULL 112u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_FILENAME_EXCED_RANGE 206u
#define ERROR_DIR_NOT_EMPTY 145u
#define ERROR_MAPPED_ALIGNMENT 1132u
#define ERROR_IO_PENDING 997u
#define INVALID_FILE_ATTRIBUTES UINT32_MAX
#define INVALID_FILE_SIZE UINT32_MAX
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_ALL 0x10000000u
#define FILE_READ_DATA 0x0001u
#define FILE_WRITE_DATA 0x0002u
#define FILE_APPEND_DATA 0x0004u
#define FILE_READ_ATTRIBUTES 0x0080u
#define FILE_FLAG_OVERLAPPED 0x40000000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_BEGIN 0u
#define FILE_CURRENT 1u
#define FILE_END 2u
#define FILE_ATTRIBUTE_READONLY 0x00000001u
#define FILE_ATTRIBUTE_HIDDEN 0x00000002u
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020u
#define FILE_TYPE_CHAR 2u
#define FILE_TYPE_DISK 1u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_WRITECOPY 0x08u
#define SEC_COMMIT 0x08000000u
#define FILE_MAP_COPY 0x0001u
#define FILE_MAP_WRITE 0x0002u
#define FILE_MAP_READ 0x0004u
#define FILE_MAP_ALL_ACCESS 0x000f001fu
#define FILE_MAP_EXECUTE 0x0020u
#define HANDLE_BASE 0xA07F000000000000ULL
#define MAPPING_HANDLE_BASE 0xA07D000000000000ULL
#define MAPPING_VIEW_BASE 0xA07C000000000000ULL
#define FIND_HANDLE_BASE 0xA07E000000000000ULL
#define HANDLE_MASK 0xFFFF000000000000ULL
#define FILE_HANDLE_CAPACITY 256
#define MAPPING_HANDLE_CAPACITY 128
#define MAPPING_VIEW_CAPACITY 256
#define FIND_HANDLE_CAPACITY 128
#define FIND_DATA_A_SIZE 320
#define FIND_DATA_W_SIZE 592

typedef struct {
  int fd;
  uint16_t generation;
  uint8_t open;
  uint8_t readable;
  uint8_t writable;
  uint8_t overlapped;
} FileHandle;

typedef struct {
  DIR *directory;
  char pattern[NAME_MAX + 1];
  uint16_t generation;
  uint8_t open;
  uint8_t wide;
} FindHandle;

typedef struct {
  int fd;
  uint64_t size;
  uint16_t generation;
  uint8_t open;
  uint8_t protection;
  uint8_t anonymous;
} MappingHandle;

typedef struct {
  void *base;
  size_t mapped_size;
  void *returned_address;
  uint64_t mapping_value;
  uint8_t open;
} MappingView;

static FileHandle handles[FILE_HANDLE_CAPACITY];
static FindHandle find_handles[FIND_HANDLE_CAPACITY];
static MappingHandle mapping_handles[MAPPING_HANDLE_CAPACITY];
static MappingView mapping_views[MAPPING_VIEW_CAPACITY];
static int game_root_fd = -1;
static char game_root_path[PATH_MAX];

static int win_error_from_errno(int error) {
  switch (error) {
    case 0: return 0;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
    case ELOOP: return ERROR_ACCESS_DENIED;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case ENOSPC: case EDQUOT: return ERROR_DISK_FULL;
    case EROFS: return ERROR_WRITE_PROTECT;
    case EISDIR: return ERROR_ACCESS_DENIED;
    case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    case EXDEV: return ERROR_NOT_SAME_DEVICE;
    case ENODEV: return ERROR_INVALID_PARAMETER;
    default: return ERROR_INVALID_DATA;
  }
}

static uint64_t fail_errno(void) {
  aot_last_error = (uint32_t)win_error_from_errno(errno);
  return 0;
}

static int append_utf8(char *out, size_t capacity, size_t *length,
                       uint32_t codepoint) {
  uint8_t bytes[4];
  size_t count;
  if (codepoint <= 0x7f) {
    bytes[0] = (uint8_t)codepoint;
    count = 1;
  } else if (codepoint <= 0x7ff) {
    bytes[0] = (uint8_t)(0xc0 | (codepoint >> 6));
    bytes[1] = (uint8_t)(0x80 | (codepoint & 0x3f));
    count = 2;
  } else if (codepoint <= 0xffff) {
    bytes[0] = (uint8_t)(0xe0 | (codepoint >> 12));
    bytes[1] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3f));
    bytes[2] = (uint8_t)(0x80 | (codepoint & 0x3f));
    count = 3;
  } else if (codepoint <= 0x10ffff) {
    bytes[0] = (uint8_t)(0xf0 | (codepoint >> 18));
    bytes[1] = (uint8_t)(0x80 | ((codepoint >> 12) & 0x3f));
    bytes[2] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3f));
    bytes[3] = (uint8_t)(0x80 | (codepoint & 0x3f));
    count = 4;
  } else {
    return -1;
  }
  if (*length + count >= capacity) return -1;
  memcpy(out + *length, bytes, count);
  *length += count;
  return 0;
}

static int path_from_wide(uint64_t address, char *out, size_t capacity) {
  if (!address) return -1;
  const uint16_t *wide = (const uint16_t *)(uintptr_t)address;
  size_t length = 0;
  for (size_t i = 0; i < capacity; i++) {
    uint32_t codepoint = wide[i];
    if (!codepoint) {
      out[length] = 0;
      return 0;
    }
    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
      if (i + 1 >= capacity) return -1;
      uint32_t low = wide[++i];
      if (low < 0xdc00 || low > 0xdfff) return -1;
      codepoint = 0x10000 + ((codepoint - 0xd800) << 10) +
          (low - 0xdc00);
    } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
      return -1;
    }
    if (append_utf8(out, capacity, &length, codepoint)) return -1;
  }
  return -1;
}

static int normalize_path(uint64_t address, int wide, int allow_wildcards,
                          char out[PATH_MAX]) {
  char input[PATH_MAX];
  if (wide) {
    if (path_from_wide(address, input, sizeof input)) return -1;
  } else {
    if (!address) return -1;
    const char *source = (const char *)(uintptr_t)address;
    size_t i = 0;
    while (i + 1 < sizeof input && source[i]) {
      input[i] = source[i];
      i++;
    }
    if (source[i]) return -1;
    input[i] = 0;
  }

  for (size_t i = 0; input[i]; i++)
    if (input[i] == '\\') input[i] = '/';
  char *path = input;
  if (!strncmp(path, "//?/", 4)) path += 4;
  size_t root_length = strlen(game_root_path);
  if (path[0] == '/' && root_length &&
      !strncmp(path, game_root_path, root_length) &&
      (path[root_length] == '/' || path[root_length] == 0))
    path += root_length;
  if (((path[0] >= 'A' && path[0] <= 'Z') ||
       (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':')
    path += 2;

  size_t output_length = 0;
  char *last_component = strrchr(path, '/');
  last_component = last_component ? last_component + 1 : path;
  out[0] = 0;
  while (*path == '/') path++;
  while (*path) {
    char *start = path;
    while (*path && *path != '/') path++;
    size_t component_length = (size_t)(path - start);
    while (*path == '/') path++;
    if (!component_length ||
        (component_length == 1 && start[0] == '.'))
      continue;
    if (component_length == 2 && start[0] == '.' && start[1] == '.') {
      if (!output_length) return -1;
      while (output_length && out[output_length - 1] != '/') output_length--;
      if (output_length) output_length--;
      out[output_length] = 0;
      continue;
    }
    for (size_t i = 0; i < component_length; i++) {
      unsigned char ch = (unsigned char)start[i];
      if (ch < 32 || ch == ':' ||
          ((!allow_wildcards || start < last_component) &&
              (ch == '*' || ch == '?')) ||
          ch == '"' || ch == '<' || ch == '>' || ch == '|')
        return -1;
    }
    size_t extra = component_length + (output_length ? 1 : 0);
    if (extra >= PATH_MAX - output_length) return -1;
    if (output_length) out[output_length++] = '/';
    memcpy(out + output_length, start, component_length);
    output_length += component_length;
    out[output_length] = 0;
  }
  return 0;
}

static int find_case_entry(int directory_fd, const char *name,
                           char resolved[NAME_MAX + 1]) {
  int scan_fd = openat(directory_fd, ".",
      O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (scan_fd < 0) return -1;
  DIR *directory = fdopendir(scan_fd);
  if (!directory) {
    int saved_error = errno;
    close(scan_fd);
    errno = saved_error;
    return -1;
  }
  struct dirent *entry;
  errno = 0;
  while ((entry = readdir(directory)) != NULL) {
    if (!strcasecmp(entry->d_name, name)) {
      size_t length = strlen(entry->d_name);
      if (length > NAME_MAX) {
        closedir(directory);
        errno = ENAMETOOLONG;
        return -1;
      }
      memcpy(resolved, entry->d_name, length + 1);
      closedir(directory);
      return 1;
    }
  }
  int saved_error = errno;
  closedir(directory);
  if (saved_error) {
    errno = saved_error;
    return -1;
  }
  return 0;
}

static int open_parent(const char *path, int *parent_fd,
                       char leaf[NAME_MAX + 1]) {
  if (game_root_fd < 0) {
    errno = EACCES;
    return -1;
  }
  if (!path[0] || !strcmp(path, ".")) {
    int copy = dup(game_root_fd);
    if (copy < 0) return -1;
    strcpy(leaf, ".");
    *parent_fd = copy;
    return 0;
  }
  char temporary[PATH_MAX];
  if (strlen(path) >= sizeof temporary) {
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(temporary, path);
  char *last_separator = strrchr(temporary, '/');
  char *name = last_separator ? last_separator + 1 : temporary;
  size_t name_length = strlen(name);
  if (!name_length || name_length > NAME_MAX) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(leaf, name, name_length + 1);
  int current = dup(game_root_fd);
  if (current < 0) return -1;
  if (last_separator) {
    *last_separator = 0;
    char *save = NULL;
    for (char *part = strtok_r(temporary, "/", &save); part;
         part = strtok_r(NULL, "/", &save)) {
      int next = openat(current, part,
          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0 && errno == ENOENT) {
        char resolved[NAME_MAX + 1];
        int match = find_case_entry(current, part, resolved);
        if (match < 0) {
          int saved_error = errno;
          close(current);
          errno = saved_error;
          return -1;
        }
        if (match)
          next = openat(current, resolved,
              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        else
          errno = ENOENT;
        if (next < 0 && errno != ENOENT) {
          close(current);
          return -1;
        }
      }
      if (next < 0) {
        int saved_error = errno;
        close(current);
        errno = saved_error;
        return -1;
      }
      close(current);
      current = next;
    }
  }
  struct stat existing;
  if (fstatat(current, leaf, &existing, AT_SYMLINK_NOFOLLOW) &&
      errno == ENOENT) {
    char resolved[NAME_MAX + 1];
    int match = find_case_entry(current, leaf, resolved);
    if (match < 0) {
      int saved_error = errno;
      close(current);
      errno = saved_error;
      return -1;
    }
    if (match) memcpy(leaf, resolved, strlen(resolved) + 1);
  }
  *parent_fd = current;
  return 0;
}

static int open_relative(const char *path, int flags, mode_t mode) {
  int parent_fd;
  char leaf[NAME_MAX + 1];
  if (open_parent(path, &parent_fd, leaf)) return -1;
  int fd = openat(parent_fd, leaf, flags | O_NOFOLLOW | O_CLOEXEC, mode);
  int saved_error = errno;
  close(parent_fd);
  errno = saved_error;
  return fd;
}

static int stat_relative(const char *path, struct stat *info) {
  int parent_fd;
  char leaf[NAME_MAX + 1];
  if (open_parent(path, &parent_fd, leaf)) return -1;
  int result = fstatat(parent_fd, leaf, info, AT_SYMLINK_NOFOLLOW);
  int saved_error = errno;
  close(parent_fd);
  errno = saved_error;
  return result;
}

static uint64_t make_handle(size_t index) {
  FileHandle *handle = &handles[index];
  if (++handle->generation == 0) handle->generation = 1;
  handle->open = 1;
  return HANDLE_BASE | ((uint64_t)handle->generation << 32) |
      (uint64_t)(index + 1);
}

static FileHandle *get_handle(uint64_t value) {
  if ((value & HANDLE_MASK) != HANDLE_BASE) return NULL;
  uint32_t slot = (uint32_t)(value & 0xffffu);
  uint16_t generation = (uint16_t)(value >> 32);
  if (!slot || slot > FILE_HANDLE_CAPACITY) return NULL;
  FileHandle *handle = &handles[slot - 1];
  return handle->open && handle->generation == generation ? handle : NULL;
}

static MappingHandle *get_mapping_handle(uint64_t value) {
  if ((value & HANDLE_MASK) != MAPPING_HANDLE_BASE) return NULL;
  uint32_t slot = (uint32_t)(value & 0xffffu);
  uint16_t generation = (uint16_t)(value >> 32);
  if (!slot || slot > MAPPING_HANDLE_CAPACITY) return NULL;
  MappingHandle *handle = &mapping_handles[slot - 1];
  return handle->open && handle->generation == generation ? handle : NULL;
}

static uint64_t install_mapping_handle(int fd, uint64_t size,
                                       uint8_t protection, int anonymous) {
  for (size_t i = 0; i < MAPPING_HANDLE_CAPACITY; i++) {
    MappingHandle *handle = &mapping_handles[i];
    if (handle->open) continue;
    if (++handle->generation == 0) handle->generation = 1;
    handle->fd = fd;
    handle->size = size;
    handle->protection = protection;
    handle->anonymous = (uint8_t)anonymous;
    handle->open = 1;
    return MAPPING_HANDLE_BASE | ((uint64_t)handle->generation << 32) |
        (uint64_t)(i + 1);
  }
  if (fd >= 0) close(fd);
  aot_last_error = ERROR_NOT_ENOUGH_MEMORY;
  return UINT64_MAX;
}

static int size_fits_size_t(uint64_t size) {
  return (uint64_t)(size_t)size == size;
}

static MappingView *reserve_mapping_view(void) {
  for (size_t i = 0; i < MAPPING_VIEW_CAPACITY; i++)
    if (!mapping_views[i].open) return &mapping_views[i];
  aot_last_error = ERROR_NOT_ENOUGH_MEMORY;
  return NULL;
}

static uint64_t install_handle(int fd) {
  for (size_t i = 0; i < FILE_HANDLE_CAPACITY; i++) {
    if (!handles[i].open) {
      handles[i].fd = fd;
      return make_handle(i);
    }
  }
  close(fd);
  aot_last_error = ERROR_NOT_ENOUGH_MEMORY;
  return UINT64_MAX;
}

static int utf8_to_utf16(const char *source, uint16_t *output,
                         size_t capacity, size_t *written) {
  size_t count = 0;
  for (size_t i = 0; source[i];) {
    uint8_t first = (uint8_t)source[i++];
    uint32_t codepoint;
    size_t continuation;
    if (first < 0x80) {
      codepoint = first;
      continuation = 0;
    } else if (first >= 0xc2 && first <= 0xdf) {
      codepoint = first & 0x1f;
      continuation = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      codepoint = first & 0x0f;
      continuation = 2;
    } else if (first >= 0xf0 && first <= 0xf4) {
      codepoint = first & 0x07;
      continuation = 3;
    } else {
      return -1;
    }
    for (size_t j = 0; j < continuation; j++) {
      uint8_t next = (uint8_t)source[i++];
      if (!next || (next & 0xc0) != 0x80) return -1;
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if ((continuation == 1 && codepoint < 0x80) ||
        (continuation == 2 && codepoint < 0x800) ||
        (continuation == 3 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
        codepoint > 0x10ffff)
      return -1;
    size_t needed = codepoint <= 0xffff ? 1 : 2;
    if (output) {
      if (count + needed >= capacity) return -1;
      if (needed == 1) {
        output[count] = (uint16_t)codepoint;
      } else {
        codepoint -= 0x10000;
        output[count] = (uint16_t)(0xd800 + (codepoint >> 10));
        output[count + 1] = (uint16_t)(0xdc00 + (codepoint & 0x3ff));
      }
    }
    count += needed;
  }
  if (output) output[count] = 0;
  *written = count;
  return 0;
}

static uint64_t get_directory_name(uint64_t buffer_address,
                                   uint32_t buffer_size, int wide) {
  /* PE32 guests expect a Windows path with a drive letter (they match it against GetLogicalDriveStrings); normalize_path strips "C:" */
  const char *root = aot_is_pe32 ? "C:\\" : game_root_path;
  if (game_root_fd < 0 || !game_root_path[0]) {
    aot_last_error = ERROR_PATH_NOT_FOUND;
    return 0;
  }
  if (!wide) {
    size_t required = strlen(root) + 1;
    if (!buffer_size) {
      aot_last_error = 0;
      return required;
    }
    if (!buffer_address) {
      aot_last_error = ERROR_INVALID_PARAMETER;
      return 0;
    }
    if (buffer_size < required) {
      aot_last_error = ERROR_INSUFFICIENT_BUFFER;
      return required;
    }
    memcpy((void *)(uintptr_t)buffer_address, root, required);
    aot_last_error = 0;
    return required - 1;
  }
  size_t units;
  if (utf8_to_utf16(root, NULL, 0, &units)) {
    aot_last_error = ERROR_INVALID_DATA;
    return 0;
  }
  size_t required = units + 1;
  if (!buffer_size) {
    aot_last_error = 0;
    return required;
  }
  if (!buffer_address) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  if (buffer_size < required) {
    aot_last_error = ERROR_INSUFFICIENT_BUFFER;
    return required;
  }
  if (utf8_to_utf16(root, (uint16_t *)(uintptr_t)buffer_address,
      buffer_size, &units)) {
    aot_last_error = ERROR_INVALID_DATA;
    return 0;
  }
  aot_last_error = 0;
  return units;
}

static uint64_t get_module_filename(uint64_t buffer_address,
                                    uint32_t buffer_size, int wide) {
  char vexe[PATH_MAX]; const char *exe = g_exe_path;
  if (aot_is_pe32 && g_exe_path) { const char *b = strrchr(g_exe_path, '/'); snprintf(vexe, sizeof vexe, "C:\\%s", b ? b + 1 : g_exe_path); exe = vexe; }
  if (!exe || !buffer_size || !buffer_address) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  if (!wide) {
    size_t length = strlen(exe);
    size_t copied = length < buffer_size - 1 ? length : buffer_size - 1;
    memcpy((void *)(uintptr_t)buffer_address, exe, copied);
    ((char *)(uintptr_t)buffer_address)[copied] = 0;
    if (copied != length) aot_last_error = ERROR_INSUFFICIENT_BUFFER;
    else aot_last_error = 0;
    return copied == length ? length : buffer_size;
  }
  size_t units;
  if (utf8_to_utf16(exe, NULL, 0, &units)) {
    aot_last_error = ERROR_INVALID_DATA;
    return 0;
  }
  if (units + 1 > buffer_size) {
    uint16_t *temporary = malloc((units + 1) * sizeof *temporary);
    if (!temporary) {
      aot_last_error = ERROR_NOT_ENOUGH_MEMORY;
      return 0;
    }
    size_t converted;
    if (utf8_to_utf16(exe, temporary, units + 1, &converted)) {
      free(temporary);
      aot_last_error = ERROR_INVALID_DATA;
      return 0;
    }
    size_t copied = buffer_size - 1;
    if (copied && temporary[copied - 1] >= 0xd800 &&
        temporary[copied - 1] <= 0xdbff)
      copied--;
    memcpy((void *)(uintptr_t)buffer_address, temporary,
        copied * sizeof *temporary);
    ((uint16_t *)(uintptr_t)buffer_address)[copied] = 0;
    free(temporary);
    aot_last_error = ERROR_INSUFFICIENT_BUFFER;
    return buffer_size;
  }
  if (utf8_to_utf16(exe, (uint16_t *)(uintptr_t)buffer_address,
      buffer_size, &units)) {
    aot_last_error = ERROR_INVALID_DATA;
    return 0;
  }
  aot_last_error = 0;
  return units;
}

void aot_win_file_init(void) {
  if (game_root_fd >= 0) return;
  if (!getcwd(game_root_path, sizeof game_root_path)) {
    aot_last_error = (uint32_t)win_error_from_errno(errno);
    fprintf(stderr, "[winfile] cannot resolve game working directory: %s\n",
        strerror(errno));
    return;
  }
  game_root_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (game_root_fd < 0) {
    aot_last_error = (uint32_t)win_error_from_errno(errno);
    fprintf(stderr, "[winfile] cannot open game working directory: %s\n",
        strerror(errno));
  }
}

SH(GetLastError) { (void)c; return aot_last_error; }
SH(SetLastError) { aot_last_error = (uint32_t)ARG(0); return 0; }
SH(GetCurrentDirectoryA) {
  return get_directory_name(ARG(1), (uint32_t)ARG(0), 0);
}
SH(GetCurrentDirectoryW) {
  return get_directory_name(ARG(1), (uint32_t)ARG(0), 1);
}
SH(GetModuleFileNameA) {
  return get_module_filename(ARG(1), (uint32_t)ARG(2), 0);
}
SH(GetModuleFileNameW) {
  return get_module_filename(ARG(1), (uint32_t)ARG(2), 1);
}

SH(CreateFileA) {
  char path[PATH_MAX];
  if (normalize_path(ARG(0), 0, 0, path)) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return UINT64_MAX;
  }
  uint32_t access = (uint32_t)ARG(1);
  uint32_t disposition = (uint32_t)ARG(4);
  uint32_t flags = (uint32_t)ARG(5);
  if (disposition < 1 || disposition > 5) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return UINT64_MAX;
  }
  int read_access = (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA |
      FILE_READ_ATTRIBUTES)) != 0;
  int write_access = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA |
      FILE_APPEND_DATA)) != 0;
  if (!read_access && !write_access) read_access = 1;
  if (!write_access && (disposition == 2 || disposition == 5)) {
    aot_last_error = ERROR_ACCESS_DENIED;
    return UINT64_MAX;
  }
  int open_flags = read_access && write_access ? O_RDWR :
      write_access ? O_WRONLY : O_RDONLY;
  if (access & FILE_APPEND_DATA) open_flags |= O_APPEND;
  switch (disposition) {
    case 1: open_flags |= O_CREAT | O_EXCL; break;
    case 2: open_flags |= O_CREAT | O_TRUNC; break;
    case 3: break;
    case 4: open_flags |= O_CREAT; break;
    case 5: open_flags |= O_TRUNC; break;
  }
  if (flags & FILE_FLAG_BACKUP_SEMANTICS) open_flags |= O_DIRECTORY;
  struct stat prior_info;
  int existed = stat_relative(path, &prior_info) == 0;
  int fd = open_relative(path, open_flags, 0666);
  if (fd < 0) {
    fail_errno();
    return UINT64_MAX;
  }
  struct stat opened_info;
  if (fstat(fd, &opened_info)) {
    int saved_error = errno;
    close(fd);
    errno = saved_error;
    fail_errno();
    return UINT64_MAX;
  }
  if (S_ISDIR(opened_info.st_mode) && !(flags & FILE_FLAG_BACKUP_SEMANTICS)) {
    close(fd);
    aot_last_error = ERROR_ACCESS_DENIED;
    return UINT64_MAX;
  }
  uint64_t handle = install_handle(fd);
  if (handle == UINT64_MAX) return handle;
  FileHandle *installed = get_handle(handle);
  installed->readable = (uint8_t)read_access;
  installed->writable = (uint8_t)write_access;
  installed->overlapped = (uint8_t)((flags & FILE_FLAG_OVERLAPPED) != 0);
  aot_last_error = existed && (disposition == 2 || disposition == 4) ?
      ERROR_ALREADY_EXISTS : 0;
  return handle;
}

SH(CreateFileW) {
  char path[PATH_MAX];
  if (normalize_path(ARG(0), 1, 0, path)) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return UINT64_MAX;
  }
  CPU copy = *c;
  copy.r[1] = (uint64_t)(uintptr_t)path;
  return sh_CreateFileA(&copy);
}

static int standard_fd(uint64_t handle, int writing) {
  if (handle == 0x10) return writing ? -1 : 0;
  if (handle == 0x11) return writing ? 1 : -1;
  if (handle == 0x12) return writing ? 2 : -1;
  return -1;
}

SH(ReadFile) {
  uint64_t handle_value = ARG(0);
  uint32_t requested = (uint32_t)ARG(2);
  uint64_t bytes_address = ARG(3);
  if (bytes_address) ST(uint32_t, bytes_address, 0);
  if (ARG(4))
    return aot_file_async_ReadFile(c);
  if (!bytes_address || (requested && !ARG(1))) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  int fd = standard_fd(handle_value, 0);
  FileHandle *handle = get_handle(handle_value);
  if (fd < 0) {
    if (!handle) {
      aot_last_error = ERROR_INVALID_HANDLE;
      return 0;
    }
    if (handle->overlapped) {
      aot_last_error = ERROR_INVALID_PARAMETER;
      return 0;
    }
    fd = handle->fd;
  }
  ssize_t result;
  do {
    result = read(fd, (void *)(uintptr_t)ARG(1), requested);
  } while (result < 0 && errno == EINTR);
  if (result < 0) return fail_errno();
  ST(uint32_t, bytes_address, (uint32_t)result);
  aot_last_error = 0;
  return 1;
}

SH(WriteFile) {
  uint64_t handle_value = ARG(0);
  uint32_t requested = (uint32_t)ARG(2);
  uint64_t bytes_address = ARG(3);
  if (bytes_address) ST(uint32_t, bytes_address, 0);
  if (ARG(4))
    return aot_file_async_WriteFile(c);
  if (!bytes_address || (requested && !ARG(1))) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  int fd = standard_fd(handle_value, 1);
  FileHandle *handle = get_handle(handle_value);
  if (fd < 0) {
    if (!handle) {
      aot_last_error = ERROR_INVALID_HANDLE;
      return 0;
    }
    if (handle->overlapped) {
      aot_last_error = ERROR_INVALID_PARAMETER;
      return 0;
    }
    fd = handle->fd;
  }
  if (handle_value != 0x10) fflush(stdout);
  ssize_t result;
  do {
    result = write(fd, (const void *)(uintptr_t)ARG(1), requested);
  } while (result < 0 && errno == EINTR);
  if (result < 0) return fail_errno();
  ST(uint32_t, bytes_address, (uint32_t)result);
  aot_last_error = 0;
  return 1;
}

SH(GetFileType) {
  uint64_t value = ARG(0);
  if (value == 0x10 || value == 0x11 || value == 0x12) return FILE_TYPE_CHAR;
  FileHandle *handle = get_handle(value);
  if (!handle) {
    aot_last_error = ERROR_INVALID_HANDLE;
    return 0;
  }
  aot_last_error = 0;
  return FILE_TYPE_DISK;
}

SH(CloseHandle) {
  uint64_t value = ARG(0);
  int async_result = aot_win_async_close_handle(value);
  if (async_result) return async_result > 0;
  MappingHandle *mapping = get_mapping_handle(value);
  if (mapping) {
    int fd = mapping->fd;
    mapping->open = 0;
    mapping->fd = -1;
    if (fd >= 0 && close(fd)) return fail_errno();
    aot_last_error = 0;
    return 1;
  }
  FileHandle *handle = get_handle(value);
  if (!handle) {
    aot_last_error = ERROR_INVALID_HANDLE;
    return 0;
  }
  int fd = handle->fd;
  handle->open = 0;
  if (close(fd)) return fail_errno();
  aot_last_error = 0;
  return 1;
}

SH(FlushFileBuffers) {
  FileHandle *handle = get_handle(ARG(0));
  if (!handle) {
    aot_last_error = ERROR_INVALID_HANDLE;
    return 0;
  }
  if (fsync(handle->fd)) return fail_errno();
  aot_last_error = 0;
  return 1;
}

static int seek_file(int fd, int64_t distance, uint32_t method,
                     int64_t *new_position) {
  int origin = method == FILE_BEGIN ? SEEK_SET :
      method == FILE_CURRENT ? SEEK_CUR : method == FILE_END ? SEEK_END : -1;
  if (origin < 0) {
    errno = EINVAL;
    return -1;
  }
  off_t position = lseek(fd, (off_t)distance, origin);
  if (position == (off_t)-1) return -1;
  *new_position = (int64_t)position;
  return 0;
}

SH(SetFilePointer) {
  FileHandle *handle = get_handle(ARG(0));
  if (!handle) {
    aot_last_error = ERROR_INVALID_HANDLE;
    return INVALID_FILE_SIZE;
  }
  int64_t distance = (int32_t)ARG(1);
  uint64_t high_address = ARG(3);
  if (high_address) {
    uint64_t bits = ((uint64_t)LD(uint32_t, high_address) << 32) |
        (uint32_t)ARG(1);
    distance = (int64_t)bits;
  }
  int64_t position;
  if (seek_file(handle->fd, distance, (uint32_t)ARG(2), &position)) {
    fail_errno();
    return INVALID_FILE_SIZE;
  }
  if (high_address) ST(int32_t, high_address, (int32_t)(position >> 32));
  aot_last_error = 0;
  return (uint32_t)position;
}

SH(SetFilePointerEx) {
  FileHandle *handle = get_handle(ARG(0));
  if (!handle) {
    aot_last_error = ERROR_INVALID_HANDLE;
    return 0;
  }
  int64_t position;
  if (seek_file(handle->fd, (int64_t)ARG(1), (uint32_t)ARG(3), &position))
    return fail_errno();
  if (ARG(2)) ST(int64_t, ARG(2), position);
  aot_last_error = 0;
  return 1;
}

int aot_win_file_duplicate(uint64_t value, int writing,
                           int require_overlapped) {
  FileHandle *handle = get_handle(value);
  if (!handle) {
    aot_last_error = ERROR_INVALID_HANDLE;
    return -1;
  }
  if ((writing > 0 && !handle->writable) ||
      (writing == 0 && !handle->readable)) {
    aot_last_error = ERROR_ACCESS_DENIED;
    return -1;
  }
  if (require_overlapped && !handle->overlapped) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return -1;
  }
  int fd = dup(handle->fd);
  if (fd < 0) {
    fail_errno();
    return -1;
  }
  return fd;
}

static int stat_handle(uint64_t value, struct stat *info) {
  FileHandle *handle = get_handle(value);
  if (!handle) {
    errno = EBADF;
    return -1;
  }

  return fstat(handle->fd, info);
}

SH(GetFileSize) {
  struct stat info;
  if (stat_handle(ARG(0), &info)) {
    fail_errno();
    return INVALID_FILE_SIZE;
  }
  if (ARG(1)) ST(uint32_t, ARG(1), (uint32_t)((uint64_t)info.st_size >> 32));
  aot_last_error = 0;
  return (uint32_t)info.st_size;
}

SH(GetFileSizeEx) {
  struct stat info;
  if (!ARG(1)) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  if (stat_handle(ARG(0), &info)) return fail_errno();
  ST(int64_t, ARG(1), (int64_t)info.st_size);
  aot_last_error = 0;
  return 1;
}

static int create_anonymous_mapping_fd(void) {
#if defined(SYS_memfd_create)
  return (int)syscall(SYS_memfd_create, "aot-file-mapping", 0x0001u);
#elif defined(__NR_memfd_create)
  return (int)syscall(__NR_memfd_create, "aot-file-mapping", 0x0001u);
#else
  errno = ENOSYS;
  return -1;
#endif
}

SH(CreateFileMappingA) {
  uint64_t file_value = ARG(0);
  uint32_t protection = (uint32_t)ARG(2);
  uint64_t size = ((uint64_t)(uint32_t)ARG(3) << 32) |
      (uint32_t)ARG(4);
  uint64_t name_address = ARG(5);
  uint32_t page_protection = protection & 0xffu;
  int anonymous = file_value == UINT64_MAX;
  if (name_address || (protection & ~0x080000ffu) ||
      (page_protection != PAGE_READONLY &&
       page_protection != PAGE_READWRITE &&
       page_protection != PAGE_WRITECOPY)) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  FileHandle *file = NULL;
  if (!anonymous) {
    file = get_handle(file_value);
    if (!file) {
      aot_last_error = ERROR_INVALID_HANDLE;
      return 0;
    }
    if (page_protection == PAGE_READWRITE && !file->writable) {
      aot_last_error = ERROR_ACCESS_DENIED;
      return 0;
    }
    if (page_protection != PAGE_READWRITE && !file->readable) {
      aot_last_error = ERROR_ACCESS_DENIED;
      return 0;
    }
  }
  if (anonymous && !size) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  if (size > (uint64_t)INT64_MAX) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  int mapping_fd = -1;
  if (anonymous) {
    mapping_fd = create_anonymous_mapping_fd();
    if (mapping_fd < 0) {
      fail_errno();
      return 0;
    }
    if (ftruncate(mapping_fd, (off_t)size)) {
      int saved_error = errno;
      close(mapping_fd);
      errno = saved_error;
      fail_errno();
      return 0;
    }
  } else {
    struct stat info;
    if (fstat(file->fd, &info)) {
      fail_errno();
      return 0;
    }
    if (info.st_size < 0) {
      aot_last_error = ERROR_INVALID_DATA;
      return 0;
    }
    if (!size) size = (uint64_t)info.st_size;
    if (!size) {
      aot_last_error = ERROR_INVALID_PARAMETER;
      return 0;
    }
    if (size > (uint64_t)info.st_size) {
      if (page_protection != PAGE_READWRITE || !file->writable) {
        aot_last_error = ERROR_ACCESS_DENIED;
        return 0;
      }
      if (ftruncate(file->fd, (off_t)size)) {
        fail_errno();
        return 0;
      }
    }
    mapping_fd = dup(file->fd);
    if (mapping_fd < 0) {
      fail_errno();
      return 0;
    }
    int descriptor_flags = fcntl(mapping_fd, F_GETFD);
    if (descriptor_flags >= 0)
      fcntl(mapping_fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
  }
  aot_last_error = 0;
  return install_mapping_handle(mapping_fd, size, (uint8_t)page_protection,
      anonymous);
}

SH(CreateFileMappingW) {
  if (ARG(5)) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  return sh_CreateFileMappingA(c);
}

static int mapping_access(uint32_t requested, uint8_t protection,
                          int *map_protection, int *map_flags) {
  if (!requested) requested = FILE_MAP_ALL_ACCESS;
  if (requested & FILE_MAP_EXECUTE) return -1;
  if (requested == FILE_MAP_COPY) {
    if (protection != PAGE_WRITECOPY && protection != PAGE_READWRITE)
      return -1;
    *map_protection = PROT_READ | PROT_WRITE;
    *map_flags = MAP_PRIVATE;
    return 0;
  }
  if (requested == FILE_MAP_READ) {
    if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
        protection != PAGE_WRITECOPY)
      return -1;
    *map_protection = PROT_READ;
    *map_flags = protection == PAGE_WRITECOPY ? MAP_PRIVATE : MAP_SHARED;
    return 0;
  }
  if (requested == FILE_MAP_WRITE ||
      requested == (FILE_MAP_READ | FILE_MAP_WRITE)) {
    if (protection != PAGE_READWRITE) return -1;
    *map_protection = PROT_READ | PROT_WRITE;
    *map_flags = MAP_SHARED;
    return 0;
  }
  if (requested == FILE_MAP_ALL_ACCESS) {
    if (protection != PAGE_READWRITE) return -1;
    *map_protection = PROT_READ | PROT_WRITE;
    *map_flags = MAP_SHARED;
    return 0;
  }
  return -1;
}

static uint64_t map_view(uint64_t mapping_value, uint32_t access,
                         uint64_t offset, uint64_t requested_size,
                         uint64_t requested_address) {
  MappingHandle *mapping = get_mapping_handle(mapping_value);
  if (!mapping) {
    aot_last_error = ERROR_INVALID_HANDLE;
    return 0;
  }
  if (requested_address) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0 || (offset & 0xffffu) ||
      offset > mapping->size || offset > (uint64_t)INT64_MAX) {
    aot_last_error = (offset & 0xffffu) ? ERROR_MAPPED_ALIGNMENT :
        ERROR_INVALID_PARAMETER;
    return 0;
  }
  uint64_t view_size = requested_size ?
      requested_size : mapping->size - offset;
  if (!view_size || view_size > mapping->size - offset ||
      !size_fits_size_t(view_size)) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  int protection, flags;
  if (mapping_access(access, mapping->protection, &protection, &flags)) {
    aot_last_error = ERROR_ACCESS_DENIED;
    return 0;
  }
  MappingView *view = reserve_mapping_view();
  if (!view) return 0;
  void *base = mmap(NULL, (size_t)view_size, protection, flags,
      mapping->fd, (off_t)offset);
  if (base == MAP_FAILED) return fail_errno();
  view->base = base;
  view->mapped_size = (size_t)view_size;
  view->returned_address = base;
  view->mapping_value = mapping_value;
  view->open = 1;
  aot_last_error = 0;
  return (uint64_t)(uintptr_t)base;
}

SH(MapViewOfFile) {
  uint64_t offset = ((uint64_t)(uint32_t)ARG(2) << 32) |
      (uint32_t)ARG(3);
  return map_view(ARG(0), (uint32_t)ARG(1), offset, ARG(4), 0);
}
SH(MapViewOfFileEx) {
  uint64_t offset = ((uint64_t)(uint32_t)ARG(2) << 32) |
      (uint32_t)ARG(3);
  return map_view(ARG(0), (uint32_t)ARG(1), offset, ARG(4), ARG(5));
}

SH(UnmapViewOfFile) {
  uintptr_t address = (uintptr_t)ARG(0);
  for (size_t i = 0; i < MAPPING_VIEW_CAPACITY; i++) {
    MappingView *view = &mapping_views[i];
    if (!view->open || view->returned_address != (void *)address) continue;
    if (munmap(view->base, view->mapped_size)) return fail_errno();
    memset(view, 0, sizeof *view);
    aot_last_error = 0;
    return 1;
  }
  aot_last_error = ERROR_INVALID_PARAMETER;
  return 0;
}

SH(FlushViewOfFile) {
  uintptr_t address = (uintptr_t)ARG(0);
  uint64_t requested_size = ARG(1);
  for (size_t i = 0; i < MAPPING_VIEW_CAPACITY; i++) {
    MappingView *view = &mapping_views[i];
    uintptr_t base = (uintptr_t)view->base;
    if (!view->open || address < base ||
        address - base >= view->mapped_size)
      continue;
    size_t offset = address - base;
    size_t available = view->mapped_size - offset;
    if (requested_size && (!size_fits_size_t(requested_size) ||
        requested_size > available)) {
      aot_last_error = ERROR_INVALID_PARAMETER;
      return 0;
    }
    size_t flush_size = requested_size ? (size_t)requested_size : available;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
      aot_last_error = ERROR_INVALID_DATA;
      return 0;
    }
    size_t aligned_offset = offset - offset % (size_t)page_size;
    size_t sync_size = offset - aligned_offset + flush_size;
    if (msync((char *)view->base + aligned_offset, sync_size, MS_SYNC))
      return fail_errno();
    aot_last_error = 0;
    return 1;
  }
  aot_last_error = ERROR_INVALID_PARAMETER;
  return 0;
}

static uint32_t file_attributes(const struct stat *info, const char *path) {
  uint32_t attributes = 0;
  if (S_ISDIR(info->st_mode)) attributes |= FILE_ATTRIBUTE_DIRECTORY;
  if (!(info->st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)))
    attributes |= FILE_ATTRIBUTE_READONLY;
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  if (name[0] == '.') attributes |= FILE_ATTRIBUTE_HIDDEN;
  if (S_ISREG(info->st_mode)) attributes |= FILE_ATTRIBUTE_ARCHIVE;
  if (!attributes) attributes = FILE_ATTRIBUTE_NORMAL;
  return attributes;
}

static void unix_time_to_filetime(time_t seconds, long nanoseconds,
                                  uint64_t *filetime) {
  const uint64_t epoch_delta = 11644473600ULL;
  if (seconds < -(int64_t)epoch_delta) {
    *filetime = 0;
    return;
  }
  *filetime = ((uint64_t)((int64_t)seconds + (int64_t)epoch_delta) *
      10000000ULL) + (uint64_t)(nanoseconds / 100);
}

static uint32_t file_attributes(const struct stat *info, const char *path);

static int wildcard_match(const char *pattern, const char *name) {
  if (!strcmp(pattern, "*.*")) pattern = "*";
  const char *star = NULL;
  const char *retry = NULL;
  while (*name) {
    if (*pattern == '?') {
      pattern++;
      name++;
    } else if (*pattern && *pattern != '*' &&
        tolower((unsigned char)*pattern) == tolower((unsigned char)*name)) {
      pattern++;
      name++;
    } else if (*pattern == '*') {
      star = pattern++;
      retry = name;
    } else if (star) {
      pattern = star + 1;
      name = ++retry;
    } else {
      return 0;
    }
  }
  while (*pattern == '*') pattern++;
  return *pattern == 0;
}

static void fill_find_data(uint64_t destination, int wide,
                           const struct dirent *entry,
                           const struct stat *info) {
  size_t size = wide ? FIND_DATA_W_SIZE : FIND_DATA_A_SIZE;
  memset((void *)(uintptr_t)destination, 0, size);
  uint64_t creation_time, access_time, write_time;
#if defined(__APPLE__)
  unix_time_to_filetime(info->st_ctimespec.tv_sec, info->st_ctimespec.tv_nsec,
      &creation_time);
  unix_time_to_filetime(info->st_atimespec.tv_sec, info->st_atimespec.tv_nsec,
      &access_time);
  unix_time_to_filetime(info->st_mtimespec.tv_sec, info->st_mtimespec.tv_nsec,
      &write_time);
#else
  unix_time_to_filetime(info->st_ctim.tv_sec, info->st_ctim.tv_nsec,
      &creation_time);
  unix_time_to_filetime(info->st_atim.tv_sec, info->st_atim.tv_nsec,
      &access_time);
  unix_time_to_filetime(info->st_mtim.tv_sec, info->st_mtim.tv_nsec,
      &write_time);
#endif
  ST(uint32_t, destination, file_attributes(info, entry->d_name));
  ST(uint64_t, destination + 4, creation_time);
  ST(uint64_t, destination + 12, access_time);
  ST(uint64_t, destination + 20, write_time);
  ST(uint32_t, destination + 28, (uint32_t)((uint64_t)info->st_size >> 32));
  ST(uint32_t, destination + 32, (uint32_t)info->st_size);
  if (wide) {
    size_t units;
    if (utf8_to_utf16(entry->d_name, (uint16_t *)(uintptr_t)(destination + 44),
        260, &units))
      ST(uint16_t, destination + 44, '?');
  } else {
    size_t length = strlen(entry->d_name);
    if (length > 259) length = 259;
    memcpy((void *)(uintptr_t)(destination + 44), entry->d_name, length);
  }
}

static FindHandle *get_find_handle(uint64_t value) {
  if ((value & HANDLE_MASK) != FIND_HANDLE_BASE) return NULL;
  uint32_t slot = (uint32_t)(value & 0xffffu);
  uint16_t generation = (uint16_t)(value >> 32);
  if (!slot || slot > FIND_HANDLE_CAPACITY) return NULL;
  FindHandle *handle = &find_handles[slot - 1];
  return handle->open && handle->generation == generation ? handle : NULL;
}

static uint64_t install_find_handle(DIR *directory, const char *pattern,
                                    int wide) {
  for (size_t i = 0; i < FIND_HANDLE_CAPACITY; i++) {
    FindHandle *handle = &find_handles[i];
    if (handle->open) continue;
    size_t length = strlen(pattern);
    if (length > NAME_MAX) {
      closedir(directory);
      aot_last_error = ERROR_FILENAME_EXCED_RANGE;
      return UINT64_MAX;
    }
    if (++handle->generation == 0) handle->generation = 1;
    memcpy(handle->pattern, pattern, length + 1);
    handle->directory = directory;
    handle->wide = (uint8_t)wide;
    handle->open = 1;
    return FIND_HANDLE_BASE | ((uint64_t)handle->generation << 32) |
        (uint64_t)(i + 1);
  }
  closedir(directory);
  aot_last_error = ERROR_NOT_ENOUGH_MEMORY;
  return UINT64_MAX;
}

static int next_find_entry(FindHandle *handle, uint64_t destination,
                           int wide) {
  if (!destination) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(handle->directory);
    if (!entry) {
      if (errno) fail_errno();
      else aot_last_error = ERROR_NO_MORE_FILES;
      return 0;
    }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
        !wildcard_match(handle->pattern, entry->d_name))
      continue;
    struct stat info;
    if (fstatat(dirfd(handle->directory), entry->d_name, &info,
        AT_SYMLINK_NOFOLLOW))
      continue;
    if (S_ISLNK(info.st_mode)) continue;
    fill_find_data(destination, wide, entry, &info);
    aot_last_error = 0;
    return 1;
  }
}

static uint64_t find_first(uint64_t path_address, uint64_t data_address,
                           int wide) {
  char path[PATH_MAX];
  if (!data_address || normalize_path(path_address, wide, 1, path)) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return UINT64_MAX;
  }
  int parent_fd;
  char pattern[NAME_MAX + 1];
  if (open_parent(path, &parent_fd, pattern)) {
    fail_errno();
    return UINT64_MAX;
  }
  DIR *directory = fdopendir(parent_fd);
  if (!directory) {
    int saved_error = errno;
    close(parent_fd);
    errno = saved_error;
    fail_errno();
    return UINT64_MAX;
  }
  uint64_t handle_value = install_find_handle(directory, pattern, wide);
  if (handle_value == UINT64_MAX) return handle_value;
  FindHandle *handle = get_find_handle(handle_value);
  if (!next_find_entry(handle, data_address, wide)) {
    uint32_t error = aot_last_error;
    closedir(handle->directory);
    handle->directory = NULL;
    handle->open = 0;
    aot_last_error = error == ERROR_NO_MORE_FILES ?
        ERROR_FILE_NOT_FOUND : error;
    return UINT64_MAX;
  }
  return handle_value;
}

SH(FindFirstFileA) { return find_first(ARG(0), ARG(1), 0); }
SH(FindFirstFileW) { return find_first(ARG(0), ARG(1), 1); }
SH(FindNextFileA) {
  FindHandle *handle = get_find_handle(ARG(0));
  if (!handle) {
    aot_last_error = ERROR_INVALID_HANDLE;
    return 0;
  }
  return next_find_entry(handle, ARG(1), 0);
}
SH(FindNextFileW) {
  FindHandle *handle = get_find_handle(ARG(0));
  if (!handle) {
    aot_last_error = ERROR_INVALID_HANDLE;
    return 0;
  }
  return next_find_entry(handle, ARG(1), 1);
}
SH(FindClose) {
  FindHandle *handle = get_find_handle(ARG(0));
  if (!handle) {
    aot_last_error = ERROR_INVALID_HANDLE;
    return 0;
  }
  DIR *directory = handle->directory;
  handle->directory = NULL;
  handle->open = 0;
  if (closedir(directory)) return fail_errno();
  aot_last_error = 0;
  return 1;
}

static int get_path_attributes(uint64_t path_address, int wide,
                               struct stat *info, char path[PATH_MAX]) {
  if (normalize_path(path_address, wide, 0, path)) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return -1;
  }
  if (stat_relative(path, info)) {
    aot_last_error = (uint32_t)win_error_from_errno(errno);
    return -1;
  }
  if (S_ISLNK(info->st_mode)) {
    aot_last_error = ERROR_ACCESS_DENIED;
    return -1;
  }
  return 0;
}

SH(GetFileAttributesA) {
  struct stat info;
  char path[PATH_MAX];
  if (get_path_attributes(ARG(0), 0, &info, path))
    return INVALID_FILE_ATTRIBUTES;
  aot_last_error = 0;
  return file_attributes(&info, path);
}

SH(GetFileAttributesW) {
  struct stat info;
  char path[PATH_MAX];
  if (get_path_attributes(ARG(0), 1, &info, path))
    return INVALID_FILE_ATTRIBUTES;
  aot_last_error = 0;
  return file_attributes(&info, path);
}

static uint64_t get_attributes_ex(uint64_t path_address, int wide,
                                 uint32_t level, uint64_t data_address) {
  if (level != 0 || !data_address) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  struct stat info;
  char path[PATH_MAX];
  if (get_path_attributes(path_address, wide, &info, path)) return 0;
  uint64_t creation_time, access_time, write_time;
#if defined(__APPLE__)
  unix_time_to_filetime(info.st_ctimespec.tv_sec, info.st_ctimespec.tv_nsec,
      &creation_time);
  unix_time_to_filetime(info.st_atimespec.tv_sec, info.st_atimespec.tv_nsec,
      &access_time);
  unix_time_to_filetime(info.st_mtimespec.tv_sec, info.st_mtimespec.tv_nsec,
      &write_time);
#else
  unix_time_to_filetime(info.st_ctim.tv_sec, info.st_ctim.tv_nsec,
      &creation_time);
  unix_time_to_filetime(info.st_atim.tv_sec, info.st_atim.tv_nsec,
      &access_time);
  unix_time_to_filetime(info.st_mtim.tv_sec, info.st_mtim.tv_nsec,
      &write_time);
#endif
  ST(uint32_t, data_address, file_attributes(&info, path));
  ST(uint64_t, data_address + 4, creation_time);
  ST(uint64_t, data_address + 12, access_time);
  ST(uint64_t, data_address + 20, write_time);
  ST(uint32_t, data_address + 28, (uint32_t)((uint64_t)info.st_size >> 32));
  ST(uint32_t, data_address + 32, (uint32_t)info.st_size);
  aot_last_error = 0;
  return 1;
}

SH(GetFileAttributesExA) {
  return get_attributes_ex(ARG(0), 0, (uint32_t)ARG(1), ARG(2));
}
SH(GetFileAttributesExW) {
  return get_attributes_ex(ARG(0), 1, (uint32_t)ARG(1), ARG(2));
}

static int make_directory(uint64_t address, int wide) {
  char path[PATH_MAX];
  if (normalize_path(address, wide, 0, path) || !path[0] || !strcmp(path, ".")) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  int parent_fd;
  char leaf[NAME_MAX + 1];
  if (open_parent(path, &parent_fd, leaf)) return (int)fail_errno();
  int result = mkdirat(parent_fd, leaf, 0777);
  int saved_error = errno;
  close(parent_fd);
  if (result) {
    errno = saved_error;
    fail_errno();
    return 0;
  }
  aot_last_error = 0;
  return 1;
}

SH(CreateDirectoryA) { return make_directory(ARG(0), 0); }
SH(CreateDirectoryW) { return make_directory(ARG(0), 1); }

static int remove_path(uint64_t address, int wide, int directory) {
  char path[PATH_MAX];
  if (normalize_path(address, wide, 0, path) || !path[0] || !strcmp(path, ".")) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  int parent_fd;
  char leaf[NAME_MAX + 1];
  if (open_parent(path, &parent_fd, leaf)) return (int)fail_errno();
  int result = unlinkat(parent_fd, leaf, directory ? AT_REMOVEDIR : 0);
  int saved_error = errno;
  close(parent_fd);
  if (result) {
    errno = saved_error;
    fail_errno();
    return 0;
  }
  aot_last_error = 0;
  return 1;
}

SH(DeleteFileA) { return remove_path(ARG(0), 0, 0); }
SH(DeleteFileW) { return remove_path(ARG(0), 1, 0); }
SH(RemoveDirectoryA) { return remove_path(ARG(0), 0, 1); }
SH(RemoveDirectoryW) { return remove_path(ARG(0), 1, 1); }

SH(CopyFileA) {
  char src[PATH_MAX], dst[PATH_MAX];
  if (normalize_path(ARG(0), 0, 0, src) || normalize_path(ARG(1), 0, 0, dst)) { aot_last_error = ERROR_INVALID_PARAMETER; return 0; }
  struct stat existing; if (ARG(2) && stat_relative(dst, &existing) == 0) { aot_last_error = ERROR_ALREADY_EXISTS; return 0; }
  int in = open_relative(src, O_RDONLY, 0); if (in < 0) return fail_errno();
  int out = open_relative(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666); if (out < 0) { int e = errno; close(in); errno = e; return fail_errno(); }
  char buffer[65536]; ssize_t n; int ok = 1;
  while ((n = read(in, buffer, sizeof buffer)) > 0) { ssize_t off = 0; while (off < n) { ssize_t w = write(out, buffer + off, (size_t)(n - off)); if (w < 0) { ok = 0; break; } off += w; } if (!ok) break; }
  if (n < 0) ok = 0; int e = errno; if (close(in) || close(out)) ok = 0; if (!ok) { errno = e; return fail_errno(); } aot_last_error = 0; return 1;
}
SH(MoveFileA) {
  char src[PATH_MAX], dst[PATH_MAX]; int sp, dp; char sl[NAME_MAX + 1], dl[NAME_MAX + 1];
  if (normalize_path(ARG(0), 0, 0, src) || normalize_path(ARG(1), 0, 0, dst)) { aot_last_error = ERROR_INVALID_PARAMETER; return 0; }
  if (open_parent(src, &sp, sl)) return fail_errno();
  if (open_parent(dst, &dp, dl)) { int e = errno; close(sp); errno = e; return fail_errno(); }
  int r = renameat(sp, sl, dp, dl), e = errno; close(sp); close(dp); if (r) { errno = e; return fail_errno(); } aot_last_error = 0; return 1;
}
SH(SetCurrentDirectoryA) {
  char path[PATH_MAX], full[PATH_MAX], resolved[PATH_MAX]; if (normalize_path(ARG(0), 0, 0, path)) { aot_last_error = ERROR_INVALID_PARAMETER; return 0; }
  int fd = open_relative(path[0] ? path : ".", O_RDONLY | O_DIRECTORY, 0); if (fd < 0) return fail_errno();
  if (snprintf(full, sizeof full, "%s/%s", game_root_path, path) >= (int)sizeof full || !realpath(full, resolved)) { int e = errno; close(fd); errno = e; return fail_errno(); }
  if (game_root_fd >= 0) close(game_root_fd); game_root_fd = fd; snprintf(game_root_path, sizeof game_root_path, "%s", resolved); (void)chdir(resolved); aot_last_error = 0; return 1;
}
SH(GetFullPathNameA) {
  char path[PATH_MAX], full[PATH_MAX]; if (normalize_path(ARG(0), 0, 1, path)) { aot_last_error = ERROR_INVALID_PARAMETER; return 0; }
  if (path[0]) snprintf(full, sizeof full, "%s/%s", game_root_path, path); else snprintf(full, sizeof full, "%s", game_root_path);
  size_t need = strlen(full) + 1; if (!ARG(2) || ARG(1) < need) return need;
  memcpy((void *)(uintptr_t)ARG(2), full, need); if (ARG(3)) { char *slash = strrchr(full, '/'); STPTR(ARG(3), ARG(2) + (slash ? slash - full + 1 : 0)); } return need - 1;
}
SH(SetFileAttributesA) {
  char path[PATH_MAX]; struct stat st; int parent; char leaf[NAME_MAX + 1];
  if (normalize_path(ARG(0), 0, 0, path) || open_parent(path, &parent, leaf)) { aot_last_error = ERROR_INVALID_PARAMETER; return 0; }
  int r = fstatat(parent, leaf, &st, 0); if (!r) { mode_t mode = st.st_mode; if (ARG(1) & FILE_ATTRIBUTE_READONLY) mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH); else mode |= S_IWUSR; r = fchmodat(parent, leaf, mode, 0); }
  int e = errno; close(parent); if (r) { errno = e; return fail_errno(); } return 1;
}
static void filetime_to_timespec(uint64_t ft, struct timespec *ts) {
  int64_t sec = (int64_t)(ft / 10000000ULL) - 11644473600LL; ts->tv_sec = (time_t)sec; ts->tv_nsec = (long)(ft % 10000000ULL) * 100;
}
SH(GetFileTime) {
  FileHandle *h = get_handle(ARG(0)); struct stat st; if (!h || fstat(h->fd, &st)) { aot_last_error = ERROR_INVALID_HANDLE; return 0; }
#if defined(__APPLE__)
  struct timespec ct = st.st_ctimespec, a = st.st_atimespec, m = st.st_mtimespec;
#else
  struct timespec ct = st.st_ctim, a = st.st_atim, m = st.st_mtim;
#endif
  uint64_t v; if (ARG(1)) { unix_time_to_filetime(ct.tv_sec, ct.tv_nsec, &v); ST(uint64_t, ARG(1), v); }
  if (ARG(2)) { unix_time_to_filetime(a.tv_sec, a.tv_nsec, &v); ST(uint64_t, ARG(2), v); }
  if (ARG(3)) { unix_time_to_filetime(m.tv_sec, m.tv_nsec, &v); ST(uint64_t, ARG(3), v); } return 1;
}
SH(SetFileTime) {
  FileHandle *h = get_handle(ARG(0)); if (!h) { aot_last_error = ERROR_INVALID_HANDLE; return 0; }
  struct timespec times[2]; times[0].tv_nsec = times[1].tv_nsec = UTIME_OMIT;
  if (ARG(2)) filetime_to_timespec(LD(uint64_t, ARG(2)), &times[0]); if (ARG(3)) filetime_to_timespec(LD(uint64_t, ARG(3)), &times[1]);
  if (futimens(h->fd, times)) return fail_errno(); return 1;
}
SH(SetEndOfFile) {
  FileHandle *h = get_handle(ARG(0)); if (!h) { aot_last_error = ERROR_INVALID_HANDLE; return 0; }
  off_t p = lseek(h->fd, 0, SEEK_CUR); if (p < 0 || ftruncate(h->fd, p)) return fail_errno(); return 1;
}
static int lock_file(CPU *c, int unlock, int extended) {
  FileHandle *h = get_handle(ARG(0)); if (!h) { aot_last_error = ERROR_INVALID_HANDLE; return 0; }
  uint64_t off, len; int flags = 0;
  if (extended) { flags = (int)ARG(1); len = (uint64_t)(uint32_t)ARG(3) | ((uint64_t)(uint32_t)ARG(4) << 32); uint64_t ov = ARG(5); size_t oo = aot_is_pe32 ? 8 : 16; off = ov ? (uint64_t)LD(uint32_t, ov + oo) | ((uint64_t)LD(uint32_t, ov + oo + 4) << 32) : 0; }
  else { off = (uint64_t)(uint32_t)ARG(1) | ((uint64_t)(uint32_t)ARG(2) << 32); len = (uint64_t)(uint32_t)ARG(3) | ((uint64_t)(uint32_t)ARG(4) << 32); }
  struct flock fl = { .l_type = unlock ? F_UNLCK : (extended && !(flags & 2)) ? F_RDLCK : F_WRLCK, .l_whence = SEEK_SET, .l_start = (off_t)off, .l_len = (off_t)len };
  int cmd = !unlock && extended && !(flags & 1) ? F_SETLKW : F_SETLK; if (fcntl(h->fd, cmd, &fl)) return fail_errno(); return 1;
}
SH(LockFile) { return lock_file(c, 0, 0); }
SH(UnlockFile) { return lock_file(c, 1, 0); }
SH(LockFileEx) { return lock_file(c, 0, 1); }

/* Small INI reader/writer used by the legacy profile APIs. */
static char *ini_load(uint64_t file, size_t *size) {
  char path[PATH_MAX]; if (!file || normalize_path(file, 0, 0, path)) { aot_last_error = ERROR_INVALID_PARAMETER; return NULL; }
  int fd = open_relative(path, O_RDONLY, 0); if (fd < 0) return NULL; struct stat st; if (fstat(fd, &st)) { close(fd); return NULL; }
  size_t n = (size_t)st.st_size; char *data = malloc(n + 2); if (!data) { close(fd); aot_last_error = 8; return NULL; }
  ssize_t got = read(fd, data, n); close(fd); if (got < 0) { free(data); return NULL; } data[got] = '\n'; data[got + 1] = 0; *size = (size_t)got; return data;
}
static void trim(char **a, char **b) { while (*a < *b && isspace((unsigned char)**a)) (*a)++; while (*b > *a && isspace((unsigned char)(*b)[-1])) (*b)--; }
static int ini_section(char *a, char *b, const char *section) {
  trim(&a, &b); return b > a + 1 && *a == '[' && b[-1] == ']' && (size_t)(b - a - 2) == strlen(section) && !strncasecmp(a + 1, section, (size_t)(b - a - 2));
}
static uint32_t profile_get(const char *section, const char *key, const char *def, uint64_t out, uint32_t cap, uint64_t file, int section_lines) {
  if (!out || !cap) return 0; ((char *)(uintptr_t)out)[0] = 0; size_t size; char *data = ini_load(file, &size);
  if (!data) { if (def && key) { snprintf((char *)(uintptr_t)out, cap, "%s", def); return (uint32_t)strlen((char *)(uintptr_t)out); } return 0; }
  char *p = data, *end = data + size + 1; int active = !section; uint32_t used = 0;
  while (p < end) { char *e = strchr(p, '\n'); if (!e) e = end; char *a = p, *b = e; if (b > a && b[-1] == '\r') b--; trim(&a, &b);
    if (b > a && *a == '[') { active = section && ini_section(a, b, section); if (!section && b[-1] == ']') { *b = 0; a++; b--; *b = 0; size_t n = strlen(a) + 1; if (used + n + 1 >= cap) goto list_done; memcpy((char *)(uintptr_t)(out + used), a, n); used += n; } }
    else if (active && b > a && *a != ';' && *a != '#') {
      char *eq = memchr(a, '=', (size_t)(b - a)); if (eq) { char *ks = a, *ke = eq, *vs = eq + 1, *ve = b; trim(&ks, &ke); trim(&vs, &ve);
        if (key && (size_t)(ke - ks) == strlen(key) && !strncasecmp(ks, key, (size_t)(ke - ks))) { size_t n = (size_t)(ve - vs); if (n >= cap) n = cap - 1; memcpy((void *)(uintptr_t)out, vs, n); ((char *)(uintptr_t)out)[n] = 0; free(data); return n; }
        if (section && !key) { const char *text = section_lines ? a : ks; size_t n = section_lines ? (size_t)(b - a) : (size_t)(ke - ks); if (used + n + 2 > cap) goto list_done; memcpy((char *)(uintptr_t)(out + used), text, n); used += n; ((char *)(uintptr_t)out)[used++] = 0; }
      }
    } p = e + 1;
  }
list_done:
  free(data); if (!key && out) { if (used + 1 < cap) ((char *)(uintptr_t)out)[used] = 0; return used; }
  if (key && def) { snprintf((char *)(uintptr_t)out, cap, "%s", def); return (uint32_t)strlen((char *)(uintptr_t)out); } return 0;
}
SH(GetPrivateProfileStringA) { return profile_get(A(0) ? STR(A(0)) : NULL, A(1) ? STR(A(1)) : NULL, A(2) ? STR(A(2)) : "", A(3), (uint32_t)A(4), A(5), 0); }
SH(GetPrivateProfileIntA) { char tmp[128], def[32]; snprintf(def, sizeof def, "%d", (int)A(1)); profile_get(STR(A(0)), STR(A(2)), def, (uint64_t)(uintptr_t)tmp, sizeof tmp, A(3), 0); return (uint32_t)strtol(tmp, NULL, 0); }
SH(GetPrivateProfileSectionA) { return profile_get(STR(A(0)), NULL, NULL, A(1), (uint32_t)A(2), A(3), 1); }
SH(GetPrivateProfileSectionNamesA) { return profile_get(NULL, NULL, NULL, A(0), (uint32_t)A(1), A(2), 0); }
SH(WritePrivateProfileStringA) {
  const char *section = STR(A(0)), *key = STR(A(1)), *value = STR(A(2)); if (!section || !A(3)) { aot_last_error = 87; return 0; }
  size_t oldn = 0; char *old = ini_load(A(3), &oldn); if (!old) { old = calloc(1, 2); oldn = 0; }
  size_t cap = oldn + strlen(section) + (key ? strlen(key) : 0) + (value ? strlen(value) : 0) + 64; char *next = malloc(cap), *p = old; size_t used = 0; int in = 0, done = 0;
  while (p && *p) { char *e = strchr(p, '\n'); if (!e) e = p + strlen(p); char *a = p, *b = e; if (b > a && b[-1] == '\r') b--; char save = *b; *b = 0;
    if (ini_section(a, b, section)) { in = 1; } else if (*a == '[') { if (in && !done && key && value) used += snprintf(next + used, cap - used, "%s=%s\n", key, value), done = 1; in = 0; }
    int skip = in && !key; if (in && key) { char *eq = strchr(a, '='); if (eq) { *eq = 0; char *ke = eq; trim(&a, &ke); skip = strlen(a) == strlen(key) && !strcasecmp(a, key); *eq = '='; } }
    *b = save; if (!skip) used += snprintf(next + used, cap - used, "%.*s\n", (int)(e - p), p); p = *e ? e + 1 : e;
  }
  if (!done && key && value) { if (!in) used += snprintf(next + used, cap - used, "[%s]\n", section); used += snprintf(next + used, cap - used, "%s=%s\n", key, value); }
  free(old); char path[PATH_MAX]; if (normalize_path(A(3), 0, 0, path)) { free(next); return 0; } int fd = open_relative(path, O_WRONLY | O_CREAT | O_TRUNC, 0666); if (fd < 0) { free(next); return fail_errno(); }
  size_t off = 0; int ok = 1; while (off < used) { ssize_t wr = write(fd, next + off, used - off); if (wr < 0 && errno == EINTR) continue; if (wr <= 0) { ok = 0; break; } off += (size_t)wr; }
  int e = errno; if (close(fd)) { ok = 0; e = errno; } free(next); if (!ok) { errno = e; return fail_errno(); } aot_last_error = 0; return 1;
}

#define S(name) { #name, sh_##name }
#define SA(name, target) { name, sh_##target }
const ShimEnt aot_shim_table_file[] = {
  S(GetLastError), S(SetLastError),
  S(GetCurrentDirectoryA), S(GetCurrentDirectoryW),
  S(GetModuleFileNameA), S(GetModuleFileNameW),
  S(CreateFileA), S(CreateFileW), S(ReadFile), S(WriteFile),
  S(GetFileType), S(CloseHandle), S(FlushFileBuffers),
  S(SetFilePointer), S(SetFilePointerEx), S(GetFileSize), S(GetFileSizeEx),
  S(GetFileAttributesA), S(GetFileAttributesW),
  S(GetFileAttributesExA), S(GetFileAttributesExW),
  S(CreateDirectoryA), S(CreateDirectoryW), S(DeleteFileA), S(DeleteFileW),
  S(RemoveDirectoryA), S(RemoveDirectoryW), S(CopyFileA), S(MoveFileA), S(SetCurrentDirectoryA), S(GetFullPathNameA),
  S(SetFileAttributesA), S(GetFileTime), S(SetFileTime), S(SetEndOfFile), S(LockFile), S(UnlockFile), S(LockFileEx),
  S(GetPrivateProfileStringA), S(GetPrivateProfileIntA), S(GetPrivateProfileSectionA), S(GetPrivateProfileSectionNamesA), S(WritePrivateProfileStringA),
  S(FindFirstFileA), S(FindFirstFileW),
  S(FindNextFileA), S(FindNextFileW), S(FindClose),
  { "CreateEventA", aot_file_async_CreateEventA },
  { "CreateEventW", aot_file_async_CreateEventW },
  { "SetEvent", aot_file_async_SetEvent },
  { "ResetEvent", aot_file_async_ResetEvent },
  { "PulseEvent", aot_file_async_PulseEvent },
  { "WaitForSingleObject", aot_file_async_WaitForSingleObject },
  { "GetOverlappedResult", aot_file_async_GetOverlappedResult },
  S(CreateFileMappingA), S(CreateFileMappingW),
  S(MapViewOfFile), S(MapViewOfFileEx), S(UnmapViewOfFile),
  S(FlushViewOfFile),
  SA("WriteConsoleA", WriteFile),
  { 0, 0 }
};
