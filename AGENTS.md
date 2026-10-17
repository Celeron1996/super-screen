# AGENTS.md — super_screen 会话交接与当前状态

> 新会话请先读本文件，再读 `docs/PLAN.md` §5/§8。协议唯一权威是
> `docs/PLAN.md` §5 与 `docs/sssc.h`；本文件只记录当前进度，不改变契约。

## 1. 项目一句话

Windows 10/11 通过已签名 VDD 注册虚拟副屏 → Win32 采集/分块/LZ4 → SSSC/TCP 5901 →
IMX6ULL 直接写 `/dev/fb0`。目标场景是准静态内容（文档/聊天/音乐界面）。

## 2. 仓库与分支（2026-10-17 11:25 UTC）

- 当前分支 `dev/linux`，工作树干净；`main` 已更新到 `origin/main`（`f310bb7`）。
- `main` 已包含 PR #1（Linux 代码到 `9680de5`）与 PR #2（Windows 端，`1152e62`/`f310bb7`）。
- `dev/linux` 在 `origin/dev/linux` 之上有 4 个本地提交（S99 修复、负向测试、merge main、
  本交接文件提交）；本文件提交后会推送同步。
- 规则：只在 `dev/linux` 开发，禁止直推 `main`；提交前缀 `board:`/`windows:`/`docs:`/`tools:`；
  `docs/PLAN.md` 与 `docs/sssc.h` 冻结，变更需协调者批准。
- 本地目录：`/home/book/100ask_development_board/project/super_screen`。

## 3. 环境与访问

- 板端：`192.168.1.14`（wlan0，SSID `USER_480514`，2.4GHz），`ssh root@192.168.1.14` 免密可用。
- Ubuntu 主机：`192.168.1.22`（VMware VM，ens33），用户 `book`。
- 串口：`/dev/ttyUSB0`（CP2104，115200）；当前 `chmod 666`。串口控制台重启后要求 `root` 密码
  （未记录），日常用 SSH；串口仅作恢复通道。临时读写脚本：`/tmp/opencode/serial_cmd.py`、
  `/tmp/opencode/serial_read.py`（`/tmp` 可能被清理）。
- 工具链：`arm-buildroot-linux-gnueabihf-gcc` 在 PATH；主机 `python3-lz4` 已安装。
- Windows 构建产物不在仓库内；Windows 侧路径 `windows\build\x64\Release\super_screen_win.exe`
  与同目录 `config.ini`，说明见 `windows/README.md`。

## 4. 板端当前状态

- `/etc/init.d/S99superscreen` 已安装，sha256
  `cbfa20e23b9ee8a3a0c965ad985449744388c8c59daa3aba7255d8395bca2285`，与提交 `8b255b5` 一致。
- S99 功能：停 psplash、解绑 fbcon、等待 wlan0、必要时启动 wpa_supplicant/udhcpc、
  关闭 rtl8723bu 省电（`rtw_power_mgnt=0 rtw_smart_ps=0 rtw_ips_mode=0`）、循环拉起客户端。
- 当前进程（PID 会变，以 `ps` 为准）：wpa_supplicant 313、udhcpc 345、S99 循环 349、
  `super_screen_client` 424。`/var/run/superscreen_loop.pid` = 349。
- WiFi：`192.168.1.14`，power save off，vtcon1 bind=0。
- **S99voiceassistant 已禁用**（`chmod -x /etc/init.d/S99voiceassistant`）：它原会开机跑
  Python 语音识别，持续占 CPU/音频。需要恢复时 `chmod +x`。
- 客户端二进制：`/root/super_screen/super_screen_client`、`test_pattern`；日志
  `/tmp/superscreen.log`。
- 重启客户端/清日志：`/etc/init.d/S99superscreen stop` → `rm -f /tmp/superscreen.log` →
  `/etc/init.d/S99superscreen start`。不要在客户端打开日志时直接 truncate（会产生稀疏 0）。
- 当前屏幕保留最后一次 Windows 桌面帧；客户端已重新进入监听状态。

## 5. Linux 端完成情况（M1/M2 自测）

- 代码：`board/src/*`、`board/tools/test_pattern.c`、`board/scripts/S99superscreen`；
  `tools/mock_sender.py`、`tools/screenshot_board.sh`、`tools/test_protocol_negative.py`。
- B1–B7 全过。B6 30min：17999 帧 / 207MB，平均 0.92Mbps，RSS 1336→1920KB 后稳定，
  0 条 ERR/WARN，全刷 355 次。
- 协议负向测试 19/19 PASS；格式切换窗口 timeout 已验证：制造 1.1MB 积压，板端连发
  `SET_QUALITY`（RGB565，fps 20→10→5），mock 故意不采纳，15s 后窗口 timeout 关闭，
  随后旧格式 tile 触发严格校验断开。
- 性能：空闲 CPU 0 ticks/10s；160 tile LZ4 全刷约 14ms（<30ms）；压力下板端
  `rx_kbps` 达 8.7–10.75Mbps（§9 ≥8Mbps 目标在压力场景达成）。早期 2.6Mbps 的裸 TCP sink
  测试不代表板端真实接收能力。
- S99 冷启动：开机约 32s SSH 恢复，WiFi/客户端自动拉起；崩溃后 1s 重启 PASS。
- 已知风险：WiFi 偶发 PONG RTT 1.4s 尖峰（正常 3–46ms），A3 延迟需联调确认。

## 6. 联合测试状态（新会话最重要）

- Windows 端已就绪，并已成功连上板端：板端收到 HELLO 1024x600 BGRA32 和全刷，屏幕显示
  Windows 桌面（1024x600，任务栏可见）。截图证据在主机：
  `/tmp/opencode/fb_windows_live.png` 和 `.raw`（可能被清理，可重新抓屏）。
- 该连接约 15s 后被板端判定心跳超时断开（`receive timeout/error`）。原因未确认：可能
  Windows 应用被手动关闭，也可能 Windows 端没有按 5s 发 PING。下次先确认应用持续运行。
- **C1 尚未做**：Ubuntu 已启动 `tools/mock_receiver.py --port 5901 --lz4-check`
  （PID 10806，日志 `/tmp/opencode/c1_receiver.log`，pidfile `/tmp/opencode/c1_receiver.pid`）。
  Windows `config.ini` 默认 `ip = 192.168.1.14`，做 C1 时需改为 `192.168.1.22` 再运行。
- **C3/A2 下一步**：Windows `config.ini` 设 `ip = 192.168.1.14`，运行
  `super_screen_win.exe --dump-frame frame.bmp` 并保持运行；把 `frame.bmp` 拷到 Ubuntu
  （例如 `scp frame.bmp book@192.168.1.22:/tmp/opencode/`）；板端抓屏后按 64x64 块逐块比对
  （BGRA32）。A2 判据是差异块数 = 0（光标涉及的 1–2 块人工确认）。
- C6 降级可与板端 `SET_QUALITY` 机制结合，板端已用 SIGSTOP 造积压验证过。

## 7. 下一步优先级

1. 确认 Windows 应用持续运行；观察板端是否不再 15s 心跳超时（PING 不打印日志，但连接会保持）。
2. C1：Windows `ip=192.168.1.22`，Ubuntu mock_receiver 已监听；核对 HELLO/TILES/PING 与 LZ4。
3. C3/A2：Windows `ip=192.168.1.14` + `--dump-frame`，逐块比对。
4. 全过后按 §8.1 发起 M1/M2 PR（协调者 squash）。
5. M3（800x480 居中）等 M2 验收后再做，不要提前混入 M2。
6. 共享文档问题：`docs/plan-windows.md` §4.5 背压 fps “不自动上调”与“全刷后恢复”表述矛盾，
   需协调者裁决；不要单方面改契约文档。

## 8. 常用命令

```bash
# 板端构建（仓库根）
make -C board clean && make -C board HOST=1 && make -C board

# 部署
scp board/build/super_screen_client board/build/test_pattern \
    root@192.168.1.14:/root/super_screen/

# S99 / 板端状态 / 抓屏
ssh root@192.168.1.14 '/etc/init.d/S99superscreen restart'
ssh root@192.168.1.14 'ps w | grep -E "super_screen_client|wpa_supplicant|udhcpc"; tail /tmp/superscreen.log'
ssh root@192.168.1.14 'dd if=/dev/fb0 bs=4096 count=600' > /tmp/opencode/fb.raw
convert -size 1024x600 -depth 8 bgra:/tmp/opencode/fb.raw /tmp/opencode/fb.png

# Linux 侧测试
python3 tools/mock_sender.py 192.168.1.14 --pattern bars --codec lz4 --seconds 6
python3 tools/test_protocol_negative.py --host 192.168.1.14
python3 tools/mock_receiver.py --port 5901 --lz4-check --log /tmp/opencode/c1_receiver.log
```

## 9. 不要做

- 不要修改 `docs/PLAN.md` §5 或 `docs/sssc.h`（协议冻结）。
- 不要直推 `main`，不要提交 Windows 构建产物/exe（`.gitignore` 已忽略）。
- 不要在板端客户端打开日志时直接 truncate `/tmp/superscreen.log`；先 S99 stop。
- 不要重新启用 `S99voiceassistant`，除非明确需要；它会占 CPU/音频。
- 不要用本地 merge 结果覆盖远端 `main`；按 §8.1 走 PR。
