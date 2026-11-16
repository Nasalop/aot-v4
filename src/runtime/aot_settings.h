#pragma once
#include <stdint.h>
/* Settings from the on-screen EDIT menu: frame-rate cap and the upscaler.
   The upscaler is SGSR2 when the vendor library is present in the build; otherwise it is the same idea without the
   vendor pass: the game gets a smaller client area (render_pct), renders into a smaller back buffer, and the display
   scales it up with the compositor's hardware bilinear filter. Off means the game sees the native size and nothing
   about rendering changes. */
void aot_settings_init(void);
void aot_settings_set(int fps_cap, int upscale, int quality);   /* fps_cap: 0 = off, 30, 60; upscale: 0 = off; quality: 0..4 */
int  aot_settings_fps(void);                                    /* 0 = uncapped */
int  aot_settings_scale(uint32_t *w, uint32_t *h);              /* applies the render scale to a client size, returns the percentage used (100 = native) */
void aot_settings_report(void);                                 /* logs the current choice once */
