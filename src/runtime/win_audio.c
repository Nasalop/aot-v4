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
#define AOT_DS_SRC 24
#define AOT_DS_FRAMES 1024
#define AOT_DS_QSZ 3
#define AOT_DS_FADE 128
/* Volume, pan and 3D distance are applied per source: a crowded street mixes dozens of voices and summing
   them all at full scale clipped, which is what the constant crackle was. A voice also fades in and out
   over ~3 ms, because starting or ending one on a non-zero sample clicks and the game does that constantly. */
typedef struct {
  void *key; const uint8_t *data; uint32_t size, pos; int looping, playing, ch;
  int32_t fadev, fstep;              /* 0..256 gain ramp; fstep < 0 while fading out, 0 when steady */
  float gain, pan;                   /* SetVolume (centibels) and SetPan (-1..1) */
  float x, y, z, min_d, max_d; int is3d;
} DsSrc;
static DsSrc ds_src[AOT_DS_SRC];
static pthread_mutex_t ds_mu = PTHREAD_MUTEX_INITIALIZER;
static SLObjectItf ds_obj; static SLPlayItf ds_play; static SLAndroidSimpleBufferQueueItf ds_queue;
static int ds_freq = 44100, ds_ch = 2, ds_warned;
static int16_t ds_mix[AOT_DS_FRAMES * 2];
static int16_t ds_out[AOT_DS_QSZ][AOT_DS_FRAMES * 2];
static int ds_flip;
/* Listener (IDirectSound3DListener): where the camera is and which way it faces; "right" is derived. */
static float lis_px, lis_py, lis_pz, lis_fx, lis_fy, lis_fz = 1.0f, lis_tx, lis_ty = 1.0f, lis_tz, lis_rolloff = 1.0f;

static DsSrc *ds_find(void *key) {
  for (int i = 0; i < AOT_DS_SRC; i++) if (ds_src[i].key == key) return &ds_src[i];
  return NULL;
}
static float ds_lin(int32_t cb) {   /* DirectSound volume is centibels of attenuation: 0 = full, -10000 = off */
  if (cb >= 0) return 1.0f;
  if (cb <= -6000) return 0.0f;
  float oct = (float)(-cb) * 0.001660964f;   /* -db / 6.0206 dB per octave */
  int ip = (int)oct; float fp = oct - (float)ip, r = 1.0f;
  for (int i = 0; i < ip && i < 32; i++) r *= 0.5f;
  return r * (1.0f - 0.34657f * fp + 0.06178f * fp * fp - 0.00723f * fp * fp * fp);
}
static void ds_gains(DsSrc *s, int32_t *gl, int32_t *gr) {   /* 8.8 fixed point gain per channel */
  float g = s->gain, pan = s->pan;
  if (s->is3d) {
    float dx = s->x - lis_px, dy = s->y - lis_py, dz = s->z - lis_pz;
    float d = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
    float md = s->min_d > 0.01f ? s->min_d : 1.0f;
    float att = d <= md ? 1.0f : md / (md + lis_rolloff * (d - md));
    if (att > 1.0f) att = 1.0f;
    g *= att;
    if (d > 0.0001f) {   /* lateral offset along the listener's right axis, scaled to -1..1 */
      float rx = lis_ty * lis_fz - lis_tz * lis_fy, ry = lis_tz * lis_fx - lis_tx * lis_fz, rz = lis_tx * lis_fy - lis_ty * lis_fx;
      float p = (dx * rx + dy * ry + dz * rz) / d * 1.6f;
      if (p > 1.0f) p = 1.0f; if (p < -1.0f) p = -1.0f;
      pan = p;
    }
  }
  float l = g * (pan > 0 ? 1.0f - pan : 1.0f), r = g * (pan < 0 ? 1.0f + pan : 1.0f);
  *gl = (int32_t)(l * 256.0f); *gr = (int32_t)(r * 256.0f);
}

static void ds_feed(void) {   /* 16-bit stereo at ds_freq: every source at its own gain, then saturate */
  static int32_t acc[AOT_DS_FRAMES * 2];
  memset(acc, 0, sizeof acc);
  for (int i = 0; i < AOT_DS_SRC; i++) {
    DsSrc *s = &ds_src[i];
    if (!s->key || !s->data || !s->size) continue;
    if (!s->playing && s->fstep >= 0) { s->key = NULL; continue; }
    int ch = s->ch == 1 ? 1 : 2; uint32_t frame = (uint32_t)(ch * 2);
    uint32_t sz = s->size - s->size % frame; if (!sz) { s->key = NULL; continue; }
    int32_t gl, gr; ds_gains(s, &gl, &gr);
    for (int k = 0; k < AOT_DS_FRAMES; k++) {
      if (s->pos + frame > sz) {
        if (!s->looping) { s->key = NULL; break; }   /* nothing audible left: free the slot right away */
        s->pos = 0;
      }
      const uint8_t *p = s->data + s->pos;
      int32_t l = (int16_t)(uint16_t)(p[0] | (p[1] << 8));
      int32_t r = ch == 2 ? (int16_t)(uint16_t)(p[2] | (p[3] << 8)) : l;   /* mono duplicated into both ears */
      s->pos += frame;
      if (s->fstep) {
        s->fadev += s->fstep;
        if (s->fadev < 0) s->fadev = 0; else if (s->fadev > 256) { s->fadev = 256; s->fstep = 0; }
        if (!s->playing && s->fadev <= 0) { s->key = NULL; break; }
      }
      l = (l * gl >> 8) * s->fadev >> 8; r = (r * gr >> 8) * s->fadev >> 8;
      acc[2 * k] += l; acc[2 * k + 1] += r;
    }
  }
  for (int k = 0; k < AOT_DS_FRAMES * 2; k++) { int32_t v = acc[k]; ds_mix[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
}
static void ds_queue_cb(SLAndroidSimpleBufferQueueItf q, void *ctx) {
  (void)q; (void)ctx;
  pthread_mutex_lock(&ds_mu);
  ds_feed();
  int16_t *b = ds_out[ds_flip]; memcpy(b, ds_mix, sizeof ds_mix); ds_flip = (ds_flip + 1) % AOT_DS_QSZ;
  pthread_mutex_unlock(&ds_mu);
  if ((*ds_queue)->Enqueue(ds_queue, b, sizeof ds_mix) != SL_RESULT_SUCCESS) {
    static int n; if (!n++) fprintf(stderr, "[audio] DirectSound: the output queue refused a buffer; sound stops here\n");
  }
}
static int ds_open(int freq, int ch) {
  if (ds_obj) return 0;
  ds_freq = freq > 0 ? freq : 44100; ds_ch = ch == 1 ? 1 : 2;
  if (audio_engine_init() != 0) return -1;
  SLDataLocator_AndroidSimpleBufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, AOT_DS_QSZ };
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
  for (int i = 0; i < AOT_DS_QSZ; i++) (*ds_queue)->Enqueue(ds_queue, ds_out[i], sizeof ds_out[i]);
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
  DsSrc *s = ds_find(key);
  int fresh = 0;
  if (!s) {
    for (int i = 0; i < AOT_DS_SRC; i++) if (!ds_src[i].key) { s = &ds_src[i]; break; }
    if (!s) { pthread_mutex_unlock(&ds_mu); return NULL; }
    memset(s, 0, sizeof *s); s->key = key; s->gain = 1.0f; fresh = 1;
  }
  s->data = data; s->size = size; s->ch = channels == 1 ? 1 : 2; s->looping = looping; s->playing = 1;
  if (s->fstep < 0) s->fstep = 256 / AOT_DS_FADE;   /* Play on a voice still fading out: bring it back */
  else if (!s->fstep) s->fadev = 256;
  /* A buffer the game calls Play on every frame must keep playing where it is, not restart from the top. */
  if (fresh) { s->pos = 0; s->fadev = 0; s->fstep = 256 / AOT_DS_FADE; }
  pthread_mutex_unlock(&ds_mu);
  return s;
}
void aot_ds_stop(void *key) {   /* Release: the buffer memory is about to go away, so drop the source now */
  if (!key) return;
  pthread_mutex_lock(&ds_mu);
  DsSrc *s = ds_find(key); if (s) { s->key = NULL; s->playing = 0; s->fstep = 0; s->fadev = 0; }
  pthread_mutex_unlock(&ds_mu);
}
void aot_ds_halt(void *key) {   /* Stop: fade over ~3 ms so ending a voice does not click */
  if (!key) return;
  pthread_mutex_lock(&ds_mu);
  DsSrc *s = ds_find(key);
  if (s) { s->playing = 0; s->fstep = -(256 / AOT_DS_FADE); if (s->fadev <= 0) s->key = NULL; }
  pthread_mutex_unlock(&ds_mu);
}
void aot_ds_seek(void *key, uint32_t pos) {   /* SetCurrentPosition: jump, faded in, instead of restarting */
  if (!key) return;
  pthread_mutex_lock(&ds_mu);
  DsSrc *s = ds_find(key);
  if (s) { uint32_t frame = (uint32_t)((s->ch == 1 ? 2 : 4)); s->pos = pos < s->size ? pos - pos % frame : 0; s->fadev = 0; s->fstep = 256 / AOT_DS_FADE; }
  pthread_mutex_unlock(&ds_mu);
}
void aot_ds_setvol(void *key, int32_t cb, int32_t pan) {   /* SetVolume / SetPan */
  if (!key) return;
  pthread_mutex_lock(&ds_mu);
  DsSrc *s = ds_find(key);
  if (s) { s->gain = ds_lin(cb); s->pan = pan <= -10000 ? -1.0f : pan >= 10000 ? 1.0f : (float)pan / 10000.0f; }
  pthread_mutex_unlock(&ds_mu);
}
void aot_ds_3d(void *key, float x, float y, float z, float min_d, float max_d) {   /* IDirectSound3DBuffer */
  if (!key) return;
  pthread_mutex_lock(&ds_mu);
  DsSrc *s = ds_find(key);
  if (s) { s->x = x; s->y = y; s->z = z; s->min_d = min_d; s->max_d = max_d; s->is3d = 1; }
  pthread_mutex_unlock(&ds_mu);
}
void aot_ds_listener(float px, float py, float pz, float fx, float fy, float fz, float tx, float ty, float tz, float rolloff) {
  pthread_mutex_lock(&ds_mu);
  lis_px = px; lis_py = py; lis_pz = pz; lis_fx = fx; lis_fy = fy; lis_fz = fz;
  lis_tx = tx; lis_ty = ty; lis_tz = tz; lis_rolloff = rolloff > 0.01f ? rolloff : 0.01f;
  pthread_mutex_unlock(&ds_mu);
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
