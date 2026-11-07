#define _GNU_SOURCE
#include "shims.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ARG(n) ((n) == 0 ? c->r[1] : (n) == 1 ? c->r[2] : \
    (n) == 2 ? c->r[8] : (n) == 3 ? c->r[9] : \
    LD(uint64_t, c->r[4] + 8 + 8 * (n)))

#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_IO_PENDING 997u
#define ERROR_OPERATION_ABORTED 995u
#define ERROR_IO_INCOMPLETE 996u
#define ERROR_DISK_FULL 112u
#define ERROR_GEN_FAILURE 31u
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED UINT32_MAX
#define INFINITE UINT32_MAX
#define STATUS_PENDING 0x103u
#define EVENT_HANDLE_BASE 0xA07B000000000000ULL
#define EVENT_HANDLE_MASK 0xFFFF000000000000ULL
#define EVENT_CAPACITY 128
static int ev_tr(void) { static int t = -1; if (t < 0) t = getenv("AOT_TRACE_EV") != NULL; return t; }
#include <sys/syscall.h>
#define EVT(op, h) do { if (ev_tr()) fprintf(stderr, "[ev] t%ld %s %llx\n", (long)syscall(SYS_gettid), op, (unsigned long long)(h)); } while (0)
#define OPERATION_CAPACITY 128

typedef struct {
  uint16_t generation;
  uint32_t pulse_generation;
  uint8_t open;
  uint8_t manual_reset;
  uint8_t signaled;
} EventHandle;

typedef struct {
  uint64_t overlapped;
  uint64_t event_value;
  uint64_t buffer;
  uint64_t length;
  uint64_t offset;
  int fd;
  uint32_t error;
  uint32_t bytes;
  uint8_t write;
  uint8_t used;
  uint8_t active;
  uint8_t complete;
  uint8_t waiters;
} AsyncOperation;

static EventHandle events[EVENT_CAPACITY];
static AsyncOperation operations[OPERATION_CAPACITY];
static pthread_mutex_t async_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t async_condition = PTHREAD_COND_INITIALIZER;

static uint32_t error_from_errno(int error) {
  switch (error) {
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
    case ENOMEM: case EAGAIN: return ERROR_NOT_ENOUGH_MEMORY;
    case EBADF: return ERROR_INVALID_HANDLE;
    case EINVAL: case EOVERFLOW: return ERROR_INVALID_PARAMETER;
    case ECANCELED: return ERROR_OPERATION_ABORTED;
    case ENOSPC: return ERROR_DISK_FULL;
    default: return ERROR_GEN_FAILURE;
  }
}

static EventHandle *get_event(uint64_t value) {
  if ((value & EVENT_HANDLE_MASK) != EVENT_HANDLE_BASE) return NULL;
  uint32_t slot = (uint32_t)(value & 0xffffu);
  uint16_t generation = (uint16_t)(value >> 32);
  if (!slot || slot > EVENT_CAPACITY) return NULL;
  EventHandle *event = &events[slot - 1];
  return event->open && event->generation == generation ? event : NULL;
}

static size_t overlapped_offset_offset(void) { return aot_is_pe32 ? 8 : 16; }
static size_t overlapped_event_offset(void) { return aot_is_pe32 ? 16 : 24; }

static void set_overlapped_status(uint64_t overlapped, uint64_t status,
                                  uint64_t bytes) {
  if (aot_is_pe32) {
    ST(uint32_t, overlapped, (uint32_t)status);
    ST(uint32_t, overlapped + 4, (uint32_t)bytes);
  } else {
    ST(uint64_t, overlapped, status);
    ST(uint64_t, overlapped + 8, bytes);
  }
}

static uint64_t overlapped_status(uint64_t overlapped) {
  return aot_is_pe32 ? LD(uint32_t, overlapped) : LD(uint64_t, overlapped);
}

static uint64_t overlapped_bytes(uint64_t overlapped) {
  return aot_is_pe32 ? LD(uint32_t, overlapped + 4) :
      LD(uint64_t, overlapped + 8);
}

static uint64_t overlapped_event(uint64_t overlapped) {
  size_t offset = overlapped_event_offset();
  uint64_t event = aot_is_pe32 ? LD(uint32_t, overlapped + offset) :
      LD(uint64_t, overlapped + offset);
  return aot_is_pe32 ? aot_x86_alias_in(event) : event;
}

static uint64_t install_event(int manual_reset, int signaled) {
  for (size_t i = 0; i < EVENT_CAPACITY; i++) {
    EventHandle *event = &events[i];
    if (event->open) continue;
    if (++event->generation == 0) event->generation = 1;
    event->manual_reset = (uint8_t)manual_reset;
    event->signaled = (uint8_t)signaled;
    event->open = 1;
    return EVENT_HANDLE_BASE | ((uint64_t)event->generation << 32) |
        (uint64_t)(i + 1);
  }
  aot_last_error = ERROR_NOT_ENOUGH_MEMORY;
  return 0;
}

int aot_win_async_close_handle(uint64_t value) {
  if ((value & EVENT_HANDLE_MASK) != EVENT_HANDLE_BASE) return 0;
  pthread_mutex_lock(&async_mutex);
  EventHandle *event = get_event(value);
  if (!event) {
    pthread_mutex_unlock(&async_mutex);
    aot_last_error = ERROR_INVALID_HANDLE;
    return -1;
  }
  event->open = 0;
  pthread_cond_broadcast(&async_condition);
  pthread_mutex_unlock(&async_mutex);
  aot_last_error = 0;
  return 1;
}

static void *run_operation(void *argument) {
  AsyncOperation *operation = argument;
  ssize_t result;
  do {
    if (operation->write) {
      result = pwrite(operation->fd,
          (const void *)(uintptr_t)operation->buffer,
          (size_t)operation->length, (off_t)operation->offset);
    } else {
      result = pread(operation->fd, (void *)(uintptr_t)operation->buffer,
          (size_t)operation->length, (off_t)operation->offset);
    }
  } while (result < 0 && errno == EINTR);
  int saved_error = result < 0 ? errno : 0;

  pthread_mutex_lock(&async_mutex);
  operation->error = saved_error ? error_from_errno(saved_error) : 0;
  operation->bytes = result < 0 ? 0u : (uint32_t)result;
  operation->active = 0;
  operation->complete = 1;
  set_overlapped_status(operation->overlapped,
      operation->error ? operation->error : 0, operation->bytes);
  EventHandle *event = get_event(operation->event_value);
  if (event) event->signaled = 1;
  pthread_cond_broadcast(&async_condition);
  int fd = operation->fd;
  operation->fd = -1;
  pthread_mutex_unlock(&async_mutex);
  close(fd);
  return NULL;
}

static uint64_t start_operation(CPU *c, int writing) {
  uint64_t file = ARG(0);
  uint64_t buffer = ARG(1);
  uint64_t length = (uint32_t)ARG(2);
  uint64_t bytes_address = ARG(3);
  uint64_t overlapped = ARG(4);
  if (bytes_address) ST(uint32_t, bytes_address, 0);
  if (!overlapped || (length && !buffer) ||
      length > (uint64_t)SSIZE_MAX) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  size_t offset_offset = overlapped_offset_offset();
  uint32_t offset_low = LD(uint32_t, overlapped + offset_offset);
  uint32_t offset_high = LD(uint32_t, overlapped + offset_offset + 4);
  uint64_t offset = ((uint64_t)offset_high << 32) | offset_low;
  if (offset > (uint64_t)INT64_MAX) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  uint64_t event_value = overlapped_event(overlapped);
  int fd = aot_win_file_duplicate(file, writing, 1);
  if (fd < 0) return 0;
  pthread_mutex_lock(&async_mutex);
  if (event_value && !get_event(event_value)) {
    pthread_mutex_unlock(&async_mutex);
    close(fd);
    aot_last_error = ERROR_INVALID_HANDLE;
    return 0;
  }
  AsyncOperation *operation = NULL;
  for (size_t i = 0; i < OPERATION_CAPACITY; i++) {
    if (operations[i].used && operations[i].active &&
        operations[i].overlapped == overlapped) {
      pthread_mutex_unlock(&async_mutex);
      close(fd);
      aot_last_error = ERROR_INVALID_PARAMETER;
      return 0;
    }
  }
  for (size_t i = 0; i < OPERATION_CAPACITY; i++) {
    if (!operations[i].used ||
        (!operations[i].active && !operations[i].waiters)) {
      operation = &operations[i];
      break;
    }
  }
  if (!operation) {
    pthread_mutex_unlock(&async_mutex);
    close(fd);
    aot_last_error = ERROR_NOT_ENOUGH_MEMORY;
    return 0;
  }
  *operation = (AsyncOperation){
    .overlapped = overlapped,
    .event_value = event_value,
    .buffer = buffer,
    .length = length,
    .offset = offset,
    .fd = fd,
    .write = (uint8_t)writing,
    .used = 1,
    .active = 1
  };
  set_overlapped_status(overlapped, STATUS_PENDING, 0);
  if (event_value) get_event(event_value)->signaled = 0;
  pthread_mutex_unlock(&async_mutex);

  pthread_t thread;
  int error = pthread_create(&thread, NULL, run_operation, operation);
  if (error) {
    pthread_mutex_lock(&async_mutex);
    operation->active = 0;
    operation->complete = 0;
    operation->used = 0;
    operation->fd = -1;
    set_overlapped_status(overlapped, error_from_errno(error), 0);
    pthread_cond_broadcast(&async_condition);
    pthread_mutex_unlock(&async_mutex);
    close(fd);
    aot_last_error = error_from_errno(error);
    return 0;
  }
  pthread_detach(thread);
  aot_last_error = ERROR_IO_PENDING;
  return 0;
}

uint64_t aot_file_async_ReadFile(CPU *c) {
  return start_operation(c, 0);
}

uint64_t aot_file_async_WriteFile(CPU *c) {
  return start_operation(c, 1);
}

uint64_t aot_file_async_CreateEventA(CPU *c) {
  if (ARG(3)) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  pthread_mutex_lock(&async_mutex);
  uint64_t value = install_event((int)ARG(1), (int)ARG(2));
  pthread_mutex_unlock(&async_mutex); if (ev_tr()) fprintf(stderr, "[ev] create %llx manual=%d init=%d\n", (unsigned long long)value, (int)ARG(1), (int)ARG(2));
  if (value) aot_last_error = 0;
  return value;
}

uint64_t aot_file_async_CreateEventW(CPU *c) {
  return aot_file_async_CreateEventA(c);
}

void aot_wd_set(uint64_t h);
uint64_t aot_file_async_SetEvent(CPU *c) {
  aot_wd_set(ARG(0)); EVT("set", ARG(0));
  pthread_mutex_lock(&async_mutex);
  EventHandle *event = get_event(ARG(0));
  if (!event) {
    pthread_mutex_unlock(&async_mutex);
    aot_last_error = ERROR_INVALID_HANDLE;
    return 0;
  }
  event->signaled = 1;
  pthread_cond_broadcast(&async_condition);
  pthread_mutex_unlock(&async_mutex);
  aot_last_error = 0;
  return 1;
}

uint64_t aot_file_async_ResetEvent(CPU *c) {
  EVT("reset", ARG(0));
  pthread_mutex_lock(&async_mutex);
  EventHandle *event = get_event(ARG(0));
  if (!event) {
    pthread_mutex_unlock(&async_mutex);
    aot_last_error = ERROR_INVALID_HANDLE;
    return 0;
  }
  event->signaled = 0;
  pthread_mutex_unlock(&async_mutex);
  aot_last_error = 0;
  return 1;
}

uint64_t aot_file_async_PulseEvent(CPU *c) {
  pthread_mutex_lock(&async_mutex);
  EventHandle *event = get_event(ARG(0));
  if (!event) {
    pthread_mutex_unlock(&async_mutex);
    aot_last_error = ERROR_INVALID_HANDLE;
    return 0;
  }
  /* A pulse is observed only by threads already waiting.  The generation
     change remains visible to those waiters after the signaled bit resets. */
  event->pulse_generation++;
  event->signaled = 1;
  pthread_cond_broadcast(&async_condition);
  event->signaled = 0;
  pthread_mutex_unlock(&async_mutex);
  aot_last_error = 0;
  return 1;
}

uint64_t aot_file_async_WaitForSingleObject(CPU *c) {
  uint64_t value = ARG(0);
  uint32_t timeout = (uint32_t)ARG(1);
  if (timeout) EVT("wait", value);
  uint32_t pulse_generation = 0;
  int pulse_ready = 0;
  struct timespec deadline = { 0, 0 };
  if (timeout != INFINITE) {
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout / 1000;
    deadline.tv_nsec += (long)(timeout % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
    }
  }
  pthread_mutex_lock(&async_mutex);
  for (;;) {
    EventHandle *event = get_event(value);
    if (!event) {
      pthread_mutex_unlock(&async_mutex);
      aot_last_error = ERROR_INVALID_HANDLE;
      return WAIT_FAILED;
    }
    if (!pulse_ready) {
      pulse_generation = event->pulse_generation;
      pulse_ready = 1;
    } else if (event->pulse_generation != pulse_generation) {
      pthread_mutex_unlock(&async_mutex);
      aot_last_error = 0;
      return WAIT_OBJECT_0;
    }
    if (event->signaled) {
      if (!event->manual_reset) event->signaled = 0;
      pthread_mutex_unlock(&async_mutex);
      aot_last_error = 0;
      return WAIT_OBJECT_0;
    }
    if (!timeout) {
      pthread_mutex_unlock(&async_mutex);
      aot_last_error = 0;
      return WAIT_TIMEOUT;
    }
    int error = timeout == INFINITE ?
        pthread_cond_wait(&async_condition, &async_mutex) :
        pthread_cond_timedwait(&async_condition, &async_mutex, &deadline);
    if (error == ETIMEDOUT) {
      pthread_mutex_unlock(&async_mutex);
      aot_last_error = 0;
      return WAIT_TIMEOUT;
    }
    if (error) {
      pthread_mutex_unlock(&async_mutex);
      aot_last_error = error_from_errno(error);
      return WAIT_FAILED;
    }
  }
}

uint64_t aot_file_async_GetOverlappedResult(CPU *c) {
  uint64_t file = ARG(0);
  uint64_t overlapped = ARG(1);
  uint64_t bytes_address = ARG(2);
  int wait = (int)ARG(3);
  if (!overlapped || !bytes_address) {
    aot_last_error = ERROR_INVALID_PARAMETER;
    return 0;
  }
  int fd = aot_win_file_duplicate(file, -1, 1);
  if (fd < 0) return 0;
  close(fd);

  pthread_mutex_lock(&async_mutex);
  AsyncOperation *operation = NULL;
  for (size_t i = 0; i < OPERATION_CAPACITY; i++) {
    if (operations[i].used && operations[i].overlapped == overlapped) {
      operation = &operations[i];
      break;
    }
  }
  if (operation && operation->active && wait) {
    operation->waiters++;
    while (operation->active)
      pthread_cond_wait(&async_condition, &async_mutex);
    operation->waiters--;
  }
  if (operation && operation->active) {
    pthread_mutex_unlock(&async_mutex);
    aot_last_error = ERROR_IO_INCOMPLETE;
    return 0;
  }
  if (operation && operation->complete) {
    uint32_t error = operation->error;
    uint32_t bytes = operation->bytes;
    operation->used = 0;
    operation->complete = 0;
    pthread_mutex_unlock(&async_mutex);
    if (error) {
      aot_last_error = error;
      return 0;
    }
    ST(uint32_t, bytes_address, bytes);
    aot_last_error = 0;
    return 1;
  }
  pthread_mutex_unlock(&async_mutex);
  uint64_t status = overlapped_status(overlapped);
  if (status == STATUS_PENDING) {
    aot_last_error = ERROR_IO_INCOMPLETE;
    return 0;
  }
  if (status) {
    aot_last_error = (uint32_t)status;
    return 0;
  }
  ST(uint32_t, bytes_address, (uint32_t)overlapped_bytes(overlapped));
  aot_last_error = 0;
  return 1;
}

#undef ARG
