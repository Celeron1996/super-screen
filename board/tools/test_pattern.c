/*
 * test_pattern.c — 板端自测图案（无网络），用于验证 fb 通路与色彩序
 * See docs/plan-imx6ull.md §3.6
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fb.h"
#include "sssc.h"
#include "util.h"

static void put_pixel(uint8_t *buf, size_t off, int format,
                      int r, int g, int b)
{
    if (format == SSSC_FMT_RGB565) {
        uint16_t p = (uint16_t)(((r >> 3) << 11) |
                                ((g >> 2) << 5) |
                                (b >> 3));

        buf[off] = (uint8_t)(p & 0xFFu);
        buf[off + 1] = (uint8_t)(p >> 8);
    } else {
        buf[off] = (uint8_t)b;
        buf[off + 1] = (uint8_t)g;
        buf[off + 2] = (uint8_t)r;
        buf[off + 3] = 0xFFu;
    }
}

static void fill_rect(uint8_t *buf, int format, int stride,
                      int x, int y, int w, int h, int r, int g, int b)
{
    int row;
    int col;
    int bpp = (format == SSSC_FMT_RGB565) ? 2 : 4;

    for (row = 0; row < h; row++) {
        for (col = 0; col < w; col++) {
            size_t off = (size_t)(y + row) * (size_t)stride +
                         (size_t)(x + col) * (size_t)bpp;

            put_pixel(buf, off, format, r, g, b);
        }
    }
}

static void build_pattern(uint8_t *buf, int format)
{
    static const int bars[8][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255},
        {255, 255, 0}, {0, 255, 255}, {255, 0, 255}, {0, 0, 0}
    };
    static const int gray[8] = {0, 36, 73, 109, 146, 182, 219, 255};
    int bpp = (format == SSSC_FMT_RGB565) ? 2 : 4;
    int stride = (int)SSSC_SCREEN_W * bpp;
    int x;
    int y;

    for (y = 0; y < (int)SSSC_SCREEN_H; y++) {
        if (y < 32) {
            fill_rect(buf, format, stride, 0, y, (int)SSSC_SCREEN_W, 1,
                      255, 255, 255);
        } else if (y < 512) {
            for (x = 0; x < 8; x++) {
                fill_rect(buf, format, stride, x * 128, y, 128, 1,
                          bars[x][0], bars[x][1], bars[x][2]);
            }
        } else if (y < 576) {
            for (x = 0; x < (int)SSSC_SCREEN_W; x += 16) {
                int white = (((x / 16) + (y / 16)) & 1) != 0;

                fill_rect(buf, format, stride, x, y, 16, 1,
                          white ? 255 : 0, white ? 255 : 0, white ? 255 : 0);
            }
        } else {
            for (x = 0; x < (int)SSSC_SCREEN_W; x += 32) {
                int v = gray[(x / 32) % 8];

                fill_rect(buf, format, stride, x, y, 32, 1, v, v, v);
            }
        }
    }
}

static void usage(const char *prog)
{
    printf("usage: %s [--format bgra32|rgb565]\n", prog);
}

int main(int argc, char **argv)
{
    int format = SSSC_FMT_BGRA32;
    size_t size;
    size_t bpp;
    uint8_t *buf;
    fb_t fb;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "bgra32") == 0) {
                format = SSSC_FMT_BGRA32;
            } else if (strcmp(argv[i], "rgb565") == 0) {
                format = SSSC_FMT_RGB565;
            } else {
                LOG_ERR("invalid format: %s", argv[i]);
                return 1;
            }
        } else {
            LOG_ERR("unknown argument: %s", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    bpp = (format == SSSC_FMT_RGB565) ? 2u : 4u;
    size = (size_t)SSSC_SCREEN_W * SSSC_SCREEN_H * bpp;
    buf = malloc(size);
    if (buf == NULL) {
        LOG_ERR("malloc %u bytes failed", (unsigned)size);
        return 2;
    }

    build_pattern(buf, format);

    if (fb_open("/dev/fb0", &fb) != 0) {
        free(buf);
        return 2;
    }

    fb_apply_tile(&fb, 0, 0, (int)SSSC_SCREEN_W, (int)SSSC_SCREEN_H,
                  buf, format);
    LOG_INFO("test pattern applied (%s)",
             format == SSSC_FMT_RGB565 ? "rgb565" : "bgra32");

    fb_close(&fb);
    free(buf);
    return 0;
}
