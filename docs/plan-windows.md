# Windows 端计划：VDD 虚拟副屏 + 采集发送应用

> 三份计划的第 3 份。总计划与 **SSSC 协议契约**见 `docs/PLAN.md`，本文件不重复定义协议。

## 0. 接手速览（TL;DR，零上下文可直接开工）

- **你的角色**：Windows 端开发者（大模型），负责在 Windows 10/11 注册虚拟副屏并实现采集发送应用。
- **你要交付**：`windows/`（VS2022 C++ 工程，产物 `super_screen_win.exe + config.ini`）、`windows/vdd/`（VDD 配置与安装记录）、`tools/mock_receiver.py`（Python，Ubuntu 运行，用于无板自测）。
- **前置阅读**：`docs/PLAN.md` §5/§5.6/§5.7（协议 + `docs/sssc.h` + 边界规则）、§9（板端底数）；协议类型**原样复制** `docs/sssc.h` → `windows/src/sssc.h`。
- **第一步**：按 §3 在 Windows 上安装已签名 VDD 并确认副屏可扩展（验收 A1）；随后建 VS 工程，实现 §4 各模块。
- **完成定义**：§7 的 C1–C6 全过（Win10 与 Win11 各验证一台）。
- **仓库协作（强制）**：在自己的开发分支 `dev/windows` 上开发，**禁止直接提交/推送 `main`**；达到里程碑且验收全过后发起 PR 合并。完整规则（分支、PR 模板、提交规范、冲突处理）见 `docs/PLAN.md` §8.1。
- **代码规范（强制）**：C++17、4 空格/LF、命名、错误处理、日志格式（`STATS`/`lat_probe`）、零警告要求见 `docs/PLAN.md` §8.2。
- **不做**：自研驱动、开测试模式/签名、修改协议字段、引入 .NET 或第三方运行时/库（LZ4 源码除外）。

## 1. 目标与范围

1. **M0**：在 Windows 10/11 上安装**已签名** VDD（IddCx 虚拟显示驱动），注册 1024x600@60（保留 800x480@60）副屏，验证可扩展显示。
2. **M1+**：实现 Win32 C++ 应用 `super_screen_win`：采集虚拟屏画面 → 64x64 分块 diff → 光标合成 → LZ4 → 按 SSSC 协议经 TCP 发往板端。
3. **不做**：自研驱动、测试签名、触摸注入、H.264 编码、.NET 运行时依赖。

## 2. 交付物布局

```
windows/
  super_screen.sln / super_screen.vcxproj   # VS2022, x64, C++17, /MT
  src/sssc.h                   # 从 docs/sssc.h 原样复制（唯一权威类型定义，勿改）
  src/main.cpp                 # 主循环（~20fps tick）、config 读取、统计输出、--dump-frame
  src/monitor.cpp/.h           # 枚举显示器，定位 VDD 副屏矩形
  src/capture_gdi.cpp/.h       # v1 基准采集：BitBlt + DIB section
  src/capture_dxgi.cpp/.h      # M3：IDXGIOutputDuplication，失败回退 GDI
  src/tiles.cpp/.h             # 分块 diff、脏块收集、全刷调度、背压降级
  src/cursor.cpp/.h            # 光标合成进分块
  src/net.cpp/.h               # 连接/重连、发送队列、心跳、TCP_NODELAY
  src/lz4wrap.cpp/.h + src/lz4/  # vendored LZ4 v1.9.4
  config.ini                   # 全部可调参数（见 §5）
  vdd/vdd_settings.xml         # 注入 1024x600@60 / 800x480@60
  vdd/install_notes.md         # 安装、验证、卸载/恢复记录
  README.md                    # 构建与使用说明

（仓库根 tools/，与 windows/ 平级）
tools/mock_receiver.py         # Ubuntu 运行：监听 5901，解析 SSSC 并打印（无板子验收 Windows 端）
```

## 3. M0：VDD 环境搭建

### 3.1 Ubuntu 侧准备（可远程）

```bash
# 下载官方 release（2025-07-23 已签名版）
wget https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VDD.Control.25.7.23.zip
sha256sum VDD.Control.25.7.23.zip        # 期望 a701f2272e9fcf382849b24f913c6dd07597b3b1116525f2e90182f019609154
unzip -l VDD.Control.25.7.23.zip         # 核对内容：驱动 .inf/.sys/.cat + 控制 App
apt install osslsigncode 2>/dev/null && osslsigncode verify -in <driver>.cat   # 可选：Linux 上先验签
```

- `windows/vdd/vdd_settings.xml`：按 VDD 官方 wiki 的 schema 撰写，加入 1024x600@60 与 800x480@60；**schema 细节在 M0 执行时以 release 内文档为准**（避免臆造格式）。
- 备选安装源：`winget install --id=VirtualDrivers.Virtual-Display-Driver -e`。

### 3.2 Windows 安装（本机或 OpenSSH 远程）

1. 运行 VDD Control App（或以 winget 安装），Install Driver；
2. 将 `vdd_settings.xml` 拷到 `C:\VirtualDisplayDriver\`，在 App 里 RELOAD_DRIVER；
3. `DisplaySwitch.exe /extend` 切扩展模式；在显示设置中把副屏设为 1024x600@60、缩放 100%、置于合适位置（建议主屏右侧）。

### 3.3 验证（命令行友好）

```powershell
Get-PnpDevice -Class Display | Format-Table Status,FriendlyName,InstanceId
Get-PnpDevice -Class Monitor | Format-Table Status,FriendlyName
Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID    # 应出现副屏 EDID
```

- 在**物理控制台**的"设置 → 系统 → 显示"中肉眼确认副屏出现且可扩展（RDP 会话会改变显示器拓扑，只能作辅助验证）。
- Win10 与 Win11 **各验一台**；记录系统版本、VDD 版本、结果到 `vdd/install_notes.md`。

### 3.4 排障预案

- 黑屏/显示优先级混乱：按 VDD README 流程，连按 2–3 次强制关机进入恢复环境 → 安全模式 → 设备管理器卸载 VDD → `bcdedit` 无需改动（未开测试模式）。
- 升级 GPU/芯片组驱动**前先卸载 VDD**，升级后重装。
- 若安装失败：回退备选方案（空闲 HDMI/DP 口 + EDID 假负载 + 同一采集应用），功能不受影响，仅"虚拟"属性不同。

## 4. 应用设计

### 4.1 main.cpp — 主循环

```
启动：读 config.ini → net_connect(板IP:5901) → 发 HELLO → 等 HELLO_ACK
tick（默认 20fps，Sleep 对齐）:
  1) capture     （采集虚拟屏 → BGRA 源帧，尺寸 = 虚拟屏分辨率）
  2) cursor      （光标合成到源帧）
  3) scale       （按 HELLO_ACK 目标分辨率缩放源帧；StretchBlt/HALFTONE；目标=源时跳过）
  4) tiles_diff  （目标分辨率 64x64 memcmp，得脏块表；必要时调度全刷）
  5) lz4 + send  （按 64KB 拆包；队列 >256KB 则本轮跳过）
  6) 心跳/统计   （每 5s PING；每 5s 打印 脏块数/压缩比/KBps）
断线：net 层 1s 重试；重连后全刷。
```

### 4.2 monitor.cpp

- `EnumDisplayMonitors` + `GetMonitorInfo` 枚举所有显示器，按 `config.ini` 的 `monitor_match`（默认 `1024x600`，可选按序号/设备名）定位 VDD 副屏，输出其在虚拟桌面中的 (`x,y,w,h`)。
- **DPI 感知必须在 `main()` 最先调用**：`SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`（失败则退 `SetProcessDPIAware()`）。否则坐标被系统 DPI 虚拟化，采集区域整体错位。
- **虚拟桌面原点可能为负**（副屏在主屏左侧/上方）：一切坐标以虚拟桌面坐标系为准（`SM_XVIRTUALSCREEN`/`SM_YVIRTUALSCREEN`）；GDI 对桌面 DC 的 `BitBlt` 支持负源坐标。
- 未找到 → 打印完整显示器清单（设备名/位置/尺寸）+ 提示"虚拟屏未处于扩展模式"，每 3s 重试等待。
- **虚拟屏分辨率可任意**（常用模式在 VDD 的 `vdd_settings.xml` 中配置，Windows 显示设置选择）；`monitor_match` 只负责定位，不限制虚拟分辨率。
- **运行时变化**：每 3s 重新定位；位置变化更新抓取矩形；分辨率变化按首次定位到的设备名回退定位，重建采集并全刷（目标分辨率由 ACK 决定，无需重连）。

### 4.3 capture_gdi.cpp（v1 基准）

- `GetDC(NULL)` 桌面 DC → `CreateCompatibleDC` → `CreateDIBSection`（32bpp、`biHeight = -600` top-down、`BI_RGB`）→ `BitBlt` 副屏矩形。
- DIB 内存即 BGRA 行主序自上而下，与协议 tile data 布局一致（无需翻转；防御性检查 `biHeight` 符号）。
- 性能：1024x600 BitBlt 一次约数 ms，20fps 下 CPU 占比可接受；统计中输出采集耗时（每 5s 最大值/平均值）。

### 4.4 capture_dxgi.cpp（M3 优化，可回退）

- `D3D11CreateDevice` + `IDXGIOutput1::DuplicateOutput`：按副屏所在 DXGI adapter/output 建立 duplicator；`AcquireNextFrame` → 拷贝到 staging → map 读回。
- 任何一步失败（含 IDD 屏不被 Duplication 支持）→ 打印一次警告并**永久回退 GDI**（会话内）。
- 支持 MoveRects 是潜在优化项，M3 非必做。

### 4.5 tiles.cpp

- 维护目标分辨率 `prev` 缓冲；逐块 `memcmp`（网格按 `HELLO_ACK` 目标宽高计算，默认 16x10=160）；块变化→记脏块。
- 缩放属于 main/capture 层：源帧 = 虚拟屏分辨率，目标 = `HELLO_ACK` 宽高；tile 数据始终是目标分辨率。
- **首帧/失效帧**：`prev` 初始为"无效"；连接后的第一帧以及每次变更 format（采纳 SET_QUALITY）后，`prev` 置无效 → 全部 tile 视为脏，并按 §5.7-4 发全刷。
- 全刷调度（契约见 PLAN §5.5）：连续 3 帧脏块数 >120 → 触发；或距上次全刷 ≥5s 且仍有脏块 → 触发一次（自愈）；画面无脏块时不发全刷。
- 拆包：单条 `TILES` payload ≤64KB，按脏块顺序装箱（一个 tile 的 14B 头 + data 不得跨消息拆开）。
- 背压降级：发送队列连续 3 帧 >256KB → `fps_cap` 减半（下限 5）；恢复充裕时**不自动上调**（避免抖动），仅重连或全刷后复原。
- 接收板端 `SET_QUALITY` → 立即采纳 `format/fps_cap`（`fps_cap` 钳制到 `[SSSC_FPS_CAP_MIN, 当前值]`，只降不升，非法/0 值按下限）、失效 `prev`、丢弃发送队列中**尚未开始发送**的旧格式消息（已部分发送的让其发完，落进板端格式切换窗口被丢弃）、下一帧起全刷、记录日志；降级值在本进程内**粘滞**——重连的 HELLO 继续使用降级后的 format/fps_cap，直到进程重启才回到 config 值（PLAN §5.5，避免"降级→断连→复位→再拥塞"循环）。

### 4.6 cursor.cpp

- VDD 提供硬件光标（IddCx H-Cursor），**光标不在采集画面内**，必须合成：
  1. `GetCursorInfo` 取可见性与 `ptScreenPos`；换算光标左上角 = `ptScreenPos - (xHotspot, yHotspot)`（取 `GetIconInfo`）。
  2. 首选：`GetIconInfo` 的 `hbmColor` 用 `GetDIBits` 读成 32bpp BGRA（含 alpha），对帧缓冲做 source-over alpha 混合。
  3. 退化：`hbmColor` 无有效 alpha 时，用 `DrawIconEx(hdcMem, ...)`（DI_NORMAL）直接画到帧缓冲副本。
  4. 位置在副屏矩形内才画；光标移出或隐藏时清除旧位置。
  5. 光标所在/曾所在块追加进脏块表。
- M1 实测：抓一帧验证画面中确实无光标；若 VDD 已把光标合成进画面，则设 `cursor.composite=0` 关闭本模块（保留旧位置清理逻辑）。

### 4.7 net.cpp

- `WSAStartup(MAKEWORD(2,2))` 失败直接报错退出；socket → `TCP_NODELAY`；阻塞 connect 带 1s 超时重试。
- **每次建立连接（含重连）后固定流程**：发 `HELLO`（宽高 = 虚拟屏分辨率；`format/fps_cap` 取当前生效值，含粘滞降级）→ 3s 内等 `HELLO_ACK`；超时、`status≠0`（板端拒绝）、`tile_size≠64` 或 ACK 宽高为 0 均断开按 1s 重试（`status≠0` 打 ERR）→ 按 **ACK 宽高** 配置 tile 网格并初始化缩放器 → 失效 `prev` → 全刷。`format/目标宽高` 以本连接 `HELLO_ACK` 为准。
- 发送：非阻塞 send + 用户态队列（cap 4MB）；队列满时由 tiles 层决策**跳过本轮**或只整条丢弃**尚未开始发送**的消息（绝不截断半条消息）；丢弃造成的画面滞后由后续全刷自愈。
- 心跳：每 5s 发 `PING`；15s 无 `PONG` 或 send 失败 → `closesocket` 后 1s 重连。
- 所有协议结构体一律取自 `src/sssc.h`（pack(1)）；本机小端，对 `sizeof` 做 `static_assert`（头文件已含）。

### 4.8 日志、延迟探针与调试开关

- 每 5s 打印一行 `STATS`（格式见 PLAN §8.2），键含：`fps tiles_s tx_kbps rss_kb queue_kb full_refresh`；同行附 capture 耗时（均值/最大）、压缩比、当前 `fps_cap/format`。
- **延迟探针**：处于"静止 → 首个变化帧"时打印 `lat_probe tx_ts=<unix毫秒>`（与板端 `lat_probe rx_ts` 对应，A3 近似测量，见总计划 §10）。
- `--dump-frame <out.bmp>`：每次全刷结束时把**缩放后、发往板端的目标分辨率帧**存为 BMP（与板端抓屏做 A2 逐块比对）；可多次运行，文件被最近一次全刷覆盖，用于 C2/C3。

## 5. 配置项（config.ini）

```ini
[server]
ip = 192.168.1.14
port = 5901

[display]
fps_cap = 20
full_refresh_sec = 5
format = bgra32          ; bgra32 | rgb565

[capture]
mode = gdi               ; gdi | dxgi
monitor_match = 1024x600 ; 1024x600 | 800x480 | index:N | name:\\.\DISPLAYn

[tile]
size = 64
lz4 = 1

[cursor]
composite = 1
```

- `monitor_match` 的 `name:` 后接 Win32 设备名原文（如 `\\.\DISPLAY2`）；INI 值不做转义处理，`GetPrivateProfileString` 原样读取，按原文写入即可（不要增删反斜杠）。
- 虚拟屏分辨率在 Windows“显示设置”中选择（模式来自 VDD 的 `vdd_settings.xml`，可自行添加常用分辨率，
  如 1280x720/1920x1080）；`config.ini` 不限制虚拟分辨率，`monitor_match` 只负责定位。
- 板端目标分辨率由板端 `--resolution WxH`（或 S99 的 `RESOLUTION`）决定，必须 ≤ 物理屏；Windows 端通过
  `HELLO_ACK` 获取后把画面缩放到该尺寸发送，`config.ini` 无需配置板端分辨率。

## 6. 构建

- VS2022（v143）+ Windows SDK 10；安装工作负载"使用 C++ 的桌面开发"；C++17；x64；**/MT 静态运行库**（免 VC++ Redist 依赖）；`/W4 /utf-8`（源码含中文注释，必须 UTF-8）。
- 子系统用 **Console**（`/SUBSYSTEM:CONSOLE`），保留日志窗口，便于 C1–C6 取证（v1 不做托盘/隐藏窗口）。
- 工程依赖：`ws2_32.lib`（网络）、`gdi32.lib`/`user32.lib`（采集/光标）；M3 增加 `d3d11.lib`/`dxgi.lib`。
- `config.ini` 用 `GetPrivateProfileString` 读取；文件缺失时使用内置默认值并在日志中提示。
- 代码可在 Ubuntu 编写，编译以 MSVC 为准；mingw-w64 仅能覆盖 GDI 路径，不作主路径。
- 产出：`super_screen_win.exe`（单文件 + config.ini + 源码编译说明已写入 `README.md`）。
- **M2 自启（A5）**：不做 Windows 服务（服务运行在会话 0，取不到交互桌面与显示器拓扑）；用**任务计划程序**注册"用户登录时启动"，触发后延迟 30s 等 VDD 与桌面就绪，工作目录设为 exe 所在目录；安装步骤记入 `windows/README.md`。

## 7. 测试与验收

| # | 测试 | 步骤 | 通过标准 |
|---|---|---|---|
| C1 | 协议一致性 | Ubuntu 跑 `tools/mock_receiver.py`，Windows 应用把 server.ip 指向 Ubuntu | mock 端正确解析 HELLO/TILES/PING，无字段错位 |
| C2 | 场景指标 | 静态桌面 / 光标移动 / 记事本打字 / 浏览器滚动，各 30s | 输出脏块数、压缩比、KBps 表（对照总计划 A4） |
| C3 | 端到端 | 与板端联调（板端 §8 B3/B4） | A2 画面比对通过；A3 延迟 <150ms |
| C4 | 双系统 | Win10、Win11 各跑 C1–C3 | 均通过（A1） |
| C5 | 鲁棒性 | 中途断网/板子重启/拔插网络；连续运行 30min | 自动重连并全刷；无崩溃；内存稳定 |
| C6 | 降级 | 板端发 SET_QUALITY / 制造大流量 | 正确切 RGB565 或降 fps；画面不崩 |

### 7.1 tools/mock_receiver.py CLI 契约（Windows 负责人实现，用于无板自测）

```
用法: python3 tools/mock_receiver.py [--port 5901] [--target WxH] [--fmt bgra32|rgb565] [--set-quality RGB565[:FPS]] [--save DIR] [--lz4-check] [--log FILE]
行为要求:
  1. 监听 0.0.0.0:<port>，接受一个连接直到断开；断开后继续等待下一个连接（便于 C5）。
  2. 收到 `HELLO` 必须回 `HELLO_ACK`（width/height = `--target`，默认 1024x600，模拟板端目标分辨率；`format` 默认回显 HELLO.format，`--fmt` 可强制覆盖以测降级；status=0，tile_size=64）；收到 `PING` 必须原样回显 `PONG`。缺任一项 Windows 端会一直等 ACK / 判心跳超时，C1/C5 无法通过。
  3. 解析 SSSC：校验魔数/version/length；打印每条消息类型/seq/count/字节数；
     TILES 时逐块校验 w,h≤64、codec∈{0,1}、data_len 与剩余字节吻合、raw（codec=0）时 data_len == w*h*bpp（bpp 按本连接 ACK 的 format），任何不符计数并打印。
  4. 每 5s 打印一行 `STATS`，键名必须与 PLAN §8.2 一致：`frames tiles_s rx_kbps ratio errors pongs`（键值含义：消息数、tiles/s、接收速率、LZ4 压缩比、错误计数、PONG 应答次数）。
  5. --lz4-check：用 python3-lz4 实际解压并校验长度 == w*h*bpp，bpp 按本连接 `HELLO_ACK.format`（`--fmt` 覆盖时按覆盖值）计算（未安装则提示 apt install python3-lz4）。
  6. --set-quality FMT[:FPS]：连接 5s 后发一条 `SET_QUALITY`（C6 无板自测降级用），默认不发。
  7. --save DIR：把每个收到的 tile 以 `tile_x_y_w_h.bin` 落盘（按需，仅 C1 排查用）。
  8. Ctrl-C 退出码 0 并打印汇总。
```

### 7.2 A2 画面比对方法（与板端联调时）

1. Windows 端启动应用并加 `--dump-frame frame.bmp`，等全刷结束后得到 `frame.bmp`（**缩放后的板端目标分辨率**，如 1024x600/800x480/960x540，BGRA）。
2. 板端抓屏（总计划 §10 命令）得到原始 `fb.raw`（按需再转 `fb.png` 目检）。
3. 比对：把两侧统一为**原始 BGRA** 后逐 64x64 块比较（`compare -metric AE` 只报像素差总数，不满足"逐块"判据）：`convert frame.bmp -depth 8 bgra:frame.raw`，板端抓屏命令直接产出 `fb.raw`；按板端居中偏移 `((1024-w)/2,(600-h)/2)` 裁剪 `fb.raw` 后与目标帧比较，判据 **差异块数 = 0**（仅光标所涉 1–2 块有差时人工确认）。注意：LZ4 无损、不应有压缩误差；RGB565 会话存在色彩量化差，A2 应在 BGRA32 下执行。

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| VDD 安装黑屏（官方已知） | 见 §3.4：安全模式卸载；先可回滚机器；不叠加大版本 GPU 驱动升级 |
| IDD 屏不被 DXGI Duplication 支持 | GDI 为基准路径；DXGI 运行时探测，失败自动回退 |
| GDI 采集耗时高 | 统计实测；超预算再上 DXGI（M3）；必要时降 fps |
| 光标未合成/残影 | C2 场景专项检查；残影通过"旧位置标脏"保证 |
| 副屏非扩展模式/分辨率不符 | monitor 模块明确报错并周期重试，不做隐式系统设置修改 |
| Windows 更新后副屏消失 | 记录 VDD 版本；按 §3 流程重装并回归 C4 |
| 双系统差异（Win10 旧版 API） | 仅用 Win10 1903+ 均有的 API（GDI/DXGI/DisplaySwitch）；C4 双验 |
