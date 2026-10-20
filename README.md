# super-screen

将 100ask IMX6ULL 开发板（1024x600 LCD）作为 **Windows 10/11 的原生副屏**。

- Windows 侧通过**已签名**的开源 IddCx 虚拟显示驱动（VDD）注册虚拟显示器，系统"显示设置"中可见、可扩展；
- `super_screen_win.exe` 采集虚拟屏画面，按 **SSSC 协议**（64x64 脏块 + 光标合成 + LZ4）经 TCP 5901 发往 IMX6ULL；
- 板端 `super_screen_client` 直接写 `/dev/fb0`，支持断线重连、自动降级（RGB565 / 降 fps）；
- 分辨率自适应：**板端目标分辨率**可选（`--resolution WxH`，≤ 物理屏 1024x600，居中黑边）；Windows 虚拟屏分辨率任意（常用分辨率），发送端负责缩放到板端目标；采集支持 DXGI（优先）与 GDI（回退）。

> **定位**：文档/聊天/音乐播放器等**准静态内容**的扩展屏。不承诺全屏视频/游戏——板端无 VPU、仅 WiFi（2.4GHz）带宽有限。

## 当前状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| 文档 | 总计划 + 协议契约 + 两端施工文档 | 完成 |
| M0 | VDD 安装、1024x600@60 副屏、A1 | Win10 完成；Win11 验证挂起 |
| M1 | GDI 采集 + raw 分块 + 板端写 fb | 完成（已合入 main） |
| M2 | LZ4、光标合成、全刷、背压、统计、板端自启 | 完成（已合入 main） |
| M3 | DXGI Duplication（回退 GDI）、800x480、显示器位置跟踪 | 完成（PR #7 / #8） |
| 分辨率自适应 | 板端目标分辨率可选（≤1024x600）+ Windows 任意虚拟分辨率缩放 | 完成（本次） |
| 按需副屏 | 点击应用后先选分辨率，再启用 VDD；退出时移除，不随开机出现 | 完成（本次） |
| M4 | USB RNDIS、驱动内集成 | 远期可选 |

## 目录

1. [工作原理](#工作原理)
2. [环境要求](#环境要求)
3. [快速开始（首次跑通）](#快速开始首次跑通)
4. [Windows 端详细教程](#windows-端详细教程)
5. [板端详细教程](#板端详细教程)
6. [配置参考](#配置参考)
7. [日志与统计](#日志与统计)
8. [验收与自测](#验收与自测)
9. [故障排查](#故障排查)
10. [FAQ](#faq)
11. [仓库结构与协作](#仓库结构与协作)

## 工作原理

```
[Windows 10/11 主机]                                   [IMX6ULL · WiFi]
已签名 VDD (IddCx 虚拟副屏 1024x600)                      super_screen_client
  └─ 系统把副屏画面渲染进 VDD                              （C99，静态交叉编译）
        │                                                       │
        ▼                                                       │ TCP 5901
super_screen_win.exe:                                           │
  DXGI/GDI 采集 → 64x64 diff → 光标合成 → LZ4 → TCP ───────────┘
                                                       mmap /dev/fb0 只写变化块
```

| 关键参数 | 值 |
|---|---|
| 虚拟屏 | Windows 任意分辨率（常用 1024x600 / 1280x720 / 1920x1080 等） |
| 板端目标 | `--resolution WxH`，默认 1024x600，≤ 物理屏；居中黑边 |
| 传输 | TCP 5901，64x64 分块，单消息 payload ≤64KB，LZ4 逐块 |
| 帧率 | 默认上限 20fps（背压 / SET_QUALITY 自适应下调，最低 5） |
| 光标 | VDD 硬件光标不在采集画面内，由 Windows 端应用合成 |
| 目标性能 | 打字/聊天 <150ms；准静态平均 <15Mbps |

## 环境要求

| 角色 | 要求 |
|---|---|
| Windows | Windows 10/11 x64；VS2022（"使用 C++ 的桌面开发"）；已签名 VDD 25.7.23 |
| 板端构建机 | Ubuntu 18.04/20.04；`arm-buildroot-linux-gnueabihf-gcc`（100ask SDK） |
| 开发板 | IMX6ULL，1024x600 framebuffer（stride 4096），WiFi 与 Windows 同一局域网 |
| 网络 | Windows 主动连接板端 TCP 5901；板端无需访问 Windows |

## 快速开始（首次跑通）

下面分别给出 **Linux 端（板端）** 与 **Windows 端（发送端）** 的最短路径；两边都完成后即可镜像。
细节见后续对应章节。

### Linux 端（板端：构建 → 部署 → 启动）

要求：Ubuntu 构建机可 SSH 登录板端（`root`），板端与 Windows 在同一局域网。

```bash
# 1) 工具链加入 PATH（100ask SDK；~/.bashrc 已有则新开终端即可）
export PATH=$PATH:/home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin

# 2) 构建（仓库根目录）
cd ~/100ask_development_board/project/super_screen
make -C board clean
make -C board HOST=1
make -C board

# 3) 部署（先停客户端，避免覆盖时报 "Text file busy"）
ssh root@<板子IP> '/etc/init.d/S99superscreen stop'
scp board/build/super_screen_client board/build/test_pattern \
    root@<板子IP>:/root/super_screen/

# 4) 首次安装自启脚本（已安装可跳过），然后启动
scp board/scripts/S99superscreen root@<板子IP>:/etc/init.d/
ssh root@<板子IP> 'chmod +x /etc/init.d/S99superscreen; /etc/init.d/S99superscreen start'

# 5) 确认监听（应有 LISTEN）
ssh root@<板子IP> 'ss -ltn | grep 5901; ps w | grep super_screen_client | grep -v grep'
```

板端 IP 默认为 DHCP（示例 `192.168.1.14`），请用串口或路由器确认后替换 `<板子IP>`。

### Windows 端（VDD → 构建 → 配置 → 运行）

要求：Windows 10/11 x64、VS2022（"使用 C++ 的桌面开发"）、已安装已签名 VDD。
若尚未安装 VDD，先按下文"Windows 端详细教程 → 安装 VDD"完成。驱动保持安装；应用正常退出后
会禁用 VDD 设备，副屏由应用在用户点击启动后按需创建，不再随开机自动出现。

```powershell
# 1) 构建（也可直接使用已有的 exe）
msbuild windows\super_screen.sln /p:Configuration=Release /p:Platform=x64

# 2) 部署到同一目录，并把 server.ip 改成板子 IP
copy windows\build\x64\Release\super_screen_win.exe D:\apps\super_screen\
copy windows\config.ini D:\apps\super_screen\
notepad D:\apps\super_screen\config.ini      # [server] ip = <板子IP>

# 3) 运行（UAC；先选分辨率，确认后才创建虚拟副屏）
cd D:\apps\super_screen
.\super_screen_win.exe
```

连接成功的关键日志：

```
win INFO virtual display \\.\DISPLAY15 active at 1024x600
win INFO capture target \\.\DISPLAY15 1024x600 at (2560,0)
win INFO HELLO_ACK ok, target 1024x600 (virtual 1024x600) format=0
win INFO session ready: target 1024x600, virtual 1024x600, scale=0
```

### 验证与停止

- 把任意窗口拖到 Windows 的**所选分辨率的虚拟副屏**（主屏右侧，不是板子 LCD 表面）；
  板端 LCD 应实时镜像。虚拟副屏位置变化后约 3s 内应用会自动跟踪。
- 停止发送端：在控制台按 `Ctrl-C`（正常退出并移除虚拟副屏）；不要用 `Stop-Process` 强杀，
  否则退出清理不会执行，副屏可能保留到下次开机。若已残留，双击应用点“移除副屏”，或运行
  `super_screen_win.exe --remove-vdd`。
- 停止板端：`ssh root@<板子IP> '/etc/init.d/S99superscreen stop'`
- 无板自测：Ubuntu/本机运行 `python tools/mock_receiver.py --port 5901 --lz4-check`，
  再把 Windows `config.ini` 的 `server.ip` 改为该机器 IP（本机即 `127.0.0.1`）后运行发送端。

## Windows 端详细教程

### 1. 安装 VDD 虚拟显示驱动（M0）

> 已在该仓库开发机完成；新机器按本节执行。完整记录见 `windows/vdd/install_notes.md`。

1. 下载官方 release（已签名）并校验：

   ```powershell
   # 下载 VDD.Control.25.7.23.zip（GitHub: VirtualDrivers/Virtual-Display-Driver）
   Get-FileHash .\VDD.Control.25.7.23.zip -Algorithm SHA256
   # 期望 a701f2272e9fcf382849b24f913c6dd07597b3b1116525f2e90182f019609154
   ```

2. 安装驱动（管理员）。控制 App 的 CLI 在无人交互时会挂起，可直接用包内 `devcon`：

   ```powershell
   devcon install <解压目录>\SignedDrivers\x86\VDD\MttVDD.inf Root\MttVDD
   ```

3. 把本仓库的 `windows/vdd/vdd_settings.xml` 复制到 `C:\VirtualDisplayDriver\vdd_settings.xml`
   （默认包含 1024x600 / 800x480 及常用分辨率，如 1280x720、1366x768、1920x1080 等；可自行添加）。

4. 无需手动扩展或设置分辨率：运行 `super_screen_win.exe` 时会先弹出分辨率选择框，
   确认后应用会启用 VDD、切换扩展模式并设置所选分辨率；退出应用时再禁用 VDD。
   可选模式来自 `C:\VirtualDisplayDriver\vdd_settings.xml`（常用分辨率可自行添加）。

5. 验证（应用运行期间）：

   ```powershell
   Get-PnpDevice -Class Display | Format-Table Status,FriendlyName
   Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID   # 应用运行时应出现 VDD 的 EDID
   ```

   **风险提示**：VDD 安装有官方已知的小概率黑屏问题。新机器请先确认可回滚；异常时按
   `docs/plan-windows.md` §3.4 进安全模式卸载。不要开启测试签名。

### 2. 构建

前置：VS2022 + "使用 C++ 的桌面开发"工作负载（v143 + Windows SDK）。

```powershell
msbuild windows\super_screen.sln /p:Configuration=Release /p:Platform=x64
# 产物：windows\build\x64\Release\super_screen_win.exe
```

要求 `/W4 /WX /utf-8` 零警告（工程已强制）。Debug 配置同理。

### 3. 部署与配置

`super_screen_win.exe` 默认读取**同目录**的 `config.ini`；也可用 `--config <path>` 指定。
最小配置：

```ini
[server]
ip = 192.168.1.14     ; 板子 IP
port = 5901

[display]
fps_cap = 20
full_refresh_sec = 5
format = bgra32

[capture]
mode = gdi            ; gdi | dxgi
monitor_match = 1024x600

[tile]
size = 64
lz4 = 1

[cursor]
composite = 1
```

完整键说明见 [配置参考](#配置参考)。

### 4. 运行与停止

```powershell
.\super_screen_win.exe [--config D:\apps\super_screen\config.ini] [--dump-frame frame.bmp]
```

- 日志格式：`win INFO ...` / `win WARN ...` / `win ERR ...`；每 5s 一行 `STATS ...`
- `--dump-frame`：每次全刷结束保存 BMP（A2 比对用）
- `Ctrl-C` 正常退出（退出码 0）

启动成功的关键日志：

```
win INFO capture target \\.\DISPLAY15 1024x600 at (2560,0)
win INFO connected, HELLO sent (1024x600 fmt=0 fps=20)
win INFO HELLO_ACK ok, session 1024x600 format=0
win INFO session ready: scheduling full refresh
```

### 5. 采集模式（GDI / DXGI）

| 模式 | 说明 |
|---|---|
| `gdi` | 默认，BitBlt + DIB，兼容性最好；1024x600 约 6–8ms/帧 |
| `dxgi` | Desktop Duplication，本机 VDD 实测约 0.2–0.3ms/帧；静态桌面复用缓存帧 |

DXGI 在初始化/帧获取失败时会打一次 WARN 并**自动永久回退 GDI**；无需手动处理。
若需要更低 CPU，优先 `mode = dxgi`。

### 6. 显示器定位与移动

`monitor_match` 支持：

| 写法 | 示例 | 说明 |
|---|---|---|
| `WxH` | `1024x600` | 按分辨率匹配（优先非主屏） |
| `index:N` | `index:1` | 枚举序号（按屏幕位置排序，日志会列出） |
| `name:` | `name:\\.\DISPLAY15` | 按设备名匹配 |

找不到目标屏时，应用会打印完整显示器清单并每 3s 重试。

**运行时变化**：应用每 3s 重新定位；显示器位置变化会更新抓取区域并全刷（日志
`capture target moved to (x,y)`）；分辨率变化会按设备名重新定位、重建采集并全刷
（日志 `capture target resized to WxH`），无需重启。Windows 端始终把画面缩放到板端目标分辨率后发送。

> 注意：板端 LCD 只是显示端，**不是输入设备**。请把窗口拖到 Windows 的虚拟副屏上，
> 不要拖到板子 LCD 表面。

### 7. 分辨率自适应（板端目标 + 虚拟屏）

- **板端目标分辨率**：由板端启动参数 `--resolution WxH`（或 S99 的 `RESOLUTION`）决定，默认
  1024x600，必须 ≤ 物理屏；板端把该尺寸的画面居中显示（四周黑边，不缩放）。
- **Windows 虚拟屏分辨率**：点击应用后，在启动时的选择框中选择（模式来自
  `C:\VirtualDisplayDriver\vdd_settings.xml`，常用分辨率可自行添加）；应用会启用 VDD、
  扩展桌面并设置该模式，退出时禁用 VDD。`monitor_match` 只负责定位，不限制分辨率。
- 连接后 Windows 端从 `HELLO_ACK` 获取板端目标分辨率，把采集画面缩放到该尺寸再分块发送；
  缩放只在 Windows 端做。`config.ini` 不需要配置板端分辨率。
- 示例：板端 `RESOLUTION=800x480`、Windows 虚拟屏 1920x1080 → 发送端缩放到 800x480，
  板端居中黑边显示。

### 8. 启动方式（不再开机自启）

按需求改为**点击应用后再创建虚拟副屏**：不再注册登录自启任务，也不在开机时扩展 VDD 副屏。
直接双击 `super_screen_win.exe`（或桌面快捷方式），在分辨率选择框确认后使用；退出时应用会
禁用 VDD。若旧机器上注册过旧版任务，先删除：

```powershell
# 管理员 PowerShell
powershell -ExecutionPolicy Bypass -File windows\scripts\remove_autostart.ps1
```

应用每次启动都需要 UAC（启停 VDD 设备），工作目录 = exe 目录（`config.ini` 与 exe 同目录）。

## 板端详细教程

以下命令在 Ubuntu 构建机执行（板端运行 Linux 4.9.88 + Buildroot）。

### 1. 构建

```bash
# 工具链加入 PATH（~/.bashrc 已有则新开终端即可）
export PATH=$PATH:/home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin

cd ~/100ask_development_board/project/super_screen
make -C board clean
make -C board HOST=1        # 主机语法检查
make -C board               # 交叉编译
file board/build/super_screen_client   # 应为 ARM ELF
```

要求零警告（`-Wall -Wextra`）。产物：`board/build/super_screen_client` 与 `test_pattern`。

### 2. 部署

```bash
# 先停客户端，否则覆盖会报 "Text file busy"
ssh root@<板子IP> '/etc/init.d/S99superscreen stop'

scp board/build/super_screen_client board/build/test_pattern \
    root@<板子IP>:/root/super_screen/

ssh root@<板子IP> '/etc/init.d/S99superscreen start'

# 验证
ssh root@<板子IP> 'sha256sum /root/super_screen/super_screen_client; \
                   ps w | grep super_screen_client | grep -v grep'
```

### 3. 开机自启（S99）

`board/scripts/S99superscreen` 负责：停 psplash、解绑 fbcon、等待 WiFi、必要时启动
wpa_supplicant/udhcpc、关闭 rtl8723bu 省电，并循环拉起客户端（崩溃后 1s 重启）。

```bash
scp board/scripts/S99superscreen root@<板子IP>:/etc/init.d/
ssh root@<板子IP> 'chmod +x /etc/init.d/S99superscreen; /etc/init.d/S99superscreen start'

# 日常操作
ssh root@<板子IP> '/etc/init.d/S99superscreen restart'
ssh root@<板子IP> '/etc/init.d/S99superscreen stop'
```

日志：`/tmp/superscreen.log`。**不要在客户端打开日志时直接 truncate**；先 stop，再删除/重启。

### 4. 板端参数

```bash
/root/super_screen/super_screen_client [--port 5901] [--format auto|bgra32|rgb565] [--resolution WxH]
```

- `--format auto`（默认）：尊重 Windows 端 `HELLO.format`；
- 强制 `bgra32`/`rgb565` 时 `SET_QUALITY` 只下调 fps，不改变 format；
- `--resolution WxH`（默认 1024x600，必须 ≤ 物理屏）：板端目标显示分辨率；Windows 端通过
  `HELLO_ACK` 获取后把画面缩放到该尺寸；板端居中显示（四周黑边）。

### 5. 日志与抓屏

```bash
# 日志
ssh root@<板子IP> 'tail -f /tmp/superscreen.log'

# 抓屏（1024x600 BGRA，stride 4096）
ssh root@<板子IP> 'dd if=/dev/fb0 bs=4096 count=600' > fb.raw
convert -size 1024x600 -depth 8 bgra:fb.raw fb.png
```

## 配置参考

### Windows `config.ini`

| 段/键 | 默认 | 说明 |
|---|---|---|
| `server.ip` / `port` | 192.168.1.14 / 5901 | 板端地址与端口 |
| `display.fps_cap` | 20 | 帧率上限；背压/SET_QUALITY 只降不升 |
| `display.full_refresh_sec` | 5 | 周期自愈全刷间隔（有脏块时才发） |
| `display.format` | bgra32 | 初始建议格式；最终以 `HELLO_ACK.format` 为准 |
| `display.manage_vdd` | 1 | 1=应用启动时启用 VDD、退出时禁用；0=使用已有显示器 |
| `display.virtual_resolution` | 1024x600 | 分辨率选择框的默认项；选择成功后写回 |
| `capture.mode` | gdi | `gdi` / `dxgi`；dxgi 失败自动回退 |
| `capture.monitor_match` | 1024x600 | `WxH` / `index:N` / `name:\\.\DISPLAYn`；管理 VDD 时运行时自动改为设备名 |
| `tile.size` | 64 | 固定 64（协议常量） |
| `tile.lz4` | 1 | 0=只用 raw，1=优先 LZ4 |
| `cursor.composite` | 1 | 合成 VDD 硬件光标 |

命令行：`--config FILE`、`--dump-frame FILE`、`--resolution WxH`（跳过选择框）、
`--no-vdd`（不管理虚拟屏）、`--remove-vdd`（只移除已注册的虚拟副屏并退出）、`--help`。

- 在默认的 `manage_vdd=1` 模式下，启动应用时先选择分辨率，应用再启用 VDD 并按设备名定位；
  `monitor_match` 无需手工维护。板端目标分辨率仍由板端 `--resolution`/S99 决定，Windows 端通过
  `HELLO_ACK` 获取并在发送前缩放，`config.ini` 无需配置板端分辨率。
- `--no-vdd` 或 `manage_vdd=0` 时回到旧行为：不启停 VDD，按 `monitor_match` 定位已有显示器。
- `--dump-frame` 保存的是**缩放后、实际发往板端的目标分辨率帧**，用于 A2 比对。

### 板端

| 项 | 说明 |
|---|---|
| 监听 | `0.0.0.0:5901`（TCP，SO_REUSEADDR） |
| `--port N` | 修改监听端口 |
| `--format F` | `auto`（默认）/`bgra32`/`rgb565` |
| `--resolution WxH` | 板端目标显示分辨率（默认 1024x600，≤ 物理屏） |
| 会话格式 | 本地强制值优先于 `HELLO.format` |
| 背压降级 | 内核收包队列 >1MB 连续 3 次 → `SET_QUALITY`（先 RGB565，后降 fps） |
| 断线 | 15s 无消息断开回监听；解析错误立即断开（自愈） |

## 日志与统计

各类日志（Windows/板端/mock）每 5s 输出一行：

```
STATS fps=20.0 tiles_s=32.0 tx_kbps=1856.8 rss_kb=14384 queue_kb=0 full_refresh=1 ...
```

| 键 | 含义 |
|---|---|
| `fps` | 本端处理/发送帧率 |
| `tiles_s` | 每秒 tile 数 |
| `tx_kbps` / `rx_kbps` | 发送/接收码率 |
| `rss_kb` | 进程常驻内存 |
| `queue_kb` | Windows 发送队列积压 |
| `full_refresh` | 累计全刷次数 |
| `backlog_hits` | 板端触发 SET_QUALITY 次数 |
| `dirty_avg/max` | 每帧脏块数（Windows） |
| `ratio` | LZ4 压缩比（原始/压缩） |
| `format` | 0=BGRA32，1=RGB565 |

延迟探针：Windows `lat_probe tx_ts=<unix_ms>`，板端 `lat_probe rx_ts=<unix_ms> tile=<x,y>`，
两端时钟需同步（板端 ntpd）。

## 验收与自测

- 板端 B1–B7（构建、fb 色彩、mock raw/LZ4、断线自愈、30min 稳定性、延迟）：
  见 `docs/plan-imx6ull.md` §8
- Windows C1–C6（协议、场景指标、端到端、双系统、鲁棒性、降级）：
  见 `docs/plan-windows.md` §7
- 总体验收 A1–A6：见 `docs/PLAN.md` §6

无板自测（本机协议对跑）：

```powershell
python tools\mock_receiver.py --port 5901 --lz4-check
# 另开终端，config.ini 的 server.ip 改 127.0.0.1，运行 super_screen_win.exe
```

## 故障排查

| 现象 | 可能原因 | 处理 |
|---|---|---|
| 板端画面静止/不刷新 | 发送端未运行；窗口没拖到虚拟副屏；VDD 被移动/关闭 | 启动/重启 `super_screen_win.exe`；确认拖到虚拟副屏；看是否打印 `capture target moved` |
| `HELLO_ACK timeout` | 板端客户端未运行、IP/端口错误、防火墙 | `S99superscreen restart`；`ss -ltn \| grep 5901`；核对 `config.ini` 的 IP |
| `connection refused` | 板端未监听 | 启动 S99；检查板端 IP（DHCP 可能变化） |
| 部署时报 `Text file busy` | 客户端正在运行 | 先 `S99superscreen stop`，再 scp，再 start |
| `DXGI duplication unavailable` | IDD 屏不支持 Duplication | 已自动回退 GDI，无需处理 |
| 自定义分辨率不生效 | 板端分辨率未改 / 虚拟屏模式未选 | 改板端 S99 的 `RESOLUTION`（或 `--resolution`）并重启；虚拟屏在显示设置中选模式 |
| 板端画面有黑边/尺寸不对 | 板端目标分辨率与预期不符 | 查看应用 `session ready: target WxH` 与板端 `HELLO ... target WxH` 日志；核对板端 `RESOLUTION` |
| 提示找不到显示器 | 虚拟屏未创建 / 匹配串错误 | 使用默认 `manage_vdd=1` 让应用创建；或应用运行后 `DisplaySwitch /extend`；改 `monitor_match`；看日志中的显示器清单 |
| 开机后仍自动出现虚拟副屏 | 旧版本未正常退出，或仍注册了旧自启任务 | 运行一次新版应用并正常退出；删除旧任务：`Unregister-ScheduledTask -TaskName super_screen_win -Confirm:$false` |
| 强制结束/关机后副屏仍在 | 进程未执行退出清理，VDD 保持启用 | 双击应用，在分辨率选择框点**“移除副屏”**；或运行 `super_screen_win.exe --remove-vdd`；也可管理员执行 `pnputil /disable-device "ROOT\DISPLAY\0001"` |
| 卡顿/延迟高 | WiFi 拥塞、屏幕大面积变化 | 关闭占带宽应用；降低 `fps_cap`；板端会自动降为 RGB565 |
| 板端变黑/花屏 | fbcon、psplash 或其他服务占屏 | 确认 S99 已解绑 fbcon/停 psplash；停无关应用 |
| VDD 安装后黑屏（主机） | VDD 已知问题 | 安全模式卸载 VDD（见 `windows/vdd/install_notes.md`） |

## FAQ

**Q：能看视频/玩游戏吗？**
不能。板端无 VPU、仅 2.4GHz WiFi；设计目标是文档/聊天/播放器等准静态内容。

**Q：为什么拖到板子屏幕上没反应？**
LCD 只是显示端，没有触摸/输入回传。请把窗口拖到 Windows 的虚拟副屏。

**Q：虚拟副屏跑到别的位置/分辨率变了？**
应用每 3s 自动跟踪位置变化；分辨率变化会按设备名重新定位、重建采集并全刷，无需重启。
应用退出后副屏会被移除（这是预期行为），下次启动会重新创建并让你选择分辨率。
板端目标分辨率由板端 `--resolution`/S99 决定，Windows 端自动缩放到该尺寸。

**Q：Windows 端要不要管理员？**
需要。创建/移除虚拟副屏要启停 VDD 设备，应用清单为 `requireAdministrator`，每次启动会有
UAC 提示。`--no-vdd` 只是跳过设备管理，不改变清单的权限要求。

**Q：板子 IP 变了？**
DHCP 可能导致变化；串口 `ip addr` 或路由器后台查询，改 `config.ini` 的 `server.ip`。

**Q：Windows 防火墙要放行吗？**
Windows 主动连板端，通常无需入站规则；若公司策略严格，放行 `super_screen_win.exe` 出站。

**Q：如何卸载 VDD？**
`VDD Control.exe UNINSTALL_DRIVER` 或设备管理器卸载 "Virtual Display Driver"；
命令行可用 `devcon remove Root\MttVDD`（管理员）。详见 `windows/vdd/install_notes.md`。

## 仓库结构与协作

```
super_screen/
  README.md               # 本文件
  docs/                   # 计划与协议（唯一权威）
  board/                  # 板端 C 工程（交叉编译）
  windows/                # Windows 发送端（VS2022）与 VDD 配置
  tools/                  # mock_sender.py / mock_receiver.py / screenshot_board.sh
```

角色与分支（详见 `docs/PLAN.md` §8.1）：

| 角色 | 分支 | 范围 |
|---|---|---|
| Ubuntu 端 | `dev/linux` | `board/`、`tools/mock_sender.py` |
| Windows 端 | `dev/windows` | `windows/`、`tools/mock_receiver.py` |
| 协调者 | `main` | 审阅 PR、squash 合并 |

提交前缀：`board:` / `windows:` / `docs:` / `tools:`。`docs/PLAN.md` §5 与 `docs/sssc.h`
为冻结契约，变更需协调者批准。

文档索引：

| 文档 | 内容 |
|---|---|
| `docs/PLAN.md` | 总计划、SSSC 协议契约、验收、里程碑、协作规范 |
| `docs/sssc.h` | 协议唯一权威定义（两端原样复制） |
| `docs/plan-imx6ull.md` | 板端施工说明与 B1–B7 |
| `docs/plan-windows.md` | Windows 施工说明与 C1–C6 |
| `windows/README.md` | Windows 构建/配置/按需创建副屏补充 |
| `windows/vdd/install_notes.md` | VDD 安装、验证与回滚记录 |

## 技术栈

- 板端：C99 + vendored LZ4，framebuffer 直写，静态交叉编译（`arm-buildroot-linux-gnueabihf-gcc`）
- Windows：C++17 Win32（VS2022，无 .NET），GDI / DXGI Desktop Duplication
- 虚拟显示：VDD（已签名 IddCx 驱动，Win10/11）
- 工具：Python 3 标准库（`python3-lz4` 仅 `--lz4-check`/LZ4 校验时需要）
