# 总计划：IMX6ULL 作为 Windows 10/11 原生副屏

> 三份计划的第 1 份（总）。分端文档：
> - 板端：`docs/plan-imx6ull.md`
> - Windows 端：`docs/plan-windows.md`
>
> **SSSC 协议契约只在本文件定义，两端文档引用本文件，不重复定义。**

## 0. 如何阅读本套文档（给接手的开发者/大模型）

| 你的角色 | 必读 | 你的任务 | 立刻做的第一件事 |
|---|---|---|---|
| Ubuntu 端（板端客户端 + mock_sender） | 本文档 §5/§9 + `plan-imx6ull.md` | 实现 `board/` 与 `tools/mock_sender.py` | 按板端 §0 速览建目录并跑 B1（交叉编译） |
| Windows 端（VDD + 采集发送） | 本文档 §5 + `plan-windows.md` | M0 环境 + 实现 `windows/` 与 `tools/mock_receiver.py` | 按 Windows §0 速览装 VDD、验副屏（A1） |
| 总协调 / 验收 | 本文档全部 | 冻结契约、裁决变更、按 §6 验收 | 核对两端对 §5 与 `sssc.h` 的实现一致性 |

- 每份分端文档都是**自包含施工说明**：文件清单、函数级签名、逐条命令、测试表；协议以本文档 §5 与 `docs/sssc.h` 为唯一权威。
- **双端协作规则（强制）**：文档、Linux 端代码、Windows 端代码同属一个远程仓库；两端各自在开发分支工作，**禁止直接向 `main` 提交**，仅里程碑时经 PR 合并。分支名：Ubuntu 端 `dev/linux`，Windows 端 `dev/windows`。完整规则见 **§8.1**。
- **代码编写规范（强制）**：语言标准、格式、命名、错误处理、日志格式、编译零警告等统一要求见 **§8.2**；仓库根 `.editorconfig` 与 `.gitattributes` 已提供，必须随仓库提交。
- **契约变更流程**：先改 §5 + `docs/sssc.h`（升版本号），再同步两端；任何一端不得单方面改字段。
- 契约未定义的行为遵循"**接收方宽松、发送方保守**"：忽略未知消息/保留字段，不崩溃；发送方不依赖接收方未承诺的行为。
- 术语：**块/tile**=64x64 像素传输单元；**全刷**=整屏所有 tile 的一次完整发送（`FULL_BEGIN→TILES→FULL_END`）；**降级**=RGB565 或降低 fps 上限。

## 1. 项目目标

将 100ask IMX6ULL 开发板（LCD 1024x600）作为 Windows 10/11 的**原生副屏设备**接入：

1. Windows 侧通过 IddCx 间接显示驱动注册虚拟显示器，系统"显示设置"中可见、可扩展；
2. 自研应用拿到系统送往该副屏的渲染图，经**局域网**（本项目仅 WiFi）发送给 IMX6ULL；
3. IMX6ULL 接收并实时显示到 LCD。

## 2. 已确认的需求决策

| 项 | 决策 |
|---|---|
| 目标系统 | Windows 10 与 Windows 11 **都要支持** |
| 网络 | **仅 WiFi**（板子 = 2.4GHz 1SS，现协商 72.2Mbit/s）；以太网/USB 暂不考虑 |
| 使用场景 | 文档/聊天/音乐播放器等**准静态内容**；不承诺视频/游戏 |
| 驱动方案 | 使用**已签名**开源 IddCx 驱动 VDD（`VirtualDrivers/Virtual-Display-Driver`，MIT），不自行开发驱动、不开测试模式 |
| 触摸回传 | **不做**，纯显示 |
| 默认模式 | 虚拟屏 **1024x600@60**，可降级 800x480@60（M3） |
| M0 执行环境 | 开发在 Ubuntu，VDD 安装/验证在 Windows（Ubuntu 可通过 SSH 远程执行大部分步骤） |
| 开工顺序 | 文档 → 板端 → Windows 端 |

## 3. 可行性结论与性能边界

**结论：可行。** 关键前提与边界：

- Windows 用户态应用**无法单独注册显示器**，必须由 IddCx 驱动完成——VDD（已签名）解决此问题；采集由自研 Win32 应用完成（GDI BitBlt → 可选 DXGI）。
- 板端 **无 VPU**、单核 Cortex-A7 @792MHz + NEON：**排除 H.264 等视频编码**，采用 **64x64 分块 diff + LZ4** 增量传输。
- 仅 WiFi 是最大瓶颈，全部设计围绕"只传变化块、小包、可降级"展开。

| 内容类型 | 1024x600 预期 | 800x480 降级档 |
|---|---|---|
| 打字/聊天/播放器界面（少量块变化） | 流畅（15–20fps 上限，带宽 1–5Mbps） | 更流畅 |
| 滚动长文档/拖动窗口（大面积变化） | 3–8fps，带宽 10–20Mbps | 6–15fps |
| 全屏视频 | 不可用 | 不可用 |

## 4. 总体架构

```
[Windows 10/11 主机]                                   [IMX6ULL · WiFi]
已签名 VDD (IddCx 虚拟副屏 1024x600@60)                    super_screen_client
  └─ 系统把副屏画面渲染进 VDD                                (C，静态交叉编译)
        │                                                       │
        ▼                                                       │ TCP 5901
自研 Win32 C++ App:                                              │
  monitor/diff/cursor/capture → LZ4 → TCP 发送 ─────────────────┘
                                                        mmap /dev/fb0 只写变化块
```

- "注册副屏"= 安装 VDD（一次性）；"副屏渲染图"= 应用用标准采集 API（GDI BitBlt，M3 起可选 DXGI Desktop Duplication）抓取虚拟屏矩形。
- VDD 硬件光标不在画面内，**由 Windows 端应用合成**进分块（协议层无光标消息）。

## 5. 接口契约：SSSC 协议 v1（冻结）

### 5.1 传输与连接

- TCP；**板端监听 0.0.0.0:5901**，Windows 端起连接端，断线后每 1s 重试。
- `TCP_NODELAY` 开启；所有多字节整数小端序。
- 连接建立后：Windows 发 `HELLO` → 板回 `HELLO_ACK` → Windows 首发一次全刷，随后增量。
- 心跳：Windows 每 5s 发 `PING`；15s 未收到 `PONG` 判定断线并重连。板端 15s 未收到任何消息则主动断开回到 listen。

### 5.2 公共消息头（12 字节）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | magic | ASCII `"SSSC"`（0x53 0x53 0x53 0x43） |
| 4 | 2 | version | =1 |
| 6 | 2 | type | 消息类型，见 5.3 |
| 8 | 4 | length | payload 字节数（不含本头） |

### 5.3 消息定义

| type | 名称 | 方向 | payload（小端） |
|---|---|---|---|
| 0x0001 | HELLO | Win→板 | `width u16, height u16, format u8, fps_cap u8, flags u32`（flags 恒 0） |
| 0x0002 | HELLO_ACK | 板→Win | `width u16, height u16, format u8, status u8(0=OK), tile_size u16, reserved u16` |
| 0x0010 | TILES | Win→板 | `seq u32, count u16, reserved u16`，后接 count 个 tile（见 5.4） |
| 0x0011 | FULL_BEGIN | Win→板 | `seq u32, reserved u32` |
| 0x0012 | FULL_END | Win→板 | `seq u32, reserved u32` |
| 0x0030 | SET_QUALITY | 板→Win | `format u8, fps_cap u8, reserved u16`（建议值，Win 端采纳） |
| 0x0040 | PING | Win→板 | `timestamp u32` |
| 0x0041 | PONG | 板→Win | `timestamp u32`（回显） |

`format`：0 = BGRA32（每像素 4 字节，内存序 B,G,R,A，与板 fb 直拷）；1 = RGB565（每像素 2 字节）。
`status`：0=OK；非 0=拒绝（1=不支持的模式，其余码保留）；Win 端收到非 0 即断开并按 1s 重试（打 ERR 日志）。

### 5.4 Tile 定义与像素约定

- 默认虚拟屏 1024x600；分块 **64x64**；网格 16x10 = 160 块（最底行高 24px）。
- Tile 条目（紧接 `TILES` 头，共 14 字节 + data）：

  | 偏移 | 长度 | 字段 |
  |---|---|---|
  | 0 | 2 | x（u16，块左上角） |
  | 2 | 2 | y |
  | 4 | 2 | w（u16，≤64） |
  | 6 | 2 | h（u16，≤64） |
  | 8 | 1 | codec：0=raw，1=LZ4 |
  | 9 | 1 | reserved |
  | 10 | 4 | data_len（u32，压缩后字节数） |
  | 14 | … | data |

- LZ4 为每 tile 独立 block（`LZ4_compress_default` / `LZ4_decompress_safe`），无字典；解压后长度 = `w*h*bpp`。压后 ≥ 原始则改发 raw。
- 板端对 tile 坐标做防御性裁剪（允许越界但不崩溃）。
- 单条消息 payload ≤ **64KB**（避免 WiFi 分片恶化）；块数与压缩率自适应拆多条。

### 5.5 全刷、背压与降级

- **全刷**：连接后首帧必发；之后满足任一条件时触发：连续 3 帧脏块数 > `SSSC_FULL_DIRTY_THRESH`(120)；或距上次全刷 ≥ `SSSC_FULL_REFRESH_SEC`(5s) 且存在脏块（自愈；画面静止无脏块时不发，不空耗带宽）。序列：`FULL_BEGIN` → 若干 `TILES` → `FULL_END`。板端收到 FULL_END 清统计并确认全屏一致。
- **背压**：Win 端发送队列 > 256KB 时跳过本轮（丢帧不丢连接）；连续 3 帧被跳过则自动将 fps_cap 减半（下限 5）。
- **降级**：板上接收积压（内核收包队列待读字节数，检测方式见板端计划 §3.3）> 1MB 时发 `SET_QUALITY`（先 `format=1` RGB565；已 RGB565 仍拥塞则 fps_cap 减半，下限 `SSSC_FPS_CAP_MIN`；板端配置为强制 format 时 format 字段恒为强制值、只降 fps）。生效时点与过渡：板端发出后立即按新 format 解码，并开启**格式切换窗口**（至收到下一个 `FULL_END` 止）——窗口内 `w*h*bpp` 校验不符的 tile 视为在途旧格式帧，丢弃并计 WARN、不断连（TCP 有序保证旧帧先于新格式全刷到达）；窗口结束恢复 §5.7-6 严格校验。Win 端采纳后失效 prev、丢弃发送队列中未开始发送的旧格式消息、下一帧起全刷；降级值在本进程内**粘滞**——重连的 HELLO 继续使用降级后的 format/fps_cap，直到进程重启才回到 config 值（避免"降级→断连→复位→再拥塞"循环），会话内不回切。
- **自愈**：任何解析错误 → 板端丢弃连接回 listen；Win 端重连后全刷。

### 5.6 规范定义文件（唯一权威）

- **`docs/sssc.h`**：两端**原样复制**（板端 → `board/src/sssc.h`；Windows → `windows/src/sssc.h`）。常量、字段、字节序、静态断言一概不得修改；两端编译必须通过该文件的 `SSSC_STATIC_ASSERT`。
- 类型定义以 `sssc.h` 为准，表格（§5.2–5.4）用于人类阅读；两者若出现差异，立即上报协调方并按 §0 变更流程修正。

### 5.7 边界与确定性规则（两端必须一致实现）

1. **格式协商**：板端最终 format 优先级 = 本地配置强制值 > `HELLO.format`（默认 BGRA32），并通过 `HELLO_ACK.format` 告知；`HELLO.format` 为非法值（非 0/1）时按默认 BGRA32 处理（宽松），Win 端收到非法 `SET_QUALITY.format` 时忽略整条 `SET_QUALITY` 并打 WARN；会话内只有板端可经 `SET_QUALITY` 单向下调，不回切（降级粘滞见 §5.5）。
2. **seq**：每连接内从 1 单调递增，仅诊断用；接收方不因 seq 跳变报错。
3. **Tile data 布局**：行主序、自上而下、紧凑无行间 padding；屏幕边缘 tile 用实际 w/h（如底行 h=24）。
4. **首帧**：连接后 Windows 发送的第一组数据必须为全刷，且所有 tile 视为脏。
5. **断线复位**：板端回 listen、format 回默认（重连时按 Win 的 `HELLO.format` 重新协商，降级粘滞见 §5.5）、统计清零；Win 端清空 prev 帧缓冲，重连后全刷。
6. **LZ4 与长度校验**：发送端目标缓冲 ≥ `LZ4_compressBound(raw_bytes)`；压后 ≥ 原始则改发 raw；接收端按 `w*h*bpp` 精确校验——LZ4 解压长度与 raw 的 `data_len` 均须等于该值，不符即断连（格式切换窗口内例外，见 §5.5）。
7. **保留字段**：发送方必须填 0；接收方必须忽略。
8. **心跳超时**：Win 端 15s 无 PONG（或发送失败）断线重连；板端 15s 无任何消息主动断开。
9. **时间戳**：PING/PONG 的 timestamp 为发送方毫秒级单调计数（非绝对时间），PONG 原样回显。
10. **尺寸协商**：HELLO 宽高必须是板端支持的显示模式（v1：1024x600；M3 起增加 800x480）。支持则 HELLO_ACK 原样回显宽高；不支持则回 status≠0 后断开。800x480 会话中板端将画面**居中、不缩放**显示在 1024x600 面板（左上偏移 (112,60)，四周黑边），tile 坐标原点仍为 800x480 虚拟屏左上角，网格 13x8=104 块（最右列宽 32、最下行高 32）。实现一律由协商宽高推导网格与居中偏移；`sssc.h` 的 `SSSC_SCREEN_*`、`SSSC_GRID_*` 仅描述 v1 默认档（1024x600），不得作为唯一硬编码。
11. **HELLO_ACK 校验**：Win 端校验 status=0 且 tile_size=64（`SSSC_TILE_SIZE`）才继续，否则断开重试。

## 6. 验收标准（总）

| # | 验收项 | 标准 | 验证方式 | 归属 |
|---|---|---|---|---|
| A1 | Win10/Win11 识别副屏 | 显示设置可见、可扩展、1024x600@60 | M0 手动/命令行枚举 | Windows |
| A2 | 画面一致性 | 板端抓屏与虚拟屏截图逐块一致（光标除外） | `dd`+`convert` 抓屏比对 | 两端 |
| A3 | 交互延迟 | 良好 WiFi 下打字/点击 < 150ms | 秒表/日志时间戳 | 两端 |
| A4 | 带宽 | 准静态平均 < 15Mbps；突发自动降级不崩 | 两端统计日志 | 两端 |
| A5 | 稳定性 | 30min 无崩溃/内存增长；断线 ≤5s 重连；两端自启 | 压力脚本 + RSS 日志 | 两端 |
| A6 | 明确不做项 | H.264 / Miracast / 触摸 / 视频场景 | — | — |

## 7. 里程碑

| 阶段 | 内容 | 产出 | 验收 |
|---|---|---|---|
| M0 | 环境：VDD 安装 + 自定义模式 + 双系统验证 | `windows/vdd/` 配置与记录 | A1 |
| M1 | 端到端最小链路：GDI 采集 + raw 块 ↔ 板端写 fb | `windows/`、`board/` 可运行版本 | A2 初步 + 延迟/带宽数据 |
| M2 | 体验完善：LZ4、光标合成、5s 全刷、双端自启、WiFi 省电关闭、统计 | 两端 v1.0 | A3/A4/A5 |
| M3 | 优化：DXGI Duplication（回退 GDI）、800x480 降级档、自适应帧率 | 两端 v1.1 | A4 改善 |
| M4 | （远期可选）USB RNDIS 链路；传输集成进 VDD 驱动 | — | — |

## 8. 仓库布局

```
super_screen/
  README.md               # 项目入口：简介、快速开始、文档索引、协作速查
  .editorconfig           # 代码格式规范（强制，见 §8.2）
  .gitattributes          # 换行符/二进制文件规则（强制，见 §8.2）
  .gitignore              # 构建/调试产物忽略规则
  docs/PLAN.md            # 本文件（总计划 + 协议契约）
  docs/sssc.h             # 协议规范定义（两端原样复制，唯一权威）
  docs/plan-imx6ull.md    # 板端计划
  docs/plan-windows.md    # Windows 端计划
  board/                  # 板端 C 工程（交叉编译）
  windows/                # Windows 发送端（VS2022 工程）与 VDD 配置
  tools/                  # 主机侧工具：mock_sender.py（板端负责人实现）、mock_receiver.py（Windows 负责人实现）、screenshot_board.sh
```

远程仓库（已建立）：`git@github.com:Celeron1996/super-screen.git`（网页：https://github.com/Celeron1996/super-screen ；本地目录名可与仓库名不同）。

### 8.1 双端协作与分支规则（强制，两端大模型必须遵守）

本仓库同时承载文档、Linux 端代码（`board/`）与 Windows 端代码（`windows/`），由两个端的大模型并行开发，均向同一个远程仓库提交。

**分支模型**

| 分支 | 用途 | 谁可以推送 |
|---|---|---|
| `main` | 唯一主分支，只包含**已通过里程碑验收**的代码 | 仅由协调者在 PR 审阅后合并，**任何人不得直接推送** |
| `dev/linux` | Ubuntu 端开发分支（`board/`、`tools/mock_sender.py`、板端脚本） | Ubuntu 端模型 |
| `dev/windows` | Windows 端开发分支（`windows/`、`tools/mock_receiver.py`） | Windows 端模型 |

**硬性规则**

1. **禁止在 `main` 上直接开发/提交/推送**（含 force push）；`main` 只能通过 PR 合并更新。
2. **各端只在自己的开发分支工作**，且只修改本端职责范围内的文件（职责划分见 §0 角色表）。
3. **共享文件不得擅自修改**：
   - `docs/PLAN.md` 与 `docs/sssc.h` 是冻结契约；确需变更时单独发"协议变更 PR"（标题 `docs: protocol vX`），说明对两端的影响，经协调者批准合入后，两端再各自同步；
   - `tools/` 按文件划分所有权，互不交叉（`mock_sender.py` 属 Ubuntu 端，`mock_receiver.py` 属 Windows 端）；
   - 其余文档（本文件其他部分、分端计划）只允许"澄清性"修订，禁止借修改文档改变已冻结的行为。
4. **里程碑合并**：达到 §7 里程碑定义且对应验收（B/C 测试表）全过后，从开发分支向 `main` 发起 PR。PR 必须包含：
   - 标题：`<端>: <里程碑> <一句话>`，例：`linux: M1 raw tiles end-to-end`、`windows: M0 VDD installed + extended display`；
   - 描述：完成项清单、对应验收编号（A/B/C）、验证方法与结果摘要（命令输出/截图/日志）、已知问题与遗留。
5. **一 PR 一里程碑**：PR 不得夹带无关改动；提交 PR 前必须先把最新 `main` 合并进开发分支并解决冲突。
6. **合并方式**：由协调者（人类）审阅后以 **squash merge** 合入 `main`；两端模型不得自行合并或关闭 PR。
7. **合并后**：更新本地 `main`，再把 `main` 合并回自己的开发分支，然后才开始下一个里程碑。
8. **冲突与契约变更**：共享文件发生冲突或需要改契约时，**暂停相关开发**，在 PR/issue 中提出由协调者裁决；禁止单方面"顺手修复"。
9. **提交信息规范**：前缀 `board:` / `windows:` / `docs:` / `tools:` + 祈使句，例：`board: add LZ4 tile decode`。
10. **托管建议**：远程仓库（如 GitHub）对 `main` 开启分支保护：禁止直接推送、禁止强制推送、要求 PR。

**首次操作**（远程仓库已建立，两端各自执行一次）

```bash
git clone git@github.com:Celeron1996/super-screen.git && cd super-screen
git checkout -b dev/linux        # Windows 端：git checkout -b dev/windows
git config user.name  "<端名称>"  # 例：super-screen-linux
git config user.email "<邮箱>"
```

**里程碑例行流程**

```bash
git checkout dev/linux                              # Windows 端改为 dev/windows
git fetch origin && git merge origin/main            # 保持与 main 同步
# ... 开发与验收（板端 §8 / Windows 端 §7 测试表）...
git add board/ tools/mock_sender.py                  # 只加本端文件
git commit -m "board: ..."
git push -u origin dev/linux
# 达到里程碑 → 在托管平台发起 PR → 等待协调者审阅合并（squash）
```

### 8.2 代码编写规范（强制，两端 PR 评审依据）

**语言与标准**

| 端 | 语言/标准 | 验收编译器 |
|---|---|---|
| 板端 | C99；禁用编译器扩展（例外：经 `sssc.h` 封装的 `packed`） | `arm-buildroot-linux-gnueabihf-gcc` |
| Windows | C++17（禁用 C++20 特性） | MSVC v143（VS2022） |
| 工具 | Python 3.8+，**仅标准库**（`python3-lz4` 仅在 `--codec lz4` / `--lz4-check` 时需要） | `python3` |

**格式（仓库根 `.editorconfig` + `.gitattributes` 已强制，二者必须随仓库提交）**

- UTF-8 无 BOM；换行 LF（`.sln/.vcxproj*` 例外，保持 CRLF）；缩进 4 空格（Makefile 用 Tab）；行宽 ≤ 100（Markdown 不受限）；文件末尾必须换行；行尾不留空格。
- Windows 编译必须带 `/utf-8`（源码含中文注释）。

**命名**

- C/C++ 文件名 `snake_case.c/.h/.cpp`；C 函数/变量 `snake_case`，类型 `xxx_t`，宏/常量 `SSSC_*` 或全大写。
- C++ 类/结构 `PascalCase`；成员变量 `m_camelCase`；Win32 风格全局函数 `PascalCase`。
- Python 遵循 PEP 8（`snake_case`）。
- **协议标识符唯一来源**：消息类型、字段、常量一律使用 `sssc.h` 中的名字；禁止另起别名、禁止手写消息字节偏移（LZ4 数据段除外）。

**注释**

- 只写"为什么/约束/坑"，不写"是什么"；禁止保留注释掉的死代码。
- 每个源文件头 3–5 行：职责一句话 + 对应文档章节（如 `See docs/PLAN.md §5.4`）。
- 公共函数标注：参数有效范围、返回值、副作用/阻塞语义。

**错误处理**

- C：所有系统调用与协议解析返回值必须检查；失败打 `ERR` 日志并返回错误码；资源逆序释放；禁止 `abort()` 与沉默失败。
- C++：优先错误码，RAII 管理句柄/内存；不用异常做控制流。
- Python：显式捕获异常并打印可操作信息。
- 退出码统一：`0` 正常；`1` 参数错误；`2` 资源/环境初始化失败。

**日志与统计（机器可读，跨端统一）**

- 事件行：`<module> <LEVEL> <message>`；module = `board|win|mock`；LEVEL = `INFO|WARN|ERR`。
- 统计行：每 5s 一行，以 `STATS ` 开头，空格分隔 `key=value`。各组件**必含键**：板端 `fps tiles_s rx_kbps rss_kb full_refresh backlog_hits`；Windows 端 `fps tiles_s tx_kbps rss_kb queue_kb full_refresh`；`mock_sender.py` `frames tiles_s tx_kbps ratio pong_rtt`；`mock_receiver.py` `frames tiles_s rx_kbps ratio errors pongs`。可按需追加其他 `key=value`。PR 证据直接粘贴此格式即可被脚本校验。
- 延迟探针：Windows `lat_probe tx_ts=<unix_ms>`；板端 `lat_probe rx_ts=<unix_ms> tile=<x,y>`（见总计划 §10）。
- 禁止逐帧/逐 tile 日志；调试细节一律用 `--verbose` 开关控制。

**协议实现纪律**

- 收发一律通过 `sssc.h` 结构体 + `static_assert` 校验；实现 §5.7 全部边界规则（未知消息忽略、长度上限、保留字段填 0/忽略）。

**编译与提交纪律**

- 板端：`make HOST=1` 与交叉编译**零警告**（`-Wall -Wextra`）；Windows：MSVC `/W4 /utf-8` **零警告**；不新增第三方依赖（文档批准除外）。
- 提交前自检：零警告 → 当前里程碑对应的本端测试表条目全过 → 无未记录的 TODO → PR 描述附验证证据（见 §8.1）。
- 不重命名文档中已引用的文件与函数；确需重命名时先提"文档变更 PR"。

## 9. 板端环境底数（2026-10 实测，供两端参考）

- 硬件：i.MX6ULL，Cortex-A7 @792MHz 单核 + NEON，489MB RAM，**无 VPU**；LCD fb0 = 1024x600x32bpp，stride 4096。
- 系统：Linux 4.9.88，Buildroot；eMMC 启动；`fb0` + `fb1`；`psplash` 常驻、fbcon 开启（`/sys/class/vtconsole/vtcon1`，name 为 `(M) frame buffer device`）；`fbcon/cursor_blink` 已为 0。
- 软件：有 libjpeg-turbo / libavcodec / zlib / Qt5；**无 liblz4**（需 vendor）；有 `/dev/input/event*`（本项目不用）。
- 网络：`wlan0` = 192.168.1.14（DHCP），2.4GHz，实测协商 72.2Mbit/s（-55dBm）；`eth0` 100M 存在但未接（本项目不用）。
- 工具链：`arm-buildroot-linux-gnueabihf-gcc`（`~/.bashrc` 已在 PATH）。
- 运行提示：部署前先停占屏/占 CPU 应用（PhotoAlbum、VoiceAssistantQt、S99voiceassistant 等）；WiFi 传输优化用 `iw dev wlan0 set power_save off`。

## 10. 验证手段

- 板端抓屏：`ssh root@192.168.1.14 'dd if=/dev/fb0 bs=4096 count=600' > fb.raw && convert -size 1024x600 -depth 8 bgra:fb.raw fb.png`
- 动画流畅度：板上 `ffmpeg -f fbdev -framerate 15 -i /dev/fb0 -t 12 out.mp4`，主机抽帧。
- 两端统计：统一输出 §8.2 的 `STATS` 行（发送端另附脏块数/压缩比；板端键含 `tiles_s/rss_kb/backlog_hits`）。
- 协议冒烟：`tools/mock_sender.py`（板端负责人实现，CLI 契约见板端文档 §8.1）、`tools/mock_receiver.py`（Windows 负责人实现，CLI 契约见 Windows 文档 §7.1）。
- **A3 延迟近似测量**：测量前两端必须对时（Windows 用系统时间同步；板端 ntpd **是否驻留需在 M1 前核实**，未驻留则先启动或改用其他对时方式），确保两端偏差 < 50ms；Windows 端记录每次"发送包含该变化的 TILES"的本地时间（应用日志），板端记录该 tile 写入 fb 的本地时间；差值即端到端近似延迟，接受作为 A3 判据。另以 PING/PONG RTT 作为网络基线（局域网 WiFi 目标 < 60ms）。

## 11. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| VDD 安装导致黑屏/显示异常（官方已知问题） | 主机不可用 | 先在可回滚机器安装；按 VDD README 安全模式卸载；不随 GPU 驱动大版本同时升级 |
| 仅 WiFi 带宽波动/丢包 | 卡顿、画面滞后 | 脏块+LZ4、64KB 小包、背压丢帧、SET_QUALITY 降级、关 WiFi 省电；必要时 M3 启用 800x480 |
| GDI 采集 CPU 偏高 / DXGI 对 IDD 支持不确定 | 帧率不足 | GDI 为基准路径；DXGI 仅作优化且失败自动回退 |
| Windows 更新兼容性 | 副屏消失 | 锁 VDD 版本记录；更新后按 M0 流程重装 |
| Win10/Win11 双验证需要两台机器 | 延期 | M0 起即双机验证；至少一台 + 官方支持声明兜底 |
| 板子被其他服务占用 fb/CPU | 画面闪烁、掉帧 | 启动脚本接管 fb（停 psplash、解绑 fbcon）；部署前停无关应用 |

## 12. 参考

- Microsoft Learn：Indirect Display Driver Model Overview（IddCx 官方机制）
- `VirtualDrivers/Virtual-Display-Driver`（MIT，已签名 IddCx 驱动，Win10/11）
- 原版 `ge9/IddSampleDriver` / `roshkins/IddSampleDriver`、Sunshine 串流项目的 VDD 集成（采集可行性佐证）
