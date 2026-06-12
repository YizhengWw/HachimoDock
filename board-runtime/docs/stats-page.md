# 设备端统计页（负一屏）

> 本文档描述板端 token 统计 + 屏幕轮播（页 0 = 统计页 / 页 1 = 桌宠主屏）的实现细节。整体架构遵循 [device-runtime-design.md](./device-runtime-design.md) 的模块边界。

> **通用功能组件渲染**：对外开源版本一期之后用户可自定义负一屏组件,该路径使用并列的 `COMPONENT_DASHBOARD_V1` 契约,见 [component-dashboard-v1.md](component-dashboard-v1.md)。本文档（STATS_DASHBOARD_V1）描述设备内部的统计页保留接口。

## 1. 数据流总览

```
pet-claw bridge ──MQTT──► broker ──► board-server
                                        │
                                        ├── runtime_protocol.c
                                        │     br_bridge_state_from_message
                                        │     提取 tokenUsage（顶层 / payload.tokenUsage）
                                        │
                                        ├── runtime_session_state.c
                                        │     已有 session 优先级机器
                                        │
                                        └── runtime_stats.c          ◄── 本次新增
                                              ingest delta 累加
                                              today.json + sessions.json 落盘
                                              .stats-display 仪表盘 payload

board-touch-input.c
  swipe_left  ──► .screen-page=stats
  swipe_right ──► .screen-page=main

fb-speech-overlay.c
  if .screen-page = stats: 解析 .stats-display 并绘制全屏仪表盘
  else:                    渲染 .current-speech （现状）

fb-display.sh
  if .screen-page = stats: 不切 touch clip / 不响应 .current-state
  else:                    保持现状播放桌宠 idle / working 动画
```

## 2. 文件契约

| 路径 | 写入方 | 读取方 | 说明 |
|---|---|---|---|
| `<root>/.screen-page` | board-touch-input / board-server | fb-speech-overlay / fb-display.sh | 屏幕页号文本（`main` / `stats`）。默认 `main`。 |
| `<root>/.stats-display` | board-server (`runtime_stats_flush`) | fb-speech-overlay | `STATS_DASHBOARD_V1` 结构化文本 payload，含 agent、工作午餐换算、token 指标和 source 摘要。 |
| `<root>/stats/today.json` | board-server | 调试 / 远程拉取 | 当日聚合（机器可读）。 |
| `<root>/stats/sessions.json` | board-server | board-server (启动 reload) | 每个 (source, sessionKey 或 sessionId) 的 prev 值，重启可恢复。 |
| `<root>/stats/YYYY-MM-DD.json` | board-server | （历史） | 跨天 rollover 时归档。 |

## 3. token 累加模型

`tokenUsage.totalTokens` 在 pet-claw 端是 session 内的单调累加值（详见 [headless-mqtt.js#normalizeTokenUsage](../../claw-pet-manager/ref/src-tauri/bridge/packages/clawd-backend-service/src/headless-mqtt.js)）。板端用 delta：

- key = `(source, sessionKey 或 sessionId 或 runId)`
- 每次新值 `cur > prev` → `delta = cur - prev`，加进 today；`prev := cur`
- `cur < prev`（session 重启 / 新会话）→ 当作起点，不产生 delta
- `tokenUsage` 缺失（Claude / Cursor / Gemini hook 帧）→ 跳过 ingest
- 跨天 rollover：检测 `today.dateStamp != localDate(now_ms)` 时归档昨日并重置今日

## 4. 工作午餐换算与仪表盘 payload（展示层）

- 默认 1 顿工作午餐 = 350,000 tokens（环境变量 `PET_CLAW_STATS_TOKENS_PER_LUNCH` 可改；旧的 `PET_CLAW_STATS_TOKENS_PER_COFFEE` 仍作为兼容 fallback）。
- `lunchCount = today.totals.totalTokens / tokensPerLunch`，展示保留 1 位小数。
- `.stats-display` 不再是普通多行文案，而是结构化文本 payload：

```text
STATS_DASHBOARD_V1
agent=Codex
eyebrow=等价于购买了
lunch=3.7
headline=约 3.7 顿工作午餐
metricTitle=今日累计 Token
metricValue=1.30M
metricUnit=TOKEN
alerts=1
completed=1
breakdown=输入 900.0K · 输出 400.0K · 缓存 0
sources=codex 1.30M
```

> token 累计和换算逻辑全部在 `runtime_stats.c` 的 `runtime_stats_render_display` 内；`fb-speech-overlay` 只识别 payload 字段并绘制 framebuffer。

## 5. 屏幕轮播

| 当前页 | 输入 | 写者 | 动作 |
|---|---|---|---|
| main (1) | swipe_left | board-touch-input | `.screen-page=stats` |
| stats (0) | swipe_right | board-touch-input | `.screen-page=main` |
| 任意 | MQTT `desk/<id>/control/screen-page {"page":"stats|main"}` 或字符串 `➡` | board-server | 同步写 `.screen-page` |
| 任意 | `POST /screen/page {"page":"..."}` | board-server | 同上（curl 调试用） |

## 6. 环境变量

| 名称 | 默认值 | 说明 |
|---|---|---|
| `PET_CLAW_STATS_TOKENS_PER_LUNCH` | `350000` | 一顿工作午餐的 token 数 |
| `PET_CLAW_STATS_TOKENS_PER_COFFEE` | - | 兼容旧配置；当 `PET_CLAW_STATS_TOKENS_PER_LUNCH` 未设置时生效 |
| `PET_CLAW_STATS_TZ_OFFSET_SEC` | `28800` | 时区偏移秒（北京时间）|
| `PET_CLAW_FB_SPEECH_HOLD_SECONDS` | `10` | 主屏文字 hold 时长（统计页强制 86400） |

## 7. 调试速查

```bash
export BOARD_HOST="<pi-user>@<pi-ip>"
export BOARD_IP="<pi-ip>"
export MQTT_PASSWORD="<MQTT_PASSWORD>"

# 抓真实 retained MQTT 数据
mosquitto_sub -h broker.openclaw.example -p 1883 -u device -P "$MQTT_PASSWORD" \
  -t 'desk/+/state/+' -v -C 10

# 模拟一帧带 1 亿 token
mosquitto_pub -h broker.openclaw.example -p 1883 -u device -P "$MQTT_PASSWORD" \
  -t 'desk/<board-id>/state/codex' -r \
  -m '{"state":"working","source":"codex","sessionId":"sim","tokenUsage":{"totalTokens":100000000,"inputTokens":50000000,"outputTokens":50000000}}'

# 切到统计页（三种方式等价）
ssh "$BOARD_HOST" "echo stats | sudo tee /opt/board-runtime/.screen-page >/dev/null"
mosquitto_pub -t 'desk/<board-id>/control/screen-page' -m '{"page":"stats"}'
curl -X POST http://$BOARD_IP/screen/page -d '{"page":"stats"}'
curl -X POST http://$BOARD_IP/screen/page -d '➡'   # alias

# 板上看落盘的累计
ssh "$BOARD_HOST" "cat /opt/board-runtime/stats/today.json"
ssh "$BOARD_HOST" "cat /opt/board-runtime/.stats-display"
```

## 8. 单测覆盖

`tests/runtime_tests.c` 覆盖：

- `br_bridge_state_from_message` 提取 tokenUsage：camelCase top-level / snake_case in payload wrapper / 缺失帧三种 fixture
- `runtime_stats_ingest`：delta 累加、session 切换重置、缺失帧跳过
- `runtime_stats_render_display`：0 / 1 / 100+ 杯三档分支、`compact` 单位（K / M / B）
- 持久化：flush + re-init 还原 prev 值
- 跨天 rollover：归档 YYYY-MM-DD.json + today 重置
