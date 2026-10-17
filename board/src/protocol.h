/*
 * protocol.h — SSSC v1 连接内状态与消息分发
 * See docs/PLAN.md §5 与 docs/plan-imx6ull.md §3.3
 */
#ifndef SUPER_SCREEN_PROTOCOL_H
#define SUPER_SCREEN_PROTOCOL_H

#include <stdint.h>

#include "fb.h"

typedef struct {
    fb_t    *fb;
    uint64_t rx_bytes, tx_bytes;
    uint32_t tiles_applied, frames, full_refreshes, backlog_hits;
    int      force_format;      /* -1=不强制, 0/1=强制 */

    /* 以下为连接内状态，不属于协议字段 */
    int      format;
    uint8_t  fps_cap;
    int      hello_done;
    uint16_t width, height;

    int      backlog_streak;
    int      win_active;
    uint64_t win_start_ms;
    int      win_full_active;
    int      in_full;
    int      have_seq;
    uint32_t last_seq;
    uint32_t win_full_drops;
    uint32_t win_full_tiles;
    uint64_t last_tiles_ms;

    uint64_t stats_ms;
    uint64_t stats_rx_bytes;
    uint32_t stats_frames;
    uint32_t stats_tiles;
} sssc_ctx_t;

/* 初始化/复位连接上下文；force_format: -1=auto, 0=BGRA32, 1=RGB565。 */
void sssc_ctx_init(sssc_ctx_t *ctx, fb_t *fb, int force_format);

/* 收包+分发主循环；0=正常断开，<0=协议/IO 错误。 */
int sssc_serve_conn(int fd, sssc_ctx_t *ctx);

/* 每 5s 调用一次；未满 5s 不输出。 */
void sssc_stats_log(sssc_ctx_t *ctx);

#endif /* SUPER_SCREEN_PROTOCOL_H */
