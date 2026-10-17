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
.\super_screen_win.exe [--config FILE] [--dump-frame frame.bmp]
```

- `--dump-frame`：每次全刷结束时把当前帧写成 BMP（A2 比对用，文件被覆盖）。
- Ctrl-C 正常退出（退出码 0）。

## 配置（config.ini）

| 段/键 | 默认 | 说明 |
|---|---|---|
| server.ip / port | 192.168.1.14 / 5901 | 板端地址 |
| display.fps_cap | 20 | 帧率上限（SET_QUALITY 与背压只降不升） |
| display.full_refresh_sec | 5 | 周期自愈全刷间隔（有脏块时才发） |
| display.format | bgra32 | 初始建议格式，板端可经 HELLO_ACK/SET_QUALITY 决定 |
| capture.mode | gdi | v1 仅 gdi（DXGI 为 M3，配置为 dxgi 时自动回退） |
| capture.monitor_match | 1024x600 | `WxH` / `index:N` / `name:\\.\DISPLAYn` |
| tile.size / lz4 | 64 / 1 | 分块大小固定 64；LZ4 开关 |
| cursor.composite | 1 | 合成硬件光标（光标不在采集画面内） |

## 自测（无板）

```powershell
# 终端 1：本机模拟接收端（mock_receiver 属 Windows 端交付物）
python tools\mock_receiver.py --port 5901 --lz4-check

# 终端 2：把 config.ini 的 server.ip 改为 127.0.0.1；
#         无 VDD 时可用 index:0 / 实际分辨率临时指向物理屏做协议演练
.\super_screen_win.exe
```

验收测试表（C1–C6）见 `docs/plan-windows.md` §7。

注意：用大于 1024x600 的物理屏做协议演练时，若画面 LZ4 压不动，单次全刷可能超过
4MB 发送队列上限，日志出现 `queue full` 告警属预期（目标虚拟屏分辨率下无此问题）。

## M0（VDD）

VDD 安装、`vdd/vdd_settings.xml` 与验证记录（A1）在 M0 阶段补充；安装步骤见
`docs/plan-windows.md` §3。CI/本地编译不依赖 VDD。

## 自启（M2）

不做 Windows 服务（服务运行在会话 0，取不到交互桌面与显示器拓扑）。用任务计划程序，
触发器 = 用户登录，延迟 30s，工作目录 = exe 所在目录（`config.ini` 与 exe 必须同目录）：

```powershell
# 管理员 PowerShell；默认使用 build\x64\Release\super_screen_win.exe
powershell -ExecutionPolicy Bypass -File windows\scripts\install_autostart.ps1

# 指定部署路径
powershell -ExecutionPolicy Bypass -File windows\scripts\install_autostart.ps1 `
    -ExePath D:\apps\super_screen\super_screen_win.exe

# 卸载
powershell -ExecutionPolicy Bypass -File windows\scripts\install_autostart.ps1 -Uninstall
```

任务以当前用户的交互令牌运行（RunLevel Limited），注册任务需要管理员权限，应用本身不需要。
