/*
 * fb.h — /dev/fb0 映射与 tile 写入
 * See docs/PLAN.md §5.4 与 docs/plan-imx6ull.md §3.4
 */
#ifndef SUPER_SCREEN_FB_H
#define SUPER_SCREEN_FB_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int      fd;
    uint8_t *base;
    size_t   map_len;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t bpp;
} fb_t;

/* 打开并校验 1024x600x32 framebuffer；成功返回 0，失败返回 -1。 */
int fb_open(const char *path, fb_t *fb);

/* 防御性裁剪后写 tile；format 取 enum sssc_format。 */
void fb_apply_tile(fb_t *fb, int x, int y, int w, int h,
                   const void *data, int format);

/* 整屏清黑（目标分辨率小于物理屏时用于四周黑边）。 */
void fb_clear(fb_t *fb);

void fb_close(fb_t *fb);

#endif /* SUPER_SCREEN_FB_H */
