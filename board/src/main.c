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
    printf("usage: %s [--port N] [--format auto|bgra32|rgb565] [--log FILE]\n",
           prog);
    printf("  --port N      listen port (default %u)\n", SSSC_PORT);
    printf("  --format F    force output format: auto|bgra32|rgb565 "
           "(default auto)\n");
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
        sssc_ctx_init(&ctx, &fb, force_format);
        (void)sssc_serve_conn(cfd, &ctx);
        close(cfd);
        LOG_INFO("client disconnected");
    }
}
