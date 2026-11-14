#define _GNU_SOURCE
#include "shims.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#endif

#define ARG(n) ((n) == 0 ? c->r[1] : (n) == 1 ? c->r[2] : \
    (n) == 2 ? c->r[8] : (n) == 3 ? c->r[9] : \
    LD(uint64_t, c->r[4] + 8 + 8 * (n)))
#define SH(name) static uint64_t sh_##name(CPU *c)

#define MMSYSERR_NOERROR 0u
#define MMSYSERR_ERROR 1u
#define MMSYSERR_BADDEVICEID 2u
#define MMSYSERR_NOMEM 7u
#define MMSYSERR_NOTSUPPORTED 8u
#define MMSYSERR_INVALHANDLE 5u
#define MMSYSERR_INVALPARAM 11u
#define WAVERR_BADFORMAT 32u
#define WAVERR_STILLPLAYING 33u
#define WAVERR_UNPREPARED 34u
#define WAVE_MAPPER UINT32_MAX
#define WAVE_FORMAT_PCM 1u
#define WAVE_FORMAT_QUERY 0x0001u
#define CALLBACK_NULL 0u
#define WHDR_DONE 0x00000001u
#define WHDR_PREPARED 0x00000002u
#define WHDR_INQUEUE 0x00000010u
#define AUDIO_HANDLE_BASE 0xA07A000000000000ULL
#define AUDIO_HANDLE_MASK 0xFFFF000000000000ULL
#define AUDIO_DEVICE_CAPACITY 4
#define AUDIO_QUEUE_CAPACITY 64

typedef struct {
  uint64_t header;
  uint8_t *converted;
} AudioQueueEntry;

typedef struct {
  uint16_t generation;
  uint8_t open;
  uint8_t paused;
  uint32_t sample_rate;
  uint16_t channels;
  uint16_t bits_per_sample;
  AudioQueueEntry queue[AUDIO_QUEUE_CAPACITY];
  uint32_t queue_head;
  uint32_t queue_count;
#ifdef __ANDROID__
  SLObjectItf player_object;
  SLPlayItf player;
  SLAndroidSimpleBufferQueueItf buffer_queue;
#endif
} AudioDevice;

static AudioDevice devices[AUDIO_DEVICE_CAPACITY];
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef __ANDROID__
static SLObjectItf engine_object;
static SLEngineItf engine;
static SLObjectItf output_mix_object;

/* ---- DirectSound output: the DS buffers the x86 DSOUND shim keeps in guest memory are mixed here and
   handed to OpenSLES, so IDirectSoundBuffer8::Play is actually audible. One player at the format of the
   first buffer that plays; sources with any other format are left silent rather than resampled, and the
   first such mismatch is reported once. ---- */
static int audio_engine_init(void);
#define AOT_DS_SRC 16
#define AOT_DS_FRAMES 1024
typedef struct { void *key; const uint8_t *data; uint32_t size, pos; int looping, playing; } DsSrc;
static DsSrc ds_src[AOT_DS_SRC];
static pthread_mutex_t ds_mu = PTHREAD_MUTEX_INITIALIZER;
static SLObjectItf ds_obj; static SLPlayItf ds_play; static SLAndroidSimpleBufferQueueItf ds_queue;
static int ds_freq = 44100, ds_ch = 2, ds_warned;
static int16_t ds_mix[AOT_DS_FRAMES * 2];
static int16_t ds_out[2][AOT_DS_FRAMES * 2];
static int ds_flip;

static void ds_feed(void) {   /* 16-bit stereo at ds_freq, saturating add of every playing source */
  memset(ds_mix, 0, sizeof ds_mix);
  int n = (int)(sizeof ds_mix / sizeof ds_mix[0]);
  for (int i = 0; i < AOT_DS_SRC; i++) {
    DsSrc *s = &ds_src[i];
    if (!s->key || !s->playing || !s->data || s->size < 4) continue;
    for (int k = 0; k < n; k++) {
      uint32_t at = s->pos;
      if (at + 2 > s->size) { if (!s->looping) { s->playing = 0; break; } s->pos = at = 0; }
      int16_t v = (int16_t)(uint16_t)(s->data[at] | (s->data[at + 1] << 8));
      int32_t m = (int32_t)ds_mix[k] + v;
      ds_mix[k] = (int16_t)(m > 32767 ? 32767 : m < -32768 ? -32768 : m);
      s->pos = at + 2; if (s->pos >= s->size) s->pos = s->looping ? 0 : s->size;
    }
  }
}
static void ds_queue_cb(SLAndroidSimpleBufferQueueItf q, void *ctx) {
  (void)q; (void)ctx;
  pthread_mutex_lock(&ds_mu);
  ds_feed();
  int16_t *b = ds_out[ds_flip]; memcpy(b, ds_mix, sizeof ds_mix); ds_flip ^= 1;
  pthread_mutex_unlock(&ds_mu);
  (*ds_queue)->Enqueue(ds_queue, b, sizeof ds_mix);
}
static int ds_open(int freq, int ch) {
  if (ds_obj) return 0;
  ds_freq = freq > 0 ? freq : 44100; ds_ch = ch == 1 ? 1 : 2;
  if (audio_engine_init() != 0) return -1;
  SLDataLocator_AndroidSimpleBufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2 };
  SLDataFormat_PCM pcm = { SL_DATAFORMAT_PCM, (SLuint32)ds_ch, (SLuint32)ds_freq * 1000, SL_PCMSAMPLEFORMAT_FIXED_16, 16,
                           ds_ch == 1 ? SL_SPEAKER_FRONT_CENTER : (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT), SL_BYTEORDER_LITTLEENDIAN };
  SLDataSource src = { &loc, &pcm };
  SLDataLocator_OutputMix mixloc = { SL_DATALOCATOR_OUTPUTMIX, output_mix_object };
  SLDataSink sink = { &mixloc, NULL };
  const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
  const SLboolean req[] = { SL_BOOLEAN_TRUE };
  if ((*engine)->CreateAudioPlayer(engine, &ds_obj, &src, &sink, 1, ids, req) != SL_RESULT_SUCCESS) goto bad;
  if ((*ds_obj)->Realize(ds_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) goto bad;
  if ((*ds_obj)->GetInterface(ds_obj, SL_IID_PLAY, &ds_play) != SL_RESULT_SUCCESS) goto bad;
  if ((*ds_obj)->GetInterface(ds_obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &ds_queue) != SL_RESULT_SUCCESS) goto bad;
  if ((*ds_queue)->RegisterCallback(ds_queue, ds_queue_cb, NULL) != SL_RESULT_SUCCESS) goto bad;
  if ((*ds_play)->SetPlayState(ds_play, SL_PLAYSTATE_PLAYING) != SL_RESULT_SUCCESS) goto bad;
  memset(ds_out, 0, sizeof ds_out);
  (*ds_queue)->Enqueue(ds_queue, ds_out[0], sizeof ds_out[0]);
  (*ds_queue)->Enqueue(ds_queue, ds_out[1], sizeof ds_out[1]);
  ds_flip = 0;
  fprintf(stderr, "[audio] DirectSound output %d Hz %d ch 16-bit (OpenSLES)\n", ds_freq, ds_ch);
  return 0;
bad:
  if (ds_obj) { (*ds_obj)->Destroy(ds_obj); ds_obj = NULL; }
  ds_play = NULL; ds_queue = NULL;
  fprintf(stderr, "[audio] OpenSLES refused a %d Hz %d ch player: DirectSound stays silent\n", ds_freq, ds_ch);
  return -1;
}
void *aot_ds_play(void *key, const void *data, uint32_t size, uint32_t freq, int channels, int bits, int looping) {
  if (!key || !data || size < 4 || bits != 16) return NULL;
  pthread_mutex_lock(&ds_mu);
  if (ds_open((int)freq, channels) != 0) { pthread_mutex_unlock(&ds_mu); return NULL; }
  if ((int)freq != ds_freq || (channels == 1 ? 1 : 2) != ds_ch) {
    if (!ds_warned++) fprintf(stderr, "[audio] DirectSound buffer at %u Hz %d ch is not mixed (output is %d Hz %d ch)\n", freq, channels, ds_freq, ds_ch);
    pthread_mutex_unlock(&ds_mu); return NULL;
  }
  int slot = -1, spare = -1;
  for (int i = 0; i < AOT_DS_SRC; i++) { if (ds_src[i].key == key) { slot = i; break; } if (spare < 0 && !ds_src[i].key) spare = i; }
  if (slot < 0) slot = spare;
  if (slot < 0) { pthread_mutex_unlock(&ds_mu); return NULL; }
  ds_src[slot].key = key; ds_src[slot].data = data; ds_src[slot].size = size; ds_src[slot].pos = 0;
  ds_src[slot].looping = looping; ds_src[slot].playing = 1;
  pthread_mutex_unlock(&ds_mu);
  return &ds_src[slot];
}
void aot_ds_stop(void *h) {
  DsSrc *s = (DsSrc *)h; if (!s) return;
  pthread_mutex_lock(&ds_mu); s->playing = 0; s->key = NULL; pthread_mutex_unlock(&ds_mu);
}

static int audio_engine_init(void) {
  if (engine) return 0;
  if (slCreateEngine(&engine_object, 0, NULL, 0, NULL, NULL) != SL_RESULT_SUCCESS)
    return -1;
  if ((*engine_object)->Realize(engine_object, SL_BOOLEAN_FALSE) !=
      SL_RESULT_SUCCESS)
    goto failed;
  if ((*engine_object)->GetInterface(engine_object, SL_IID_ENGINE, &engine) !=
      SL_RESULT_SUCCESS)
    goto failed;
  if ((*engine)->CreateOutputMix(engine, &output_mix_object, 0, NULL, NULL) !=
      SL_RESULT_SUCCESS)
    goto failed;
  if ((*output_mix_object)->Realize(output_mix_object, SL_BOOLEAN_FALSE) !=
      SL_RESULT_SUCCESS)
    goto failed;
  return 0;

failed:
  if (output_mix_object) {
    (*output_mix_object)->Destroy(output_mix_object);
    output_mix_object = NULL;
  }
  if (engine_object) {
    (*engine_object)->Destroy(engine_object);
    engine_object = NULL;
  }
  engine = NULL;
  return -1;
}

static void audio_queue_callback(SLAndroidSimpleBufferQueueItf queue,
                                 void *context) {
  (void)queue;
  AudioDevice *device = context;
  pthread_mutex_lock(&audio_mutex);
  if (device->open && device->queue_count) {
    AudioQueueEntry *entry = &device->queue[device->queue_head];
    uint64_t header = entry->header;
    device->queue_head = (device->queue_head + 1) % AUDIO_QUEUE_CAPACITY;
    device->queue_count--;
    uint32_t flags = LD(uint32_t, header + 24);
    flags = (flags & ~WHDR_INQUEUE) | WHDR_DONE;
    ST(uint32_t, header + 24, flags);
    free(entry->converted);
    entry->converted = NULL;
  }
  pthread_mutex_unlock(&audio_mutex);
}
#endif

void aot_win_audio_cleanup(void) {
  for (size_t i = 0; i < AUDIO_DEVICE_CAPACITY; i++) {
    AudioQueueEntry queued[AUDIO_QUEUE_CAPACITY];
    uint32_t queued_count = 0;
    pthread_mutex_lock(&audio_mutex);
    AudioDevice *device = &devices[i];
    if (!device->open) {
      pthread_mutex_unlock(&audio_mutex);
      continue;
    }
#ifdef __ANDROID__
    SLAndroidSimpleBufferQueueItf buffer_queue = device->buffer_queue;
    SLObjectItf player_object = device->player_object;
    device->player_object = NULL;
    device->player = NULL;
    device->buffer_queue = NULL;
#endif
    for (uint32_t j = 0; j < device->queue_count; j++) {
      uint32_t slot = (device->queue_head + j) % AUDIO_QUEUE_CAPACITY;
      queued[queued_count++] = device->queue[slot];
    }
    device->queue_count = 0;
    device->open = 0;
    pthread_mutex_unlock(&audio_mutex);
#ifdef __ANDROID__
    if (buffer_queue) (*buffer_queue)->Clear(buffer_queue);
    if (player_object) (*player_object)->Destroy(player_object);
#endif
    for (uint32_t j = 0; j < queued_count; j++) {
      uint64_t header = queued[j].header;
      uint32_t flags = LD(uint32_t, header + 24);
      ST(uint32_t, header + 24, (flags & ~WHDR_INQUEUE) | WHDR_DONE);
      free(queued[j].converted);
    }
  }
#ifdef __ANDROID__
  pthread_mutex_lock(&audio_mutex);
  if (output_mix_object) {
    (*output_mix_object)->Destroy(output_mix_object);
    output_mix_object = NULL;
  }
  if (engine_object) {
    (*engine_object)->Destroy(engine_object);
    engine_object = NULL;
  }
  engine = NULL;
  pthread_mutex_unlock(&audio_mutex);
#endif
}

static AudioDevice *get_device(uint64_t value) {
  if ((value & AUDIO_HANDLE_MASK) != AUDIO_HANDLE_BASE) return NULL;
  uint32_t slot = (uint32_t)(value & 0xffffu);
  uint16_t generation = (uint16_t)(value >> 32);
  if (!slot || slot > AUDIO_DEVICE_CAPACITY) return NULL;
  AudioDevice *device = &devices[slot - 1];
  return device->open && device->generation == generation ? device : NULL;
}

static uint64_t install_device(uint32_t sample_rate, uint16_t channels,
                               uint16_t bits) {
  for (size_t i = 0; i < AUDIO_DEVICE_CAPACITY; i++) {
    AudioDevice *device = &devices[i];
    if (device->open) continue;
    if (++device->generation == 0) device->generation = 1;
    device->sample_rate = sample_rate;
    device->channels = channels;
    device->bits_per_sample = bits;
    device->queue_head = 0;
    device->queue_count = 0;
    device->paused = 0;
    device->open = 1;
    return AUDIO_HANDLE_BASE | ((uint64_t)device->generation << 32) |
        (uint64_t)(i + 1);
  }
  return 0;
}

static int supported_format(uint64_t address) {
  if (!address) return 0;
  uint16_t format = LD(uint16_t, address);
  uint16_t channels = LD(uint16_t, address + 2);
  uint32_t sample_rate = LD(uint32_t, address + 4);
  uint32_t average_bytes = LD(uint32_t, address + 8);
  uint16_t block_align = LD(uint16_t, address + 12);
  uint16_t bits = LD(uint16_t, address + 14);
  uint16_t extra_size = LD(uint16_t, address + 16);
  if (format != WAVE_FORMAT_PCM || extra_size ||
      (channels != 1 && channels != 2) ||
      sample_rate < 8000 || sample_rate > 192000 ||
      (bits != 8 && bits != 16))
    return 0;
  return block_align == channels * (bits / 8) &&
      average_bytes == sample_rate * block_align;
}

SH(waveOutGetNumDevs) {
  (void)c;
  return 1;
}

SH(waveOutOpen) {
  uint64_t output = ARG(0);
  uint32_t device_id = (uint32_t)ARG(1);
  uint64_t format = ARG(2);
  uint64_t callback = ARG(3);
  uint32_t flags = (uint32_t)ARG(5);
  if (!supported_format(format)) return WAVERR_BADFORMAT;
  if (device_id != 0 && device_id != WAVE_MAPPER) return MMSYSERR_BADDEVICEID;
  if (flags & WAVE_FORMAT_QUERY)
    return flags == WAVE_FORMAT_QUERY ? MMSYSERR_NOERROR :
        MMSYSERR_NOTSUPPORTED;
  if (!output) return MMSYSERR_INVALPARAM;
  if (flags != CALLBACK_NULL || callback) return MMSYSERR_NOTSUPPORTED;
  uint16_t channels = LD(uint16_t, format + 2);
  uint32_t sample_rate = LD(uint32_t, format + 4);
  uint16_t bits = LD(uint16_t, format + 14);
  pthread_mutex_lock(&audio_mutex);
#ifdef __ANDROID__
  if (audio_engine_init()) {
    pthread_mutex_unlock(&audio_mutex);
    return MMSYSERR_ERROR;
  }
#endif
  uint64_t value = install_device(sample_rate, channels, bits);
  AudioDevice *device = get_device(value);
  if (!device) {
    pthread_mutex_unlock(&audio_mutex);
    return MMSYSERR_NOMEM;
  }
#ifdef __ANDROID__
  SLDataLocator_AndroidSimpleBufferQueue queue_locator = {
    SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, AUDIO_QUEUE_CAPACITY
  };
  SLuint32 channel_mask = channels == 1 ?
      SL_SPEAKER_FRONT_CENTER :
      SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
  SLDataFormat_PCM pcm = {
    SL_DATAFORMAT_PCM,
    channels,
    sample_rate * 1000,
    bits == 8 ? SL_PCMSAMPLEFORMAT_FIXED_8 : SL_PCMSAMPLEFORMAT_FIXED_16,
    bits,
    channel_mask,
    SL_BYTEORDER_LITTLEENDIAN
  };
  SLDataSource source = { &queue_locator, &pcm };
  SLDataLocator_OutputMix output_locator = {
    SL_DATALOCATOR_OUTPUTMIX, output_mix_object
  };
  SLDataSink sink = { &output_locator, NULL };
  const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
  const SLboolean required[] = { SL_BOOLEAN_TRUE };
  if ((*engine)->CreateAudioPlayer(engine, &device->player_object, &source,
      &sink, 1, ids, required) != SL_RESULT_SUCCESS)
    goto open_failed;
  if ((*device->player_object)->Realize(device->player_object,
      SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS)
    goto open_failed;
  if ((*device->player_object)->GetInterface(device->player_object,
      SL_IID_PLAY, &device->player) != SL_RESULT_SUCCESS)
    goto open_failed;
  if ((*device->player_object)->GetInterface(device->player_object,
      SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &device->buffer_queue) !=
      SL_RESULT_SUCCESS)
    goto open_failed;
  if ((*device->buffer_queue)->RegisterCallback(device->buffer_queue,
      audio_queue_callback, device) != SL_RESULT_SUCCESS)
    goto open_failed;
  if ((*device->player)->SetPlayState(device->player, SL_PLAYSTATE_PLAYING) !=
      SL_RESULT_SUCCESS)
    goto open_failed;
#endif
  ST(uint64_t, output, value);
  pthread_mutex_unlock(&audio_mutex);
  return MMSYSERR_NOERROR;

#ifdef __ANDROID__
open_failed:
  if (device->player_object) {
    (*device->player_object)->Destroy(device->player_object);
    device->player_object = NULL;
  }
  device->open = 0;
  pthread_mutex_unlock(&audio_mutex);
  return MMSYSERR_ERROR;
#endif
}

SH(waveOutPrepareHeader) {
  AudioDevice *device = get_device(ARG(0));
  uint64_t header = ARG(1);
  uint32_t size = (uint32_t)ARG(2);
  if (!device) return MMSYSERR_INVALHANDLE;
  if (!header || size < 48) return MMSYSERR_INVALPARAM;
  uint32_t flags = LD(uint32_t, header + 24);
  if (flags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
  ST(uint32_t, header + 24, flags | WHDR_PREPARED);
  return MMSYSERR_NOERROR;
}

SH(waveOutUnprepareHeader) {
  AudioDevice *device = get_device(ARG(0));
  uint64_t header = ARG(1);
  uint32_t size = (uint32_t)ARG(2);
  if (!device) return MMSYSERR_INVALHANDLE;
  if (!header || size < 48) return MMSYSERR_INVALPARAM;
  uint32_t flags = LD(uint32_t, header + 24);
  if (!(flags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
  if (flags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
  ST(uint32_t, header + 24, flags & ~(WHDR_PREPARED | WHDR_DONE));
  return MMSYSERR_NOERROR;
}

SH(waveOutWrite) {
  AudioDevice *device = get_device(ARG(0));
  uint64_t header = ARG(1);
  uint32_t size = (uint32_t)ARG(2);
  if (!device) return MMSYSERR_INVALHANDLE;
  if (!header || size < 48) return MMSYSERR_INVALPARAM;
  uint32_t flags = LD(uint32_t, header + 24);
  uint64_t data = LD(uint64_t, header);
  uint32_t length = LD(uint32_t, header + 8);
  if (!(flags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
  if ((flags & WHDR_INQUEUE) || (length && !data))
    return MMSYSERR_INVALPARAM;
  ST(uint32_t, header + 12, 0);
  if (!length) {
    ST(uint32_t, header + 24, (flags & ~WHDR_INQUEUE) | WHDR_DONE);
    return MMSYSERR_NOERROR;
  }

  pthread_mutex_lock(&audio_mutex);
  if (device->queue_count == AUDIO_QUEUE_CAPACITY) {
    pthread_mutex_unlock(&audio_mutex);
    return MMSYSERR_NOMEM;
  }
  uint32_t tail = (device->queue_head + device->queue_count) %
      AUDIO_QUEUE_CAPACITY;
  device->queue[tail].header = header;
  device->queue[tail].converted = NULL;
#ifdef __ANDROID__
  const void *audio_data = (const void *)(uintptr_t)data;
  if (device->bits_per_sample == 8 && length) {
    uint8_t *converted = malloc(length);
    if (!converted) {
      pthread_mutex_unlock(&audio_mutex);
      return MMSYSERR_NOMEM;
    }
    const uint8_t *source = (const uint8_t *)(uintptr_t)data;
    for (uint32_t i = 0; i < length; i++)
      converted[i] = (uint8_t)(source[i] ^ 0x80u);
    device->queue[tail].converted = converted;
    audio_data = converted;
  }
#endif
  device->queue_count++;
  flags = (flags & ~WHDR_DONE) | WHDR_INQUEUE;
  ST(uint32_t, header + 24, flags);
#ifdef __ANDROID__
  SLresult result = (*device->buffer_queue)->Enqueue(device->buffer_queue,
      audio_data, length);
  if (result != SL_RESULT_SUCCESS) {
    device->queue_count--;
    free(device->queue[tail].converted);
    device->queue[tail].converted = NULL;
    ST(uint32_t, header + 24, flags & ~WHDR_INQUEUE);
    pthread_mutex_unlock(&audio_mutex);
    return MMSYSERR_ERROR;
  }
#else
  device->queue_count--;
  flags = (flags & ~WHDR_INQUEUE) | WHDR_DONE;
  ST(uint32_t, header + 12, length);
  ST(uint32_t, header + 24, flags);
#endif
  pthread_mutex_unlock(&audio_mutex);
  return MMSYSERR_NOERROR;
}

SH(waveOutReset) {
  AudioDevice *device = get_device(ARG(0));
  if (!device) return MMSYSERR_INVALHANDLE;
  AudioQueueEntry queued[AUDIO_QUEUE_CAPACITY];
  uint32_t queued_count;
  pthread_mutex_lock(&audio_mutex);
  queued_count = device->queue_count;
  for (uint32_t i = 0; i < queued_count; i++)
    queued[i] = device->queue[(device->queue_head + i) % AUDIO_QUEUE_CAPACITY];
  device->queue_head = 0;
  device->queue_count = 0;
  pthread_mutex_unlock(&audio_mutex);
  int clear_failed = 0;
#ifdef __ANDROID__
  if ((*device->buffer_queue)->Clear(device->buffer_queue) !=
      SL_RESULT_SUCCESS)
    clear_failed = 1;
#endif
  for (uint32_t i = 0; i < queued_count; i++) {
    uint64_t header = queued[i].header;
    uint32_t flags = LD(uint32_t, header + 24);
    ST(uint32_t, header + 24, (flags & ~WHDR_INQUEUE) | WHDR_DONE);
    free(queued[i].converted);
  }
  return clear_failed ? MMSYSERR_ERROR : MMSYSERR_NOERROR;
}

SH(waveOutPause) {
  AudioDevice *device = get_device(ARG(0));
  if (!device) return MMSYSERR_INVALHANDLE;
#ifdef __ANDROID__
  if ((*device->player)->SetPlayState(device->player, SL_PLAYSTATE_PAUSED) !=
      SL_RESULT_SUCCESS)
    return MMSYSERR_ERROR;
#endif
  device->paused = 1;
  return MMSYSERR_NOERROR;
}

SH(waveOutRestart) {
  AudioDevice *device = get_device(ARG(0));
  if (!device) return MMSYSERR_INVALHANDLE;
#ifdef __ANDROID__
  if ((*device->player)->SetPlayState(device->player, SL_PLAYSTATE_PLAYING) !=
      SL_RESULT_SUCCESS)
    return MMSYSERR_ERROR;
#endif
  device->paused = 0;
  return MMSYSERR_NOERROR;
}

SH(waveOutClose) {
  AudioDevice *device = get_device(ARG(0));
  if (!device) return MMSYSERR_INVALHANDLE;
  pthread_mutex_lock(&audio_mutex);
  if (device->queue_count) {
    pthread_mutex_unlock(&audio_mutex);
    return WAVERR_STILLPLAYING;
  }
#ifdef __ANDROID__
  SLObjectItf player_object = device->player_object;
  device->player_object = NULL;
  device->player = NULL;
  device->buffer_queue = NULL;
#endif
  device->open = 0;
  pthread_mutex_unlock(&audio_mutex);
#ifdef __ANDROID__
  if (player_object) (*player_object)->Destroy(player_object);
#endif
  return MMSYSERR_NOERROR;
}

SH(waveOutGetVolume) {
  if (!ARG(1)) return MMSYSERR_INVALPARAM;
  ST(uint32_t, ARG(1), UINT32_MAX);
  return MMSYSERR_NOERROR;
}

SH(waveOutSetVolume) {
  return get_device(ARG(0)) ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}

#define S(name) { #name, sh_##name }
const ShimEnt aot_shim_table_audio[] = {
  S(waveOutGetNumDevs), S(waveOutOpen), S(waveOutPrepareHeader),
  S(waveOutUnprepareHeader), S(waveOutWrite), S(waveOutReset),
  S(waveOutPause), S(waveOutRestart), S(waveOutClose),
  S(waveOutGetVolume), S(waveOutSetVolume),
  { 0, 0 }
};
