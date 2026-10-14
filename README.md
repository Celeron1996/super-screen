# super-screen

将 100ask IMX6ULL 开发板（1024x600 LCD）作为 **Windows 10/11 的原生副屏**。

- Windows 侧通过**已签名**的开源 IddCx 虚拟显示驱动（VDD）注册虚拟显示器，系统"显示设置"中可见、可扩展；
- 自研 Win32 应用采集该虚拟屏画面，按 **SSSC 协议**（64x64 脏块 + LZ4 增量）经局域网发往 IMX6ULL；
- 板端客户端直接写入 framebuffer（`/dev/fb0`）实时显示，支持断线重连、自动降级。

> **定位**：文档/聊天/音乐播放器等**准静态内容**的扩展屏。不承诺全屏视频/游戏——板端无 VPU、仅 WiFi（2.4GHz）带宽有限。

## 工作原理

```
[Windows 10/11 主机]                                   [IMX6ULL · WiFi]
已签名 VDD (IddCx 虚拟副屏 1024x600@60)                    super_screen_client
  └─ 系统把副屏画面渲染进 VDD                                (C99，静态交叉编译)
        │                                                       │
        ▼                                                       │ TCP 5901
自研 Win32 C++ App:                                              │
  GDI/DXGI 采集 → 分块 diff → 光标合成 → LZ4 → TCP 发送 ────────┘
                                                        mmap /dev/fb0 只写变化块
```

| 关键参数 | 值 |
|---|---|
| 虚拟屏 | 1024x600@60（降级档 800x480），默认 BGRA32，可降 RGB565 |
| 传输 | TCP 5901，64x64 分块，单消息 ≤64KB，LZ4 逐块 |
| 帧率 | 上限 20fps（自适应降级），心跳 5s |
| 目标性能 | 打字/聊天 <150ms 延迟；准静态平均 <15Mbps |

## 当前状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| 文档 | 三份计划 + SSSC v1 契约 + 协作/代码规范 | 已完成（见 `docs/`） |
| M0 | Windows 安装 VDD、注册副屏并验证（Win10/11） | 待开始 |
| M1 | 端到端最小链路（GDI 采集 + raw 块） | 待开始 |
| M2 | LZ4、光标合成、自启、统计（v1.0） | 待开始 |
| M3 | DXGI 采集、800x480 降级档 | 待开始 |
| M4 | （可选）USB RNDIS、驱动内集成 | 远期 |

## 仓库结构

```
super_screen/
  docs/                  # 计划文档与协议定义（唯一权威）
  board/                 # 板端 C 工程（交叉编译）
  windows/               # Windows 发送端（VS2022）与 VDD 配置
  tools/                 # 主机侧工具：mock_sender.py / mock_receiver.py / screenshot_board.sh
  .editorconfig          # 代码格式规范（强制）
  .gitattributes         # 换行符/二进制规则（强制）
```

## 两端接手指引

| 你的角色 | 必读 | 你的分支 | 第一条命令 |
|---|---|---|---|
| Ubuntu 端（板端 + mock_sender） | `docs/PLAN.md` §5/§8.1/§8.2/§9 + `docs/plan-imx6ull.md` | `dev/linux` | 按板端文档 §0 速览执行 |
| Windows 端（VDD + 采集发送） | `docs/PLAN.md` §5/§8.1/§8.2 + `docs/plan-windows.md` | `dev/windows` | 按 Windows 文档 §0 速览执行 |
| 协调者 / 验收 | `docs/PLAN.md` 全部 | `main`（仅 PR 合并） | 按 §6 验收标准 |

## 快速开始

```bash
git clone git@github.com:Celeron1996/super-screen.git && cd super-screen

# Ubuntu 端
git checkout -b dev/linux      # 然后按 docs/plan-imx6ull.md §0 执行

# Windows 端（在 Windows 机器上）
git checkout -b dev/windows    # 然后按 docs/plan-windows.md §0 执行
```

板端构建（Ubuntu，仓库根）：

```bash
mkdir -p board/src board/tools board/scripts board/build tools
cp docs/sssc.h board/src/sssc.h
cd board && make HOST=1 && make   # 产物: build/super_screen_client
```

## 文档索引

| 文档 | 内容 |
|---|---|
| `docs/PLAN.md` | 总计划：决策、架构、**SSSC 协议契约（§5）**、验收 A1–A6、里程碑、协作规则（§8.1）、代码规范（§8.2） |
| `docs/sssc.h` | 协议唯一权威定义（两端原样复制，含编译期静态断言） |
| `docs/plan-imx6ull.md` | 板端施工说明：函数级设计、命令、测试 B1–B7、mock_sender CLI 契约 |
| `docs/plan-windows.md` | Windows 施工说明：VDD 安装、采集/光标/LZ4/网络设计、测试 C1–C6、mock_receiver CLI 契约 |

## 协作速查

- `main` 受保护，**禁止直接推送**；两端各自在 `dev/linux` / `dev/windows` 开发；
- 里程碑验收全过后向 `main` 发起 PR，由协调者 squash 合并；
- 提交前缀：`board:` / `windows:` / `docs:` / `tools:`；
- 完整规则见 `docs/PLAN.md` §8.1，代码规范见 §8.2（零警告、统一日志格式等）。

## 验证速查

```bash
# 板端抓屏（1024x600 BGRA，stride 4096）
ssh root@192.168.1.14 'dd if=/dev/fb0 bs=4096 count=600' > fb.raw
convert -size 1024x600 -depth 8 bgra:fb.raw fb.png

# 板端日志（STATS 行 / 延迟探针）
ssh root@192.168.1.14 'tail -f /tmp/superscreen.log'
```

## 技术栈

- 板端：C99 + vendored LZ4，framebuffer 直写，静态交叉编译（`arm-buildroot-linux-gnueabihf-gcc`）
- Windows：C++17 Win32（VS2022，无 .NET），GDI（M3 起可选 DXGI Desktop Duplication）
- 虚拟显示：VDD（已签名 IddCx 驱动，Win10/11）
- 工具：Python 3 标准库（可选 `python3-lz4`）
