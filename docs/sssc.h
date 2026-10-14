/*
 * SSSC (Super Screen Streaming Contract) v1 — 规范定义文件（唯一权威）
 *
 * 契约正文：docs/PLAN.md §5。本文件是两端（board/ 与 windows/）唯一权威类型定义：
 *   - 板端：原样复制到 board/src/sssc.h，禁止修改字段/常量。
 *   - Windows 端：原样复制到 windows/src/sssc.h，禁止修改字段/常量。
 *   - 任何变更：先改 docs/PLAN.md §5 与本文件（升版本号），再同步两端。
 *
 * 兼容：C99+ / C++11+；MSVC 与 GCC/Clang 均可编译。
 */
#ifndef SSSC_H
#define SSSC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 全局常量（冻结） ---------------- */

#define SSSC_MAGIC              0x43535353u  /* 内存字节序 "SSSC"：'S','S','S','C' */
#define SSSC_VERSION            1u
#define SSSC_PORT               5901u

#define SSSC_SCREEN_W           1024u
#define SSSC_SCREEN_H           600u
#define SSSC_TILE_SIZE          64u
#define SSSC_GRID_COLS          16u          /* 1024 / 64 */
#define SSSC_GRID_ROWS          10u          /* ceil(600 / 64)，底行高 24px */
#define SSSC_TILE_MAX_RAW       16384u       /* 64*64*4，接收端 LZ4 解压缓冲 */

#define SSSC_FPS_CAP_DEFAULT    20u
#define SSSC_FPS_CAP_MIN        5u
#define SSSC_FULL_REFRESH_SEC   5u
#define SSSC_FULL_DIRTY_THRESH  120u         /* 连续 3 帧脏块 > 120 触发全刷 */

#define SSSC_MAX_MSG_PAYLOAD    (64u * 1024u)     /* 发送端：单条消息 payload 上限 */
#define SSSC_MAX_MSG_ACCEPT     (256u * 1024u)    /* 接收端：头部 length 合法上限 */
#define SSSC_RECV_BUF_SIZE      (2u * 1024u * 1024u)
#define SSSC_BACKLOG_QUALITY    (1u * 1024u * 1024u)  /* 板端积压 → SET_QUALITY */
#define SSSC_SEND_QUEUE_SKIP    (256u * 1024u)        /* Win 端背压 → 跳过本轮 */

#define SSSC_HEARTBEAT_SEC      5u
#define SSSC_HEARTBEAT_TIMEOUT  15u

/* ---------------- 枚举 ---------------- */

enum sssc_type {
    SSSC_TYPE_HELLO       = 0x0001,  /* Win -> 板 */
    SSSC_TYPE_HELLO_ACK   = 0x0002,  /* 板 -> Win */
    SSSC_TYPE_TILES       = 0x0010,  /* Win -> 板 */
    SSSC_TYPE_FULL_BEGIN  = 0x0011,  /* Win -> 板 */
    SSSC_TYPE_FULL_END    = 0x0012,  /* Win -> 板 */
    SSSC_TYPE_SET_QUALITY = 0x0030,  /* 板 -> Win */
    SSSC_TYPE_PING        = 0x0040,  /* Win -> 板 */
    SSSC_TYPE_PONG        = 0x0041   /* 板 -> Win */
};

enum sssc_format { SSSC_FMT_BGRA32 = 0, SSSC_FMT_RGB565 = 1 };
enum sssc_codec  { SSSC_CODEC_RAW = 0, SSSC_CODEC_LZ4 = 1 };
enum sssc_status { SSSC_STATUS_OK = 0 };

/* ---------------- 消息结构（1 字节对齐） ---------------- */

#if defined(_MSC_VER)
#  define SSSC_PACKED
#  pragma pack(push, 1)
#else
#  define SSSC_PACKED __attribute__((packed))
#endif

typedef struct {                 /* 12B，所有消息的公共头 */
    uint32_t magic;              /* SSSC_MAGIC */
    uint16_t version;            /* SSSC_VERSION */
    uint16_t type;               /* enum sssc_type */
    uint32_t length;             /* payload 字节数（不含本头） */
} sssc_hdr;

typedef struct SSSC_PACKED {     /* 10B */
    uint16_t width;
    uint16_t height;
    uint8_t  format;             /* enum sssc_format（发送方建议值） */
    uint8_t  fps_cap;
    uint32_t flags;              /* 恒 0 */
} sssc_hello;

typedef struct SSSC_PACKED {     /* 10B */
    uint16_t width;
    uint16_t height;
    uint8_t  format;             /* 板端最终 format */
    uint8_t  status;             /* enum sssc_status */
    uint16_t tile_size;          /* = SSSC_TILE_SIZE */
    uint16_t reserved;           /* 0 */
} sssc_hello_ack;

typedef struct SSSC_PACKED {     /* 8B，后接 count 个 sssc_tile_hdr+data */
    uint32_t seq;
    uint16_t count;
    uint16_t reserved;           /* 0 */
} sssc_tiles_hdr;

typedef struct SSSC_PACKED {     /* 8B，FULL_BEGIN / FULL_END 共用 */
    uint32_t seq;
    uint32_t reserved;           /* 0 */
} sssc_full_hdr;

typedef struct SSSC_PACKED {     /* 4B */
    uint8_t  format;             /* 建议 enum sssc_format */
    uint8_t  fps_cap;            /* 建议值，最小 SSSC_FPS_CAP_MIN */
    uint16_t reserved;           /* 0 */
} sssc_set_quality;

typedef struct SSSC_PACKED {     /* 4B，PING / PONG 共用 */
    uint32_t timestamp;          /* 发送方毫秒级单调计数；PONG 原样回显 */
} sssc_ping;

typedef struct SSSC_PACKED {     /* 14B，后接 data_len 字节 */
    uint16_t x;
    uint16_t y;
    uint16_t w;                  /* <= 64 */
    uint16_t h;                  /* <= 64 */
    uint8_t  codec;              /* enum sssc_codec */
    uint8_t  reserved;           /* 0 */
    uint32_t data_len;           /* 压缩后字节数 */
} sssc_tile_hdr;

#if defined(_MSC_VER)
#  pragma pack(pop)
#endif

/* ---------------- 工具宏 ---------------- */

/* 单像素字节数 */
#define SSSC_BPP(fmt) \
    (((fmt) == SSSC_FMT_RGB565) ? 2u : 4u)

/* tile 解压后字节数；接 sssc_format 值 */
#define SSSC_TILE_RAW_BYTES(w, h, fmt) \
    ((size_t)(w) * (size_t)(h) * SSSC_BPP(fmt))

/* 接收端正确性断言（编译期） */
#if defined(__cplusplus)
#  define SSSC_STATIC_ASSERT(c, m) static_assert((c), m)
#else
#  define SSSC_STATIC_ASSERT(c, m) _Static_assert((c), m)
#endif

SSSC_STATIC_ASSERT(sizeof(sssc_hdr)        == 12, "sssc_hdr must be 12 bytes");
SSSC_STATIC_ASSERT(sizeof(sssc_hello)      == 10, "sssc_hello must be 10 bytes");
SSSC_STATIC_ASSERT(sizeof(sssc_hello_ack)  == 10, "sssc_hello_ack must be 10 bytes");
SSSC_STATIC_ASSERT(sizeof(sssc_tiles_hdr)  ==  8, "sssc_tiles_hdr must be 8 bytes");
SSSC_STATIC_ASSERT(sizeof(sssc_full_hdr)   ==  8, "sssc_full_hdr must be 8 bytes");
SSSC_STATIC_ASSERT(sizeof(sssc_set_quality)==  4, "sssc_set_quality must be 4 bytes");
SSSC_STATIC_ASSERT(sizeof(sssc_ping)       ==  4, "sssc_ping must be 4 bytes");
SSSC_STATIC_ASSERT(sizeof(sssc_tile_hdr)   == 14, "sssc_tile_hdr must be 14 bytes");
SSSC_STATIC_ASSERT(offsetof(sssc_tile_hdr, data_len) == 10, "tile data_len offset");
SSSC_STATIC_ASSERT(SSSC_TILE_MAX_RAW == (SSSC_TILE_SIZE * SSSC_TILE_SIZE * 4), "tile raw size");

#ifdef __cplusplus
}
#endif

#endif /* SSSC_H */
