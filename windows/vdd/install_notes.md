# M0 安装记录：VDD 虚拟显示驱动

## 环境

| 项 | 值 |
|---|---|
| 系统 | Windows 10 Pro 22H2 (19045) |
| GPU | NVIDIA GeForce RTX 5060 |
| VDD 版本 | 25.7.23（release 25.7.23，已签名） |
| 安装包 | `VDD.Control.25.7.23.zip`，sha256 `a701f2272e9fcf382849b24f913c6dd07597b3b1116525f2e90182f019609154`（与 `docs/plan-windows.md` §3.1 一致） |
| 安装日期 | 2026-10-17 |

## 安装方式

1. 下载官方 release 并校验 sha256（通过）。
2. 控制 App 的 CLI 模式（`VDD Control.exe INSTALL_DRIVER`）在无人交互环境下会挂起，未采用；
   改用包内官方 `Dependencies\devcon.exe`（与控制 App 使用同一工具）手动安装：
   ```powershell
   devcon install <pkg>\SignedDrivers\x86\VDD\MttVDD.inf Root\MttVDD
   ```
   （`x86` 目录内 INF 为 `NTamd64`，即 x64 驱动包；ARM64 目录未使用。）
3. 将仓库内 `windows/vdd/vdd_settings.xml` 复制到 `C:\VirtualDisplayDriver\vdd_settings.xml`
   （驱动从该路径加载分辨率与选项）。
4. `DisplaySwitch.exe /extend`，副屏自动落在主屏右侧。

## 验证结果

- 设备管理器 / PnP：
  - `ROOT\DISPLAY\0001` → **Virtual Display Driver**，Status OK
  - 监视器 `DISPLAY\MTT1337\...`，Status OK
  - `WmiMonitorID` 出现 **"VDD by MTT"**（EDID 已枚举）
- 显示拓扑（应用枚举）：`\\.\DISPLAY15` **1024x600**，位置 `(2560,0)`（主屏右侧）
- 当前模式 1024x600@60（`vdd_settings.xml` 仅声明 60Hz，窗口无法选到其他刷新率）
- 端到端本地验证：应用采集该副屏 → mock_receiver（`--lz4-check`）：
  - HELLO/ACK、FULL_BEGIN→160 tiles→FULL_END、PING/PONG 全部正常，`errors=0`
  - `capture_ms_avg≈6.2ms`，`--dump-frame` 产出 1024x600 32bpp BMP
- 注：本机另有 `OrayIddDriver`（向日葵），两者共存无冲突。

## 待办 / 已知项

- Win11 机器上的 A1 复验尚未执行（C4 需要第二台机器）。
- 800x480 模式已在配置中声明，运行时切换验证留待 M3。
- 物理控制台"显示设置"肉眼确认建议由人工完成（命令行枚举已确认设备与模式）。
- 建议显示缩放保持 100%（本机为 1024x600 小屏，Windows 默认通常为 100%）。

## 按需创建（2026-10 调整）

- 驱动保持安装，但应用未运行时 VDD 设备默认禁用，因此开机后不会自动出现虚拟副屏。
- `super_screen_win.exe` 启动时先让用户选择分辨率，再启用 VDD、设置模式；退出时禁用。
- 手动移除残留副屏：双击 `super_screen_win.exe`，在分辨率选择框点“移除副屏”；或运行
  `super_screen_win.exe --remove-vdd`。
- 命令行检查/清理（管理员）：

  ```powershell
  pnputil /disable-device "ROOT\DISPLAY\0001"
  pnputil /enable-device  "ROOT\DISPLAY\0001"
  ```

  实例 ID 以 `pnputil /enum-devices /class Display` 的实际输出为准。

## 回滚 / 卸载

1. 优先：以管理员运行 `VDD Control.exe UNINSTALL_DRIVER`（或设备管理器卸载 "Virtual Display Driver"）。
2. 命令行：`devcon remove Root\MttVDD`（管理员）。
3. 若出现黑屏/显示异常（官方已知问题）：按 `docs/plan-windows.md` §3.4 —— 强制关机进入
   安全模式，设备管理器卸载 VDD；本项目未开启测试模式，无需 `bcdedit` 改动。
4. 卸载后可删除 `C:\VirtualDisplayDriver\`。
