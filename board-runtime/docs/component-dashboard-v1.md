# COMPONENT_DASHBOARD_V1 — 设备端契约

通用功能组件的负一屏渲染契约。`fb_speech_overlay` 在 `br_overlay_build_frame` 中优先尝试 `br_component_dashboard_parse`,命中后走 `br_overlay_render_component_dashboard` 渲染。与既有 `STATS_DASHBOARD_V1` 并列、不互相替换。

## Payload 格式

文本文件,通常通过 `.stats-display`(或后续阶段的专门文件契约)送达。首行是版本标识,其后每行 `key=value`。整个 payload 必须 < 2048 字节。

```
COMPONENT_DASHBOARD_V1
title=摸鱼倒计时
eyebrow=距离今天下班
headline=还有 2 小时 13 分
metricLabel=下班时间
metricValue=18:00
metricUnit=
badge=5
note=本周已坚持 5 天
footer=红钮 切显示 · 旋钮 调下班时间 · 长按 重设
progress=30:本次间隔
```

## 10 个槽位

| key (camelCase) | maxBytes | 设备版式角色 | C 字段 |
|------|----------|--------------|--------|
| `title`       | 60  | 左上角无外框标题 | `title[64]`         |
| `eyebrow`     | 90  | 标题下小号说明   | `eyebrow[96]`       |
| `headline`    | 156 | 右上角橙色状态句,空间不足时回落到正文 | `headline[160]`     |
| `metricLabel` | 90  | 指标面板标题     | `metric_label[96]`  |
| `metricValue` | 60  | 指标大号数值     | `metric_value[64]`  |
| `metricUnit`  | 30  | 数值后小号单位   | `metric_unit[32]`   |
| `badge`       | 12  | 右上角绿色圆数字 | `badge[16]`         |
| `note`        | 156 | 指标面板内小号说明 | `note[160]`         |
| `footer`      | 156 | 底部硬件操作提示 | `footer[160]`       |
| `progress`    | 64  | 指标面板内进度条,格式 `<0-100>:<label>` | `progress[64]`      |

字节数为 UTF-8 字节上限（CJK 1 字 ≈ 3 字节）。超出 maxBytes 的输入由 `br_normalize_text` 安全截断。

未知 key 被解析器忽略。

## 与 STATS_DASHBOARD_V1 的关键差异

- **`note` / `footer` / `progress` 在 compact 模式（fb->height ≤ 540,真机 480 屏恒为 compact）也渲染**。`note` 和 `progress` 固定在指标面板内,避免挤压底部操作提示。
- `title` 不再画绿色胶囊外框,只保留左上角标题文字;`headline` 优先作为右上角状态句渲染,缓解小屏顶部拥挤。
- 右上角徽章只画一个绿色 `badge` 圆,不画 STATS 的 amber alert 圆。
- 槽位 key 为 camelCase（`metricLabel`/`metricValue`/`metricUnit`）以对齐 claw-pet-manager 端 JS payload；STATS 用的是 `metricTitle`。

## 上游契约

由 `claw-pet-manager/ref/src/lib/clawpkg-contract.js` 中的 `COMPONENT_DASHBOARD_V1_SLOTS` 定义同步。两端 9 槽位 id 与 maxBytes 必须严格一致;修改契约时必须同步两端。

## 实现入口

- `src/fb_speech_overlay.c`:
  - `br_component_dashboard_model` (struct)
  - `br_component_dashboard_defaults / set_value / parse / combined_text` (helpers)
  - `br_overlay_render_component_dashboard` (renderer)
  - `br_overlay_build_frame` (dispatcher,优先尝试 component 解析)
- `tests/fb_speech_overlay_layout_tests.c` 覆盖 parse / 拒非 magic / render note+footer / 截断 / 未知 key 五个场景。

## USB OTA 投放

由 `claw-pet-manager` 的 `install_clawpkg_over_usb` Tauri 命令承担投放:

1. 客户端调用 `clawpkg::validate_clawpkg_at_path()` 解 `.clawpkg.zip` 并校验。
2. 用 `clawpkg::render_component_dashboard_payload()` 把 `negative-screen.json` 的 dashboard 转成本文件描述的 `COMPONENT_DASHBOARD_V1` 文本 payload。
3. 通过 USB 串口发送 JSON-line `{"v":1,"type":"payload_write","path":".stats-display","content":"..."}`,设备端 `board_serial_bridge` 走 `br_apply_payload_write` 助手把 payload 原子写入 `<runtime_root>/.stats-display`。
4. 紧接着客户端发送另一条 `payload_write` 把 `.screen-page` 切到 `stats`,通知 overlay 切到 stats 屏。
5. `fb-speech-overlay` 在下一次 poll 读取 `.stats-display`,触发 `br_component_dashboard_parse` → `br_overlay_render_component_dashboard`。

允许写入的设备文件由 `br_apply_payload_write` 内嵌白名单约束,目前只放行 `.stats-display`、`.current-speech`、`.screen-page`。
