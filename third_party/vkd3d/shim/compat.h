#pragma once
#include <stddef.h>
#ifndef CONTAINING_RECORD
#define CONTAINING_RECORD(a, t, f) ((t *)((char *)(a) - offsetof(t, f)))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef D3DVS_VERSION
#define D3DVS_VERSION(major, minor) (0xfffe0000u | ((major) << 8) | (minor))
#define D3DPS_VERSION(major, minor) (0xffff0000u | ((major) << 8) | (minor))
#endif
