# 桌面快捷键服务（PC 宿主服务 `hotkeys`）

把设备变成一块有界的快捷键面板：资源包声明场景与组合键，设备按键经 PC 宿主服务
向**这台电脑当前前台应用**发送组合键。它不是宏、文本输入、远程控制或任意脚本。

## 包内声明

`pc/application.json`：

```json
{"schemaVersion": 1, "engine": "pc-app-services-v1", "service": "hotkeys",
 "actions": {"host.profile": "profile", "host.one": "key_1", "host.two": "key_2", "host.three": "key_3",
             "host.four": "key_4", "host.five": "key_5", "host.six": "key_6"},
 "content": [
   {"id": "common", "title": "常用", "keys": [
     {"label": "复制", "chord": ["cmd", "c"]}, {"label": "粘贴", "chord": ["cmd", "v"]}]}
 ]}
```

- 1–4 个场景（`id` 1–24 位小写/数字/短横线，`title` ≤30 字节），每个 1–6 个键。
- `chord` 为 1–4 个字符串：最多 3 个不重复修饰键 `ctrl|shift|alt|cmd`，最后一个是按键：
  `a-z`、`0-9`、`f1-f12`、`enter esc tab space backspace delete up down left right home end
  pageup pagedown minus equal comma period slash semicolon quote backquote lbracket rbracket backslash`。
  没有媒体键（播放/音量）、没有鼠标、没有序列。`cmd` 在 Windows 上映射为 Win 键。
- 动作只能是 `profile` 与 `key_1..key_6`，`key_n` 不得超过场景中最少的键数；`profile` 必须声明。
  宿主动作名仍遵守 `host.` + 小写字母/下划线（不能含数字），例如 `host.one`。
- 设备 runtime 仍是 `host:true` + `p4-app-surface-v1` + `p4-input-v1`，无 vars/tick/persist；
  服务回填 `eyebrow`（场景名 n/N）、`headline`（①～⑥ 键名）、`metricValue`（发送状态）、
  `badge`（已发送次数）、`note`（最近一次结果或提示）。

## 发送边界

- **默认关闭。** 每台电脑、每个组件各自在 PC 组件详情里勾选"允许向这台电脑发送快捷键"，
  才会真正按键；开关保存在组件服务目录 `hotkeys-settings.json`，只能从 PC 修改，设备事件改不了。
- macOS 需要用户在 系统设置 › 隐私与安全性 › 辅助功能 允许 Pet Manager；未授权时记为"未发送"。
  Windows 用 `SendInput`；其他平台不支持。
- 实时对话进行中不发送；组件退出、服务停用后不再发送。设备显示"已发送"只表示操作系统收到了
  按键事件，不保证目标软件执行。
- 诊断/快照里只出现组合键名称与计数，不出现目标应用内容。

## 验收

- `validate_generated_widget.py` 走 `app_services.py` 的镜像校验；Rust `app_services::parse` 与
  `hotkeys::validate_profiles` 在安装/发布时再校验一次。
- `smoke_test_widget_game.py` 对宿主包只验证动作声明契约（`hostServicesVerified=false`）。
- Rust 单测覆盖白名单、场景边界、开关默认关闭；真实按键发送需在 macOS/Windows 上人工验收。
