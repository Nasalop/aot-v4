#pragma once
#include <stddef.h>
typedef struct { const char *name, *value; } AotHlslMacro;
/* 0 = ok (*code malloc'd DXBC, *log may hold warnings), <0 = failure (*log holds the reason; -2 = compiler library not available). */
int aot_hlsl_compile(const char *src, size_t len, const char *name, const AotHlslMacro *macros, size_t nmacros, const char *entry, const char *profile,
                     unsigned flags, void **code, size_t *size, char **log);
