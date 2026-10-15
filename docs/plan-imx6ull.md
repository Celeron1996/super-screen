# 板端计划：IMX6ULL 接收客户端

> 三份计划的第 2 份。总计划与 **SSSC 协议契约**见 `docs/PLAN.md`，本文件不重复定义协议。

## 0. 接手速览（TL;DR，零上下文可直接开工）

- **你的角色**：Ubuntu 端开发者（大模型），负责板端接收客户端 + 主机自测工具。
- **你要交付**：`board/`（客户端、test_pattern、S99 脚本）与 `tools/mock_sender.py`（Python，Ubuntu 运行）。
- **前置阅读**：`docs/PLAN.md` §5（协议）与 §5.6/§5.7（唯一权威 `docs/sssc.h` + 边界规则）、§9（板端底数）。本文档给出函数级设计与逐条命令。
- **第一步**（仓库根目录下执行；先做环境准备，按 §2–§6 写出源码与 Makefile 骨架后再跑构建自检）：

```bash
# 环境准备（仓库根）
mkdir -p board/src board/tools board/scripts board/build tools
cp docs/sssc.h board/src/sssc.h          # 原样复制，勿改

# 编写完成后自检（需要 Makefile 与源码已存在）
cd board && make HOST=1                  # 主机侧语法检查（无需板子）
make && file build/super_screen_client   # 交叉编译，确认输出为 ARM ELF
```

- **完成定义**：§8 的 B1–B7 全过；其中 B3–B7 由你亲手实现的 `mock_sender.py` 驱动。
- **仓库协作（强制）**：在自己的开发分支 `dev/linux` 上开发，**禁止直接提交/推送 `main`**；达到里程碑且验收全过后发起 PR 合并。完整规则（分支、PR 模板、提交规范、冲突处理）见 `docs/PLAN.md` §8.1。
- **代码规范（强制）**：C99、4 空格/LF、命名、错误处理、日志格式（`STATS`/`lat_probe`）、零警告要求见 `docs/PLAN.md` §8.2。
- **不做**：修改协议字段/常量、触摸回传、音频、Qt、引入板上不存在的库（允许 vendored LZ4 与 libc/libm）。

## 1. 目标与范围

- 实现 `super_screen_client`：TCP 监听 5901，接收 Windows 发送的 `TILES`，解码 LZ4/raw，只把变化块写入 `/dev/fb0`（1024x600x32bpp，只写变化区域）。
- 实现 `test_pattern`（无网络自测）与开机自启脚本。
- **不做**：触摸回传、音频、Qt 界面、视频解码（协议只承载 BGRA32/RGB565 块）。

## 2. 交付物布局

```
board/
  Makefile                  # 交叉编译：arm-buildroot-linux-gnueabihf-gcc -O2 -Wall -Wextra -mfpu=neon
  src/sssc.h                # 从 docs/sssc.h 原样复制（唯一权威类型定义）
  src/main.c                # 参数解析、fb 初始化、listen/accept 主循环、重连
  src/net.c / net.h         # TCP server、流式收包缓冲、超时、PONG/心跳管理
  src/protocol.c / .h       # SSSC v1 头解析、TILES tile 遍历、消息分发
  src/fb.c / fb.h           # /dev/fb0 open/FBIOGET_*/mmap、apply_tile、RGB565→BGRA
  src/lz4/lz4.c / lz4.h     # vendored LZ4 v1.9.4（板端无 liblz4）
  tools/test_pattern.c      # 直接写 fb 的自测图（--format bgra32|rgb565，图案见 §3.6）
  scripts/S99superscreen    # 开机自启（见 §5）
```

## 3. 模块设计

### 3.0 函数级接口（两端联调以本表签名为准）

```c
/* fb.h */
int  fb_open(const char *path, fb_t *fb);        /* 校验 1024x600x32；mmap；fb->stride=line_length */
void fb_apply_tile(fb_t *fb, int x, int y, int w, int h,
                   const void *data, int format);/* 防御裁剪；BGRA 直拷 / RGB565 逐像素转换 */
void fb_close(fb_t *fb);

/* net.h */
int  net_listen(uint16_t port);                  /* SO_REUSEADDR；返回 listen fd */
int  net_accept(int lfd);                        /* 阻塞 accept；失败 -1 */
int  net_recv_some(int fd, void *buf, size_t cap); /* 一次 recv；>0 字节数，0=对端关闭，-1=错误/超时 */
int  net_pending(int fd);                        /* ioctl(FIONREAD)：内核收包队列待读字节数；失败 -1 */
int  net_send_all(int fd, const void *buf, size_t len); /* 循环 send，仅用于 HELLO_ACK/PONG 小包 */

/* protocol.h */
typedef struct {
    fb_t    *fb;
    uint64_t rx_bytes, tx_bytes;
    uint32_t tiles_applied, frames, full_refreshes, backlog_hits;
    int      force_format;      /* -1=不强制, 0/1=强制 */
} sssc_ctx_t;

int  sssc_serve_conn(int fd, sssc_ctx_t *ctx);   /* 收包+分发主循环；0=正常断开，<0=错误 */
void sssc_stats_log(sssc_ctx_t *ctx);            /* 每 5s 调用，打印一行统计 */

/* LZ4 用法（vendored）： */
/* LZ4_decompress_safe(src, dst, src_len, SSSC_TILE_MAX_RAW)，返回值必须 == w*h*bpp */
```

### 3.1 main.c

```
main:
  解析参数（--port 5901 --format auto --log /tmp/superscreen.log；format=auto|bgra32|rgb565，auto 尊重 HELLO.format）
  fb_open() 校验 1024x600x32，mmap
  listen socket（SO_REUSEADDR）
  for (;;) {
    c = accept(); 登录 connected
    serve(c);      // 阻塞直到断线/超时/解析错误
    close(c); 登录 disconnected
    reset 统计与状态
  }
```

### 3.2 net.c — 流式收包

- 2MB 线性缓冲 + 读写指针；`recv()` 循环，按需 compact（不动态扩容；单条消息 ≤ `SSSC_MAX_MSG_ACCEPT`(256KB)，2MB 必然容纳）。
- 状态机：凑满 12B 公共头 → 校验 magic/version/`length ≤ 256KB` → 等 payload → 交 `protocol_dispatch`。
- `SO_RCVTIMEO = 15s`：超时即视为心跳丢失，断开回 listen。
- 发送路径仅小包（`HELLO_ACK`/`PONG`，可直接 `send`）。

### 3.3 protocol.c — 分发

| 收到 | 行为 |
|---|---|
| HELLO | 宽高须为支持的显示模式（1024x600；M3 起含 800x480），否则回 `HELLO_ACK(status=1)` 后断开；最终 format = 本地强制值（`--format` 非 auto 时）否则 HELLO.format；回 HELLO_ACK（原样回显宽高、tile_size=64、status=0）；重置本连接统计 |
| TILES | 遍历 tile：`codec=0` 先校验 `data_len == w*h*bpp` 再 `fb_apply_tile`；`codec=1` 先用 `LZ4_decompress_safe` 解到 16KB 暂存（`SSSC_TILE_MAX_RAW`），解压长度须 == `w*h*bpp`（bpp 按本连接最终 format）；任一校验不符即断连触发全刷自愈（格式切换窗口内例外，见下） |
| FULL_BEGIN/END | 重置/记录全刷统计 |
| PING | 回 PONG（原样回显 timestamp） |
| 其他/未知 | 记录一次警告并忽略（向前兼容） |

- 接收积压检测：用户态缓冲即收即消费，积压只体现在**内核收包队列**——每处理完一条消息用 `net_pending()`（`ioctl(FIONREAD)`）采样待读字节数，连续 3 次 > `SSSC_BACKLOG_QUALITY`(1MB) → 发 `SET_QUALITY`（首次 RGB565；已 RGB565 仍拥塞则 fps 减半，下限 5；强制 format 时 format 字段恒为强制值、只降 fps）。发出后本连接后续 TILES 立即按新 format 解码；随后计数清零重新累计，`backlog_hits` 递增。
- 格式切换窗口：发出 `SET_QUALITY` 后置标志，收到下一个 `FULL_END` 时清除。窗口内长度校验不符的 tile 视为在途旧格式帧——丢弃并计 WARN，不触发断连；窗口外维持严格校验（不符即断连自愈）。TCP 有序保证旧格式帧必然先于新格式全刷到达，窗口在 `FULL_END` 关闭后不会误伤新数据。
- 板端不发送 `HELLO`/`TILES`/`FULL_*`；收到的 `SET_QUALITY`/`PONG` 若出现则忽略。

### 3.4 fb.c

- `fb_open()`：`FBIOGET_VSCREENINFO` 校验 1024x600/32bpp；`FBIOGET_FSCREENINFO` 取 `line_length`（实测 4096）；`mmap`。
- `fb_apply_tile(x,y,w,h,const void*data,format)`：坐标/HW 防御性裁剪到屏幕；`dst = base + y*stride + x*4`；BGRA32 走逐行 `memcpy(w*4)`；RGB565 走逐像素转换：

  ```
  r=(p>>11)&0x1F; g=(p>>5)&0x3F; b=p&0x1F;
  out = ((r<<3)|(r>>2))<<16 | ((g<<2)|(g>>4))<<8 | ((b<<3)|(b>>2))   // 按内存序 B,G,R,A 写入
  ```

- M3 起：若本连接尺寸为 800x480，写入前对坐标加居中偏移 (112,60)（不缩放，四周留黑）；进入该模式的首个全刷前可将整屏清黑一次，避免旧内容残留在边框区。
- 首帧到达前保持 fb 原内容（psplash 已被杀，残留最后画面）。

### 3.5 统计与日志

- 每 5s 输出一行 `STATS`（格式见 PLAN §8.2），键含：`fps tiles_s rx_kbps rss_kb full_refresh backlog_hits`；写 stdout（由 S99 重定向到 `/tmp/superscreen.log`）。
- 用于验收 A3/A4/A5 的数据采集。
- **延迟日志**（配合 A3）：每次"屏内容静止 → 首个变化 tile"时，打印 `lat_probe rx_ts=<board墙钟毫秒> tile=<x,y>`；Windows 端有对应发送时间戳（见 Windows 文档 §4.8），两者差值即近似端到端延迟。

### 3.6 test_pattern（自测图定义，客观可验）

板上运行 `./test_pattern`（`--format bgra32|rgb565`，默认 bgra32），图案固定如下：

| 区域 | 内容 |
|---|---|
| y ∈ [0,32) | 纯白条（检查偏移/闪烁） |
| y ∈ [32,512) | 8 条垂直色带（每条宽 128px），从左到右：红、绿、蓝、白、黄、青、品红、黑 |
| y ∈ [512,576) | 16px 黑白棋盘格 |
| y ∈ [576,600) | 32 段灰阶阶梯：8 个灰度值 0,36,73,109,146,182,219,255，每段 32px 循环 4 次 |

- `--format rgb565` 时：内部先生成 RGB565 源数据，再调用与 `fb.c` **同一个** RGB565→BGRA 转换函数写入 fb，用于验证降级路径。
- 运行后抓屏（§8 命令）与上表逐项核对即 B2 通过。

## 4. 关键实现要点（平台经验）

- **static + 直连 fb**：不引入 Qt；`mmap` 后只写变化块，避免整屏刷新造成闪烁。
- **LZ4 vendoring**：板上无 `liblz4`。从 LZ4 官方 v1.9.4（BSD-2）取 `lib/lz4.c`、`lib/lz4.h` 两个文件放入 `board/src/lz4/`，随工程编译；
  获取方式（Ubuntu）：
  ```bash
  curl -L -o /tmp/lz4.tar.gz https://github.com/lz4/lz4/archive/refs/tags/v1.9.4.tar.gz
  tar -xzf /tmp/lz4.tar.gz -C /tmp
  cp /tmp/lz4-1.9.4/lib/lz4.c /tmp/lz4-1.9.4/lib/lz4.h board/src/lz4/
  ```
- **不碰 fbcon 绑定以外的东西**：解绑 fbcon 只影响屏幕文本输出，SSH 不受影响。
- **RGB565 属降级路径**：默认 BGRA32 直拷（零转换）。
- **单核 CPU 预算**：目标单帧 160 块全解压+写入 < 30ms；空闲（无变化）CPU < 5%；实现后实测调优（必要时 NEON 优化转换）。

## 5. 系统设置与自启脚本

`board/scripts/S99superscreen`（buildroot init 风格）：

```sh
#!/bin/sh
DAEMON=/root/super_screen/super_screen_client
PIDFILE=/var/run/superscreen_loop.pid
start() {
  killall psplash 2>/dev/null
  for v in /sys/class/vtconsole/vtcon*; do
    [ -e "$v/name" ] || continue
    grep -q "frame buffer" "$v/name" && echo 0 > "$v/bind"   # 实测 vtcon1 = "(M) frame buffer device"
  done
  echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null
  iw dev wlan0 set power_save off 2>/dev/null
  [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null && return  # 已在运行
  ( while true; do "$DAEMON" >>/tmp/superscreen.log 2>&1; sleep 1; done ) &
  echo $! > "$PIDFILE"
}
stop() {
  [ -f "$PIDFILE" ] && kill "$(cat "$PIDFILE")" 2>/dev/null; rm -f "$PIDFILE"  # 先停重启循环，否则 1s 内拉起
  killall super_screen_client 2>/dev/null
}
case "$1" in start) start;; stop) stop;; restart) stop; start;; *) echo "usage: $0 {start|stop|restart}";; esac
```

安装：`scp` 到 `/etc/init.d/S99superscreen && chmod +x`；首次手动 `S99superscreen start` 验证后再启用开机自启。

## 6. 构建

```bash
cd board
make                     # 输出 super_screen_client 与 tools/test_pattern → build/
make HOST=1              # 主机侧语法检查（x86 gcc，自动去掉 neon/float-abi 参数）
```

- Makefile 关键项：`CC ?= arm-buildroot-linux-gnueabihf-gcc`、`CFLAGS = -O2 -Wall -Wextra -mfpu=neon -mfloat-abi=hard`（`HOST=1` 时 `CC=gcc` 且去掉 neon 参数）、`LDFLAGS = -lrt`（按需）。
- 要求：**零警告**（`-Wall -Wextra`）。

## 7. 部署

```bash
# 1) 停占屏/占 CPU 应用（避免干扰）
ssh root@192.168.1.14 'killall PhotoAlbum VoiceAssistantQt lv_100ask_camera_demo video_test 2>/dev/null; /etc/init.d/S99voiceassistant stop 2>/dev/null'
# 2) 部署
scp board/build/super_screen_client board/build/test_pattern root@192.168.1.14:/root/super_screen/
# 3) 运行（测试期手动，验证后交 S99superscreen）
ssh root@192.168.1.14 'cd /root/super_screen && nohup ./super_screen_client >/tmp/superscreen.log 2>&1 &'
```

## 8. 测试与验收

| # | 测试 | 步骤 | 通过标准 |
|---|---|---|---|
| B1 | 交叉编译 | `make clean && make` | 零警告、产物为 ARM 可执行 |
| B2 | fb 通路/色彩序 | 板上运行 `test_pattern`，主机抓屏转 PNG | 彩条颜色与代码定义一致（验证 BGRA 顺序） |
| B3 | mock 原始流 | Ubuntu 运行 `tools/mock_sender.py <板IP> --codec raw` | 静态图/移动方块可见；无花屏、无撕裂异常 |
| B4 | mock LZ4 流 | `tools/mock_sender.py <板IP> --codec lz4` | 与 B3 效果一致；日志显示 LZ4 解码成功 |
| B5 | 断线自愈 | 杀 mock → 重启 mock | 5s 内重连，全刷后画面恢复一致 |
| B6 | 稳定性 | mock 变速内容跑 30min | 无崩溃；RSS 无持续增长；CPU/带宽记录在案 |
| B7 | 延迟近似 | 先确认板端已对时（ntpd 运行，见总计划 §10）；mock 静止 10s 后画一个 64x64 方块 | 板端 `lat_probe` 与 mock 发送时间戳差值 < 150ms（方法见总计划 §10） |

抓屏命令（总计划 §10）：`dd if=/dev/fb0 bs=4096 count=600 | convert -size 1024x600 -depth 8 bgra:- fb.png`（主机侧亦可先落地 raw 再转）。

### 8.1 tools/mock_sender.py CLI 契约（板端负责人实现，Windows 端按此对拍）

```
用法: python3 tools/mock_sender.py <board_ip> [选项]
  --port N        默认 5901
  --pattern P     bars | move | scroll   （默认 move）
  --fmt F         bgra32 | rgb565        （默认 bgra32）
  --codec C       auto | raw | lz4       （默认 auto：优先 LZ4，压不小则 raw）
  --fps N         目标帧率（默认 10，上限 30）
  --seconds N     运行时长（默认 0 = 一直运行）
  --full-every N  距上次全刷 ≥N 秒且有变化时插入全刷（默认 5，0=禁用）
```

行为要求（可被 B3–B7 直接使用）：

1. 连接板端 TCP；发 `HELLO`（1024x600、`--fmt`、fps_cap=20），3s 内未收 `HELLO_ACK` 则报错退出；**后续一律按 `HELLO_ACK.format` 发送**。
2. 内部维护 2.4MB 当前帧与干净帧，逐 64x64 块 `bytes` 比较，只发送变化块；连接后第一帧必为全刷（`FULL_BEGIN`→`TILES`→`FULL_END`）；之后距上次全刷 ≥`--full-every` 秒且存在变化块时插入一次全刷，画面无变化时不发。
3. 每 5s 打印一行 `STATS`（格式见 PLAN §8.2，必含键 `frames tiles_s tx_kbps ratio pong_rtt`）；每个变化 tile 若处于"静止后首次"则打印 `lat_probe tx_ts=<毫秒>(time.time()*1000)`（对应 §3.5）。
4. `--codec lz4` 依赖 `python3-lz4`（`apt install python3-lz4`）；未安装时给出明确报错。
5. 图案定义：`bars`=§3.6 静态图；`move`=黑底上一个 64x64 白方块按 45° 反弹（每帧移动 8px）；`scroll`=100px 高渐变条从下向上循环滚动。
6. Ctrl-C 退出码 0，并打印汇总（总帧数/总字节/平均 Mbps/最长连续静止时长）。
7. 每 5s 发送 `PING` 并记录 `PONG` 往返毫秒（`STATS` 的 `pong_rtt`，超时记 -1 并打 WARN；不因心跳超时主动断开，运行时长由 `--seconds`/Ctrl-C 控制）。
8. 收到 `SET_QUALITY` 立即采纳 `format/fps_cap` 并清空干净帧缓冲（下帧起全刷），与 Windows 端行为一致。

### 8.2 tools/screenshot_board.sh（主机抓屏封装）

```
用法: tools/screenshot_board.sh [out.png] [board_ip=192.168.1.14]
行为: ssh 抓 fb0 原始数据 → convert 转 PNG → 打印 out.png 路径与 sha256
```

## 9. 性能目标

| 指标 | 目标 | 备注 |
|---|---|---|
| tile 应用（含 LZ4 解压） | 160 块 < 30ms | 决定板端可消化的上限 |
| 空闲 CPU | < 5% | 无变化时仅收心跳 |
| 接收吞吐 | ≥ 8Mbps 持续 | 覆盖降级档需求 |
| 重连时间 | ≤ 5s | accept 循环间隔 1s + 心跳超时 |

## 10. 风险与对策

| 风险 | 对策 |
|---|---|
| fbcon/内核日志覆盖画面 | S99 解绑 fbcon（按 name 匹配，不影响 SSH） |
| psplash 未退出 | 启动脚本 kill；必要时 `S99myirhmi2` 一并停 |
| 其他服务抢 CPU | 部署前停无关应用；实测 CPU 不足时再调服务 |
| WiFi 丢包导致协议错乱 | 流式解析 + 长度校验 + 出错断连重连全刷（自愈设计） |
| RGB565 色彩/字节序错误 | B2 用 test_pattern 双格式分别验证后再上 mock |

## 11. 与 Windows 端的接口

- 协议版本、消息、tile 格式全部以 `docs/PLAN.md` §5 为准；本端同时是协议的**参考实现**（字段顺序以本端解析器为准）。
- 联调节点：M1（raw）+ M2（LZ4/统计/自启），双方按 §8 测试表交叉验证 A2/A3/A4/A5。
