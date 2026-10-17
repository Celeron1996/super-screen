/*
 * main.c — 参数解析、fb 初始化、listen/accept 主循环
 * See docs/plan-imx6ull.md §3.1
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fb.h"
#include "net.h"
#include "protocol.h"
#include "sssc.h"
#include "util.h"

static void usage(const char *prog)
{
    printf("usage: %s [--port N] [--format auto|bgra32|rgb565] "
           "[--resolution WxH] [--log FILE]\n", prog);
    printf("  --port N      listen port (default %u)\n", SSSC_PORT);
    printf("  --format F    force output format: auto|bgra32|rgb565 "
           "(default auto)\n");
    printf("  --resolution WxH  target display size (default %ux%u, "
           "must not exceed the panel)\n", SSSC_SCREEN_W, SSSC_SCREEN_H);
    printf("  --log FILE    append logs to FILE (default stdout)\n");
}

static int parse_format(const char *s)
{
    if (strcmp(s, "auto") == 0) {
        return -1;
    }
    if (strcmp(s, "bgra32") == 0) {
        return SSSC_FMT_BGRA32;
    }
    if (strcmp(s, "rgb565") == 0) {
        return SSSC_FMT_RGB565;
    }
    return -2;
}

int main(int argc, char **argv)
{
    uint16_t port = SSSC_PORT;
    int force_format = -1;
    uint16_t target_w = SSSC_SCREEN_W;
    uint16_t target_h = SSSC_SCREEN_H;
    const char *log_path = NULL;
    fb_t fb;
    int lfd;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);

            if (v < 1 || v > 65535) {
                LOG_ERR("invalid port: %s", argv[i]);
                return 1;
            }
            port = (uint16_t)v;
        } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            force_format = parse_format(argv[++i]);
            if (force_format == -2) {
                LOG_ERR("invalid format: %s", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--resolution") == 0 && i + 1 < argc) {
            unsigned int w = 0;
            unsigned int h = 0;

            if (sscanf(argv[++i], "%ux%u", &w, &h) != 2 || w == 0 || h == 0 ||
                w > 65535u || h > 65535u) {
                LOG_ERR("invalid resolution: %s", argv[i]);
                return 1;
            }
            target_w = (uint16_t)w;
            target_h = (uint16_t)h;
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else {
            LOG_ERR("unknown argument: %s", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (log_path != NULL) {
        if (freopen(log_path, "a", stdout) == NULL) {
            fprintf(stderr, "board ERR open log %s failed: %s\n",
                    log_path, strerror(errno));
            return 2;
        }
        if (freopen(log_path, "a", stderr) == NULL) {
            fprintf(stderr, "board ERR open log %s failed: %s\n",
                    log_path, strerror(errno));
            return 2;
        }
        setvbuf(stdout, NULL, _IOLBF, 0);
    }

    signal(SIGPIPE, SIG_IGN);

    if (fb_open("/dev/fb0", &fb) != 0) {
        return 2;
    }
    if (target_w > fb.width || target_h > fb.height) {
        LOG_ERR("resolution %ux%u exceeds panel %ux%u", target_w, target_h,
                fb.width, fb.height);
        fb_close(&fb);
        return 1;
    }
    LOG_INFO("target resolution %ux%u (panel %ux%u)", target_w, target_h,
             fb.width, fb.height);
    lfd = net_listen(port);
    if (lfd < 0) {
        fb_close(&fb);
        return 2;
    }

    for (;;) {
        sssc_ctx_t ctx;
        int cfd = net_accept(lfd);

        if (cfd < 0) {
            LOG_WARN("accept failed: %s", strerror(errno));
            sleep(1);
            continue;
        }

        LOG_INFO("client connected");
        sssc_ctx_init(&ctx, &fb, force_format, target_w, target_h);
        (void)sssc_serve_conn(cfd, &ctx);
        close(cfd);
        LOG_INFO("client disconnected");
    }
}
