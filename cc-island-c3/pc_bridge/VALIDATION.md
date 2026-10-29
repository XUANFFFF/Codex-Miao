# 第三轮修复验证记录

日期：2026-10-29。基于 `c103d223` 的第三轮审核修复，最终提交号见本文件 Git 历史。

| 项目 | 证据与边界 |
| --- | --- |
| Windows | Windows 11 家庭中文版，10.0.26200 |
| Codex | PATH CLI 0.134.0；桌面运行时 0.165.0-alpha.1.2 |
| Bridge 运行路径 | 原托盘及开机启动项指向 `C:\tmp\esp32-ascii` 旧副本；已备份启动项，并切换到当前仓库 |
| 本机 HTTP | `/health` 返回 `ok: true`、`protocol_version: 2`、`activity.monitor_ok: true`、`last_error: ""`；正在执行的任务识别为 `working` |
| Hook 部署 | 8 个事件均已注册，脚本和 Python 存在；用户确认已经审核并信任 |
| Hook 真实回调 | 检查时 `last_recorded_event: null`；尚未证实当前桌面会话已加载 Hook。需在新本地会话中执行 README 的授权卡验收 |
| ESP32 本地编译 | PlatformIO 6.2.0 / espressif32 7.0.1 / Arduino 2.0.17；ESP32-C3 编译成功。RAM 41,148/327,680；应用 Flash 945,530/1,310,720 字节 |
| 串口 | COM3，Espressif USB VID 303A / PID 1001；健康查询得到 `OK`，未收到协议 2 的健康 JSON。新版固件是否已运行尚未确认 |
| 刷写 | 此记录创建时未执行固件刷写；未进行全 Flash 转储 |
| 实机状态切换 | 原生授权待决、批准、拒绝、取消及屏幕 `NEED OK` 尚无完整联调证据 |
| 物理重连与 Wi-Fi | 拔插 USB、Wi-Fi 断线和恢复尚未验收 |

Python 回归覆盖并行授权、无关工具完成、同输入歧义、重复结果、旧轮次事件、子代理隔离、压缩会话以及历史缓存清理。这些是合成事件测试，不能替代真实授权卡与屏幕观察。固件原生逻辑测试由 PR 的 CI 单独验证。

当 `PermissionRequest` 缺少调用 ID 且不能唯一匹配前置调用时，Bridge 会保留提示直到明确结束/中断或超时；这是一项已知保守行为。批准后要等对应工具结果才清除提示，并非在点击批准的瞬间清除。
