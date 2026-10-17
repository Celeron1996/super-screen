/*
 * fb.c — framebuffer 映射、tile 裁剪与 RGB565 转换
 * See docs/PLAN.md §5.4 与 docs/plan-imx6ull.md §3.4
 */
#include "fb.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sssc.h"
#include "util.h"

int fb_open(const char *path, fb_t *fb)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    size_t map_len;
    uint32_t map_rows;

    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;

    fb->fd = open(path, O_RDWR);
    if (fb->fd < 0) {
        LOG_ERR("open %s failed: %s", path, strerror(errno));
        return -1;
    }

    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
        LOG_ERR("FBIOGET_VSCREENINFO failed: %s", strerror(errno));
        goto fail;
    }
    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) != 0) {
        LOG_ERR("FBIOGET_FSCREENINFO failed: %s", strerror(errno));
        goto fail;
    }

    if (vinfo.xres != SSSC_SCREEN_W || vinfo.yres != SSSC_SCREEN_H ||
        vinfo.bits_per_pixel != 32) {
        LOG_ERR("unsupported fb mode %ux%u@%ubpp (need %ux%u@32bpp)",
                vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,
                SSSC_SCREEN_W, SSSC_SCREEN_H);
        goto fail;
    }

    map_rows = vinfo.yres_virtual > vinfo.yres ? vinfo.yres_virtual : vinfo.yres;
    map_len = (size_t)finfo.line_length * map_rows;
    fb->base = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                    fb->fd, 0);
    if (fb->base == MAP_FAILED) {
        fb->base = NULL;
        LOG_ERR("mmap fb failed: %s", strerror(errno));
        goto fail;
    }

    fb->map_len = map_len;
    fb->width = vinfo.xres;
    fb->height = vinfo.yres;
    fb->stride = finfo.line_length;
    fb->bpp = vinfo.bits_per_pixel / 8;

    LOG_INFO("fb %s %ux%u stride=%u", path, fb->width, fb->height, fb->stride);
    return 0;

fail:
    if (fb->fd >= 0) {
        close(fb->fd);
        fb->fd = -1;
    }
    return -1;
}

void fb_apply_tile(fb_t *fb, int x, int y, int w, int h,
                   const void *data, int format)
{
    int sx = 0;
    int sy = 0;
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;
    int rows;
    int cols;
    int row;
    int bpp = (format == SSSC_FMT_RGB565) ? 2 : 4;
    const uint8_t *src = (const uint8_t *)data;

    if (fb == NULL || fb->base == NULL || data == NULL || w <= 0 || h <= 0) {
        return;
    }
    if (x0 < 0) {
        sx = -x0;
        x0 = 0;
    }
    if (y0 < 0) {
        sy = -y0;
        y0 = 0;
    }
    if (x1 > (int)fb->width) {
        x1 = (int)fb->width;
    }
    if (y1 > (int)fb->height) {
        y1 = (int)fb->height;
    }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    rows = y1 - y0;
    cols = x1 - x0;
    for (row = 0; row < rows; row++) {
        const uint8_t *s = src + ((size_t)(sy + row) * (size_t)w +
                                  (size_t)sx) * (size_t)bpp;
        uint8_t *d = fb->base + (size_t)(y0 + row) * fb->stride +
                     (size_t)x0 * 4u;

        if (format == SSSC_FMT_RGB565) {
            int col;

            for (col = 0; col < cols; col++) {
                uint16_t p = (uint16_t)(s[col * 2] |
                                        ((uint16_t)s[col * 2 + 1] << 8));
                uint32_t r = (p >> 11) & 0x1Fu;
                uint32_t g = (p >> 5) & 0x3Fu;
                uint32_t b = p & 0x1Fu;
                uint32_t out = (((r << 3) | (r >> 2)) << 16) |
                               (((g << 2) | (g >> 4)) << 8) |
                               ((b << 3) | (b >> 2));

                d[col * 4] = (uint8_t)(out & 0xFFu);
                d[col * 4 + 1] = (uint8_t)((out >> 8) & 0xFFu);
                d[col * 4 + 2] = (uint8_t)((out >> 16) & 0xFFu);
                d[col * 4 + 3] = 0xFFu;
            }
        } else {
            memcpy(d, s, (size_t)cols * 4u);
        }
    }
}

void fb_clear(fb_t *fb)
{
    if (fb == NULL || fb->base == NULL) {
        return;
    }
    memset(fb->base, 0, (size_t)fb->stride * fb->height);
}

void fb_close(fb_t *fb)
{
    if (fb == NULL) {
        return;
    }
    if (fb->base != NULL && fb->map_len > 0) {
        munmap(fb->base, fb->map_len);
    }
    if (fb->fd >= 0) {
        close(fb->fd);
    }
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;
}
