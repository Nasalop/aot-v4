#pragma once
#include <android/native_window.h>
typedef struct TriCtx TriCtx;
TriCtx *tri_create(void);
int tri_init(TriCtx *, ANativeWindow *);   /* 0 = ok */
int tri_draw(TriCtx *, float t);           /* 0 ok, 1 swapchain was stale (call tri_recreate), <0 fatal */
int tri_recreate(TriCtx *);                /* 0 ok */
void tri_destroy(TriCtx *);
void tri_log(const char *fmt, ...);
void tri_open_log(const char *path);
