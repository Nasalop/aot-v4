#define _GNU_SOURCE
#include "../src/runtime/win_audio.c"

#include <assert.h>
#include <stdio.h>

const char *g_exe_path = "game.exe";
char g_cmdline[1024] = "game.exe";
uint64_t g_image_base;
uint32_t aot_last_error;

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
  for (const ShimEnt *shim = aot_shim_table_audio; shim->name; shim++)
    if (!strcmp(shim->name, name)) return shim->fn(&cpu);
  assert(!"missing audio shim");
  return UINT64_MAX;
}

int main(void) {
  assert(invoke("waveOutGetNumDevs", NULL, 0) == 1);
  uint8_t format[18] = { 0 };
  ST(uint16_t, format, WAVE_FORMAT_PCM);
  ST(uint16_t, format + 2, 1);
  ST(uint32_t, format + 4, 8000);
  ST(uint32_t, format + 8, 8000);
  ST(uint16_t, format + 12, 1);
  ST(uint16_t, format + 14, 8);

  uint64_t queried = invoke("waveOutOpen",
      (uint64_t[]){ 0, WAVE_MAPPER, (uint64_t)(uintptr_t)format, 0, 0,
        WAVE_FORMAT_QUERY }, 6);
  assert(queried == MMSYSERR_NOERROR);

  uint64_t output = 0;
  assert(invoke("waveOutOpen",
      (uint64_t[]){ (uint64_t)(uintptr_t)&output, WAVE_MAPPER,
        (uint64_t)(uintptr_t)format, 0, 0, CALLBACK_NULL }, 6) ==
      MMSYSERR_NOERROR);
  assert(get_device(output));

  uint8_t samples[32];
  memset(samples, 0x80, sizeof samples);
  uint8_t header[48] = { 0 };
  ST(uint64_t, header, (uint64_t)(uintptr_t)samples);
  ST(uint32_t, header + 8, sizeof samples);
  assert(invoke("waveOutPrepareHeader",
      (uint64_t[]){ output, (uint64_t)(uintptr_t)header, sizeof header }, 3) ==
      MMSYSERR_NOERROR);
  assert(LD(uint32_t, header + 24) & WHDR_PREPARED);
  assert(invoke("waveOutWrite",
      (uint64_t[]){ output, (uint64_t)(uintptr_t)header, sizeof header }, 3) ==
      MMSYSERR_NOERROR);
#ifndef __ANDROID__
  assert(LD(uint32_t, header + 24) & WHDR_DONE);
  assert(LD(uint32_t, header + 12) == sizeof samples);
#endif
  assert(invoke("waveOutPause", (uint64_t[]){ output }, 1) ==
      MMSYSERR_NOERROR);
  assert(invoke("waveOutRestart", (uint64_t[]){ output }, 1) ==
      MMSYSERR_NOERROR);
  assert(invoke("waveOutUnprepareHeader",
      (uint64_t[]){ output, (uint64_t)(uintptr_t)header, sizeof header }, 3) ==
      MMSYSERR_NOERROR);

  uint32_t volume = 0;
  assert(invoke("waveOutGetVolume",
      (uint64_t[]){ output, (uint64_t)(uintptr_t)&volume }, 2) ==
      MMSYSERR_NOERROR);
  assert(volume == UINT32_MAX);
  assert(invoke("waveOutSetVolume", (uint64_t[]){ output, UINT32_MAX }, 2) ==
      MMSYSERR_NOERROR);
  assert(invoke("waveOutClose", (uint64_t[]){ output }, 1) ==
      MMSYSERR_NOERROR);
  assert(invoke("waveOutClose", (uint64_t[]){ output }, 1) ==
      MMSYSERR_INVALHANDLE);
  aot_win_audio_cleanup();
  puts("win_audio waveOut lifecycle: PASS");
  return 0;
}
