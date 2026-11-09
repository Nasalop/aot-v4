#pragma once
#include <stdint.h>
/* Platform -> Win32 input bridge. Thread-safe; call from the Android UI thread. */
typedef struct { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; } AotPad;   /* XINPUT_GAMEPAD layout (buttons: 1 up,2 down,4 left,8 right,0x10 start,0x20 back,0x40 L3,0x80 R3,0x100 LB,0x200 RB,0x1000 A,0x2000 B,0x4000 X,0x8000 Y) */
void aot_input_view(uint32_t w, uint32_t h);               /* size of the physical touch surface */
void aot_input_mouse(int kind, float x, float y);          /* 0 move,1 L down,2 L up,3 R down,4 R up */
void aot_input_key(int vk, int down);
void aot_input_pad(const AotPad *p);                       /* NULL = disconnected */
void aot_input_quit(void);
void aot_input_dik(uint8_t out[256]);                       /* DirectInput keyboard state (DIK scancodes, 0x80 = down) */
void aot_input_dimouse(int32_t *dx, int32_t *dy, uint8_t b[3]); /* DirectInput relative mouse: delta since last call + buttons */
