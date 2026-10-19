# super_screen_win

Windows 发送端：采集虚拟副屏 → 64x64 分块 diff → 光标合成 → LZ4 → SSSC/TCP 发送。
协议与行为以 `docs/PLAN.md` 为准；施工说明见 `docs/plan-windows.md`。

## 构建

前置：VS2022（v143）+ Windows 10/11 SDK，"使用 C++ 的桌面开发"工作负载。

```powershell
msbuild windows\super_screen.sln /p:Configuration=Release /p:Platform=x64
```

产物：`windows\build\x64\Release\super_screen_win.exe`（需与 `config.ini` 同目录）。
编译要求 `/W4 /utf-8` 零警告。

## 运行

```powershell
cd windows\build\x64\Release
.\super_screen_win.exe [--config FILE] [--dump-frame frame.bmp] [--resolution WxH] [--no-vdd]
```

- 直接双击或运行 exe 时，会先弹出**分辨率选择框**，确认后才启用虚拟副屏；可用模式来自
  `C:\VirtualDisplayDriver\vdd_settings.xml`。
- 虚拟副屏**只在应用运行期间存在**：启动时启用 VDD 设备，退出（包括 Ctrl-C）时禁用。
  因此应用没有运行时，电脑开机后不会自动出现虚拟副屏。
- 启用/禁用 VDD 设备需要管理员权限：exe 清单为 `requireAdministrator`，每次启动会有 UAC 提示。
- `--resolution WxH`：跳过选择框，直接用指定模式（自动化/测试用）。
- `--no-vdd`：不管理虚拟副屏，按 `capture.monitor_match` 使用已有显示器（无板自测/物理屏演练）。
- `--dump-frame`：每次全刷结束时把当前帧写成 BMP（A2 比对用，文件被覆盖）。
- Ctrl-C 正常退出（退出码 0）。

## 配置（config.ini）

| 段/键 | 默认 | 说明 |
|---|---|---|
| server.ip / port | 192.168.1.14 / 5901 | 板端地址 |
| display.fps_cap | 20 | 帧率上限（SET_QUALITY 与背压只降不升） |
| display.full_refresh_sec | 5 | 周期自愈全刷间隔（有脏块时才发） |
| display.format | bgra32 | 初始建议格式，板端可经 HELLO_ACK/SET_QUALITY 决定 |
| display.manage_vdd | 1 | 1=启动时启用 VDD、退出时禁用；0=使用已有显示器（等同 `--no-vdd`） |
| display.virtual_resolution | 1024x600 | 分辨率选择框的默认项（选择后会写回） |
| capture.mode | gdi | gdi / dxgi；dxgi 优先 Desktop Duplication，失败自动回退 GDI |
| capture.monitor_match | 1024x600 | `WxH` / `index:N` / `name:\\.\DISPLAYn`；管理 VDD 时运行时改为按设备名定位 |
| tile.size / lz4 | 64 / 1 | 分块大小固定 64；LZ4 开关 |
| cursor.composite | 1 | 合成硬件光标（光标不在采集画面内） |

## 自测（无板）

```powershell
# 终端 1：本机模拟接收端（mock_receiver 属 Windows 端交付物）
python tools\mock_receiver.py --port 5901 --lz4-check

# 终端 2：把 config.ini 的 server.ip 改为 127.0.0.1；
#         无 VDD 时加 --no-vdd，用 index:0 / 实际分辨率指向物理屏做协议演练
.\super_screen_win.exe --no-vdd
```

验收测试表（C1–C6）见 `docs/plan-windows.md` §7。

M3：`capture.mode = dxgi` 已实现并在 VDD 上实测可用（Desktop Duplication，capture
约 0.2–0.3ms）；800x480 会话由板端居中、四周留黑显示（A2 逐块比对 0 差异）。

注意：用大于 1024x600 的物理屏做协议演练时，若画面 LZ4 压不动，单次全刷可能超过
4MB 发送队列上限，日志出现 `queue full` 告警属预期（目标虚拟屏分辨率下无此问题）。

## M0（VDD）

VDD 安装、`vdd/vdd_settings.xml` 与验证记录（A1）在 M0 阶段补充；安装步骤见
`docs/plan-windows.md` §3。CI/本地编译不依赖 VDD。

驱动安装后设备保持存在；应用启动时启用、正常退出时禁用。正常退出后设备保持**禁用**状态，
因此未运行应用时开机不会自动多出一块虚拟副屏。若第一次从旧版本升级，设备可能仍处于启用
状态，运行一次应用并正常退出即可。

## 启动方式（不再开机自启）

按需求改为**点击应用后再创建虚拟副屏**，不再安装登录自启任务。若旧机器上注册过旧版任务，
先删除：

```powershell
Unregister-ScheduledTask -TaskName super_screen_win -Confirm:$false
```

然后直接双击 `super_screen_win.exe`（或创建桌面快捷方式），在分辨率选择框里确认后使用。
`--no-vdd` 模式不会创建/删除虚拟副屏。
