# Windows Bridge 与授权提示部署

本文适用于本地 Windows Codex + ESP32-C3。Hook 只记录状态，不会批准、拒绝或修改工具请求。

## 1. 启动 Bridge

在 PowerShell 进入仓库根目录。以下路径对应本项目当前工作目录；仓库位于其他位置时请替换路径。

```powershell
Set-Location -LiteralPath 'D:\工作文件\工作文件\14.ai项目\esp32'
python --version
python cc-island-c3\pc_bridge\serve_usage.py
```

需要 Python 3.10 或更高版本，以及已经配置好的本地 `cc-island-c3/src/wifi_secrets.h`。该文件包含本机 Wi-Fi 与 Bridge Token，应保持在 Git 忽略范围内。已有托盘 Bridge 运行时，先从托盘退出再启动，避免重复占用 8765 端口。

另开 PowerShell 检查：

```powershell
Invoke-RestMethod 'http://127.0.0.1:8765/health' | ConvertTo-Json -Depth 6
Invoke-RestMethod 'http://127.0.0.1:8765/usage?format=json' | ConvertTo-Json
```

`ok: true` 只说明 HTTP 可用；还应检查 `activity.monitor_ok`、`last_error`、`tracked_files` 和会话状态。云端配额暂不可用时，`usage_available` 可以为 `false`，本地状态仍应更新。健康响应仅包含会话 ID、状态及时间，不包含用户提示或工具命令。

更新仓库后，应退出已有托盘，再从当前仓库运行 `usage_tray.ps1`。使用开机启动时重新运行 `install_startup.ps1`，避免启动项仍指向旧副本。健康响应缺少 `protocol_version: 2` 或 `activity` 时，先检查运行进程的脚本路径。

### 登录后自动启动

先结束手动启动的 USB 转发，避免相对路径启动的旧进程占用串口；然后安装串口依赖并注册当前仓库的启动项：

```powershell
python -m pip install pyserial
powershell.exe -NoProfile -ExecutionPolicy Bypass -File cc-island-c3\pc_bridge\install_startup.ps1 -UsbPort COM3
```

下次登录 Windows 时，同一个 `Codex Usage Tray.vbs` 启动项会隐藏启动 Bridge 托盘和 USB 转发。设备暂未连接、Bridge 尚未就绪时，USB 转发会重试。启动项会检查同路径脚本是否已运行，避免重复启动；修改串口或 Python 路径后，应先退出已有转发进程，再重新安装。仅使用 Wi-Fi 时，重新安装并加 `-SkipUsb`。

不用等待重启即可测试：在资源管理器地址栏输入 `shell:startup`，双击 `Codex Usage Tray.vbs`，然后检查 Bridge 健康响应和屏幕是否持续更新。这里的“开机启动”是用户登录后启动，不是登录前的系统服务。

退出托盘只结束它管理的 Bridge；USB 转发独立运行，等待 Bridge 恢复。需要临时停止 USB 时，可在任务管理器的“详细信息”中显示“命令行”列，仅结束指向本仓库 `send_serial.py` 的 Python 进程。停止全部组件时先停 USB，再退出托盘。删除自动启动使用 `uninstall_startup.ps1`；这只移除下一次登录的启动项，不结束当前进程。Python 环境需保持已安装 pyserial。

## 2. 安装与检查 Hook

在仓库根目录执行，安装、检查和卸载应使用同一个 Python 环境：

```powershell
python cc-island-c3\pc_bridge\install_permission_hook.py
python cc-island-c3\pc_bridge\install_permission_hook.py --check
```

安装器向 `%USERPROFILE%\.codex\hooks.json` 添加本项目处理器，保留其他 Hook；重复执行不会重复添加。当前应注册 8 个事件：`PreToolUse`、`PermissionRequest`、`PostToolUse`、`Stop`、`Interrupt`、`UserPromptSubmit`、`SessionStart`、`SessionEnd`。从旧版的 7 个事件升级时，也需要重新执行安装命令。

`--check` 是只读检查，报告缺失事件、脚本和 Python 是否存在、待处理请求数量，以及最后记录的 Hook 事件。它不会自动信任 Hook，也不能证明 Codex 当前会话已经加载配置。最后事件不存在表示尚无记录；存在也需核对是否来自刚刚进行的真实会话。

状态文件在 `%LOCALAPPDATA%\CodexMiao\permission_state.json`，保存会话、轮次、调用 ID、输入 SHA-256 摘要和时间；不保存命令、工具返回值或授权决定。不要把此文件作为授权审计记录。

## 3. 在 Codex 中审核并信任

在 PowerShell 运行 `codex`，进入 Codex CLI 后，在 **Codex 输入框**输入：

```text
/hooks
```

找到来源为用户级 `hooks.json`、命令指向本仓库 `permission_request_hook.py` 的 8 个处理器，逐项审核、信任并启用。`PreToolUse` 也只观察工具调用，不返回允许或拒绝决定。不要把 `/hooks` 当作 PowerShell 命令执行。

Codex 官方要求未受管理的 Hook 先审核并信任；新增或定义变化的 Hook 在信任前会跳过。参见 [官方 Hook 文档](https://learn.chatgpt.com/docs/hooks#review-and-trust-hooks)。无需使用跳过信任的启动参数。

新开一个本地 Codex 会话验证。如果当前桌面会话始终没有新回调，可在结束任务后重启桌面应用，再新开会话检查。CLI 中完成信任，不应单独记作桌面端联调通过；以真实回调和屏幕表现为准。若管理员策略禁用了用户 Hook，需要由管理员检查有效策略。

## 4. 验证真实授权链路

保持 Bridge 运行，并使用已经包含当前显示协议的固件。先确保 Codex 的权限模式会实际展示授权卡：全自动批准或没有授权卡的操作，不能算作人工批准/拒绝验收。

在 Codex 中发起一个确实需要原生授权、且可安全拒绝的操作，例如需要网络授权时读取公开网页。然后在另一窗口执行 `--check`，并观察 `/usage` 与 ESP32。

| 场景 | 应观察到的结果 |
| --- | --- |
| 原生授权卡仍待答复 | Hook 已记录 `PermissionRequest`，Bridge 为 `permission`，屏幕显示 `NEED OK` |
| A 待授权，另一工具 B 完成 | A 的待授权记录和 `NEED OK` 保留 |
| 用户批准 A | 对应调用的 `PostToolUse` 到达后，才清除 A；其他请求保留 |
| 用户拒绝 A | 若 Codex 提供对应工具结果则清除 A；否则该轮明确停止或中断后清除，不能凭其他工具完成推断 |
| 用户中断 | 该会话、该轮次的待处理请求清除，其他会话保持 |
| 同一会话有两个待授权请求 | 一个完成只清除可唯一对应的那个请求 |
| 完全相同的输入并行，无法唯一关联 | 保守保留未关联请求，直到该轮明确结束/中断；不要按输入相同或完成时间猜测 |

当前 Codex 文档中 `PermissionRequest` 不保证提供 `tool_use_id`，而 `PreToolUse`/`PostToolUse` 提供调用 ID。Bridge 用前置事件的输入摘要做唯一关联，实际清理依据调用 ID。未安装/未信任前置 Hook、输入被其他 Hook 改写、缺失轮次或关联歧义时，提示可能停留更久；待处理记录还有 24 小时失效上限。批准瞬间不一定已有工具结果，所以不承诺立即从 `NEED OK` 切换。参见 [官方事件字段](https://learn.chatgpt.com/docs/hooks#permissionrequest)。

旧版本遗留的无调用 ID 记录也遵循保守清理。日志中的普通工具输出不再清除 Hook 待授权状态；只有匹配轮次的明确结束事件可以作为日志侧兜底。

## 5. USB 与 Wi-Fi 验收

USB 转发默认从同一个本地 Bridge 读取状态。需要时安装串口依赖：

```powershell
python -m pip install pyserial
python cc-island-c3\pc_bridge\send_serial.py --port COM3 --diagnose
python cc-island-c3\pc_bridge\send_serial.py --port COM3
```

将 `COM3` 替换为设备实际端口。健康查询应返回 `device: codex-miao-c3` 和 `protocol_version: 2`；只有 `OK` 而没有健康 JSON，不能确认固件版本或新版协议已经运行。串口监视器、烧录器和 USB 转发器不能同时占用同一端口。

固件在 USB 初始化前分配 2,050 字节的接收队列，以容纳最大 1,024 字节状态行及换行。Arduino HWCDC 默认队列只有 256 字节；旧版固件可能表现为短健康查询正常、完整状态同步超时或 `ERR`，此时需要更新固件。

验收步骤：

1. 工作和待授权时，核对 Bridge、USB 与屏幕显示一致。
2. 保持转发器运行，拔插 USB；确认错误被报告、端口重新出现后能够重连。
3. USB 连续供电时，断开 Wi-Fi；若 USB 正在同步，状态应继续更新。
4. 停止 USB 转发，确认 Wi-Fi 可接管；再让 Wi-Fi 断线，确认屏幕不会误显示空闲。
5. 恢复 Wi-Fi，确认重新发现 Bridge，并显示当前状态而非旧快照。

Wi-Fi 通信使用 TCP 8765 和 UDP 8766；需要允许 Windows 当前专用网络的相应入站通信。存在 VPN/虚拟网卡时，可通过 `--discovery-host` 指定本机实际局域网 IPv4。HTTP 健康检查成功不代表 ESP32 所在网络已经能访问这两个端口。

每次验收记录日期、Windows/Codex 版本、提交号、串口、固件健康响应和各场景的实际观察。合成 Hook JSON、Python 回归和 CI 固件编译都是软件验证，不能替代原生授权卡与真实设备验收。

## 6. 长期常驻的清理策略

已经观察到的运行、思考、后台进程和待授权会话不会因日志安静而被清理。普通闲置记录在最后读取 15 分钟后移除，同时最多缓存 64 条闲置记录；明确完成/错误提示先保留 60 秒。被移除文件仅保留最多 512 条轻量指纹，避免每次扫描重新读入相同历史；文件变化时可恢复跟踪，旧完成事件不会作为新提示重播。清理只影响内存，不删除 Codex JSONL 文件。

## 7. 卸载 Hook

先结束或中断正在等待授权的任务，再用安装时同一个 Python 环境和仓库路径执行：

```powershell
python cc-island-c3\pc_bridge\install_permission_hook.py --remove
python cc-island-c3\pc_bridge\install_permission_hook.py --check
```

卸载只移除本项目的命令条目，其他 Hook 保留。卸载后 `--check` 报告本项目事件缺失并返回非零是预期结果。结束已有会话后，在 Codex `/hooks` 确认条目已移除；必要时重新打开本地会话。

如果所有相关会话都已结束，但还保留旧待处理提示，可清除本项目的状态文件：

```powershell
Remove-Item -LiteralPath "$env:LOCALAPPDATA\CodexMiao\permission_state.json" -ErrorAction SilentlyContinue
```

不要删除整个 `.codex` 目录或覆盖其他 Hook 配置。卸载 Hook 不会卸载托盘/Bridge；托盘自启动使用同目录的 `uninstall_startup.ps1` 单独移除。

## 8. Windows 固件编译

安装 PlatformIO 后在仓库根目录执行 `pio run -d cc-island-c3 -e esp32-c3-supermini`。若 Windows 链接器因中文构建路径无法创建 map 文件，可将当前 PowerShell 的构建目录指定到英文临时路径后重试：

```powershell
$env:PLATFORMIO_BUILD_DIR = Join-Path ([System.IO.Path]::GetTempPath()) 'codex-miao-build'
pio run -d cc-island-c3 -e esp32-c3-supermini
```

使用真实 `wifi_secrets.h` 编译的固件含有本机凭据，不应上传到 GitHub。构建成功并不表示已经刷写设备。当前实机证据与待验项目见 [VALIDATION.md](VALIDATION.md)。
