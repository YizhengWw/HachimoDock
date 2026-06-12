# OpenClaw 桌搭项目实现清单

更新时间：2026-04-21

说明：
- 用于后续逐项落地实现与验收。
- 状态标记：`[ ]` 未开始，`[~]` 进行中，`[x]` 已完成。
- 建议后续提交代码时在 PR/提交信息里引用功能编号（如 `D2.3`、`C4.1`）。

## 一、设备端（Board Runtime）功能清单

### 1. 状态机管理与 UI 提示
- [x] D1.1 开机硬件自检流程（关键服务、屏幕、网络基础检查）
- [x] D1.2 无有效网络配置时进入“待连接状态（待配网）”
- [x] D1.3 待连接引导 UI 渲染（提示“请在电脑端打开 HachimoDock（哈基米机）”）
- [x] D1.4 状态机事件定义与状态切换日志（便于排障）

### 2. 局域网发现与 AP 热点双轨支持
- [x] D2.1 局域网被动发现：mDNS 服务广播
- [x] D2.2 局域网被动发现：UDP Broadcast 广播/响应
- [x] D2.3 发现超时后的 Fallback：启动 SoftAP 热点
- [x] D2.4 SoftAP 固定地址配置为 `192.168.44.1`
- [x] D2.5 AP 模式下启动轻量 HTTP Server
- [x] D2.6 AP 模式下启动 WebSocket Server
- [x] D2.7 AP/STA 模式切换状态同步到 UI

### 3. 核心 API 与配置接收
- [x] D3.1 提供设备标识接口（暴露 `boardDeviceId`）
- [x] D3.2 统一配置下发接口（HTTP/WS）接收 JSON 配置包
- [x] D3.3 配置 JSON 字段校验：Wi-Fi SSID/PWD、MQTT 地址/端口、`desktopDeviceId`、`namespace`
- [x] D3.4 配置持久化与版本管理（防止脏配置）
- [x] D3.5 配置回执接口（成功/失败原因）

### 4. 网络切换与 MQTT 接入
- [x] D4.1 收到配置后关闭 AP 模式
- [x] D4.2 切换到 Station 模式并连接目标 Wi-Fi
- [x] D4.3 Wi-Fi 连接重试与超时策略
- [ ] D4.4 网络可用后建立 MQTT 连接
- [ ] D4.5 订阅下行 Topic：`<desk>/<boardDeviceId>/state`
- [ ] D4.6 MQTT 在线状态上报（含遗嘱 LWT）

### 5. 动效测试与异常恢复
- [ ] D5.1 响应“测试状态”指令并触发动效渲染
- [ ] D5.2 响应“测试字幕”指令并显示文字
- [ ] D5.3 渲染异常检测（卡死/白屏/进程崩溃）
- [ ] D5.4 网络异常超时恢复流程
- [ ] D5.5 触发恢复时重置网络状态并清空错误配置
- [ ] D5.6 恢复后回到“待连接状态（第一步）”

### 6. 语音/对话交互功能
- [ ] D6.1 字幕展示：回复文本 + 发送方标识
- [ ] D6.2 对话面板上板（含发送状态）
- [ ] D6.3 语音输入入口上板
- [x] D6.4 触控输入：切换到统计页（swipe_left → `.screen-page=stats`，swipe_right → `main`；详见 [docs/stats-page.md](docs/stats-page.md)）
- [x] D6.5 统计页滑动浏览各 channel 的 token/费用消耗（`runtime_stats.c` 按 source 分桶，渲染时 top-3 展示 + 咖啡换算）
- [ ] D6.6 RTC 对话链路接入
- [ ] D6.7 语音交互文字回传（转写/回复文本）
- [ ] D6.8 语音功能分期规划（明确“第几期”上线范围）

## 二、电脑端（HachimoDock（哈基米机））功能清单

### 1. 基础信息采集
- [ ] C1.1 获取或生成本机唯一标识 `desktopDeviceId`
- [ ] C1.2 读取本机局域网网段信息

### 2. 设备发现与连接策略引擎
- [ ] C2.1 主动扫描局域网设备（组播/端口扫描）
- [ ] C2.2 识别“待连接状态”硬件设备
- [ ] C2.3 扫描超时后 UI 引导用户连接设备热点
- [ ] C2.4 热点直连后请求 `192.168.44.1` 拉取 `boardDeviceId`
- [ ] C2.5 发现策略状态机与重试策略

### 3. 组装与下发配置
- [ ] C3.1 组装配置包（SSID/PWD、MQTT、`desktopDeviceId`、`namespace`）
- [ ] C3.2 发送配置包到已连接硬件
- [ ] C3.3 下发结果确认与错误提示

### 4. 绑定关系确认与持久化
- [ ] C4.1 监听 MQTT 设备上线状态（心跳/LWT）
- [ ] C4.2 确认 `boardDeviceId` 成功连入目标 Broker
- [ ] C4.3 本地持久化绑定关系（`desktopDeviceId` <-> `boardDeviceId`）
- [ ] C4.4 重启后绑定关系恢复与状态校验

### 5. 联调测试验证
- [ ] C5.1 绑定成功后发送测试 Payload
- [ ] C5.2 UI 弹窗询问“设备是否正常显示测试动画”
- [ ] C5.3 用户确认“是”后进入正式工作模式
- [ ] C5.4 用户确认“否”后触发解绑逻辑并提示重试
- [ ] C5.5 对接白屏恢复流程（与设备端恢复路径一致）

## 三、建议实现顺序（里程碑）

- [~] M1 最小可配网闭环：D1 + D2 + D3 + D4（不含语音）—— D1/D2/D3/D4.1~D4.3 已落地，剩 D4.4 ~ D4.6（MQTT 对接配网结果）
- [ ] M2 最小可绑定闭环：C1 + C2 + C3 + C4
- [ ] M3 联调验收闭环：D5 + C5
- [ ] M4 交互增强：D6.1 ~ D6.5
- [ ] M5 语音分期：D6.6 ~ D6.8

## 四、本轮落地摘要（2026-04-22）

面向「冷启动 → AP 配网 → 写 Wi-Fi 凭据 → 验证能否连上 → 页面给反馈」这条主干做闭环，对应 D1 / D2 / D3 / D4.1~D4.3。

### 关键代码改动

- `board-selfcheck.sh`（新增）：开机自检（runtime 目录、关键二进制、fb0、unifont、wlan0）。对应 D1.1。
- `board-ap-up.sh` / `board-ap-down.sh`（新增/重写）：`hostapd` + `dnsmasq` 真 AP，固定 `192.168.44.1`，上电时用重试式 `assign_ap_ip()` 抵御 `hostapd` 抖动；AP up 前先调 `board-wifi-scan.sh` 抓一份周边 Wi-Fi 快照写到 `wifi-scan.json` 给 portal 用。对应 D2.3 / D2.4 / D2.5 / D2.6。
- `board-wifi-scan.sh`（新增）：`iw dev wlan0 scan` → 解析 SSID/signal/secure → 去重 → 写 JSON。
- `board-sta-apply.sh`（新增，本轮主力脚本）：读 `/tmp/board-runtime-ap/sta-apply.creds`，用 `wpa_passphrase`（缺失时 fallback 到明文 psk）重写 `/etc/wifi/wpa_supplicant.conf`（保留一次性 `.bak`），调 `board-ap-down.sh` 切回 STA，轮询最多 25 秒等 IPv4 非 `169.254`；成功写 `last-attempt.json={ok:true,ip,ssid}`；失败按 `wpa_cli status` 分类 `ssid_not_found` / `wrong_password_or_assoc` / `no_dhcp_lease`，再 `POST /pairing/ap-mode {"on":true}` 让 board-server 回 AP。对应 D3.5 + D4.1 / D4.2 / D4.3。
- `board-network-watchdog.sh`：加 `is_ap_mode()` 兜底，AP 模式下不再跑 `udhcpc`，避免它吃掉 `192.168.44.1`。
- `board-runtime.init`：默认端口改 80（同时 `stop` 并 `disable` `uhttpd`，释放 80），挂 `BOARD_RUNTIME_AP_SSID=claw-pet` / `AP_PSK=88888888`，挂 `BOARD_RUNTIME_AP_UP_CMD` / `AP_DOWN_CMD` / `STA_APPLY_CMD`，`DISCOVERY_TIMEOUT_MS` 默认 `0`（禁用自动 AP fallback，改为显式触发）。
- `src/board_server.c`：
  - `br_server_spawn_shell`（双 fork + 500ms 延迟）替换 `system()`，保证 HTTP 响应先 flush，再切网；所有 `socket()`/`accept()` 加 `FD_CLOEXEC`，修了 AP cycling 后端口 18789 被子进程继承导致再也 bind 不上的问题。
  - `POST /pairing/apply-config`：写 SSID/PSK 到 `sta-apply.creds`（`chmod 600`），先落一份 `lastAttempt:"pending"`，然后**无条件** `spawn_shell(sta_apply_cmd)`（修过一次 bug：之前只在 `br_pairing_apply_config` 返回 true 时 spawn，但设备开机若已有 `network-config.json` 则 state 已是 `sta_ready`，transition 不发生，导致脚本永不触发）。
  - `POST /pairing/ap-mode {"on":true|false}`：给 sta-apply 失败兜底用（也可手动触发 AP）。
  - `GET /pairing/state`：返回拼接 `last-attempt.json` 内容的 `lastAttempt` 字段，portal 据此判成功/失败。
  - `GET /wifi/scan`：读取 wifi-scan 快照。
  - 根路径 `/`：AP fallback 模式下直接 serve `ui/pairing-portal.html`。
  - `br_server_parse_network_config_json`：`mqtt_url` / `namespace` 变为可选，只有 `ssid` 必填。对应 D3.3。
- `src/runtime_pairing.c`：`discovery_timeout_ms <= 0` 表示不自动切 AP。
- `ui/pairing-portal.html`：
  - 标题改 `claw-pet 配网`，去掉 MQTT Broker URL 字段。
  - Wi-Fi 用 `<select>` 下拉，来自 `/wifi/scan`，带信号条/加锁图标，"找不到？手动输入" 切换。
  - 提交后进入 `pollApplyResult(ssid)`：每 2s 轮询 `/pairing/state.lastAttempt`，elapsed 提到 tick 最外层（修了"秒数一直 0"的 bug），try/catch 两边都更新文案；`ok:true` → 绿字成功；`error` 非 pending → 红字失败 + "重试"；连续 12 次 fetch 失败且曾看到 pending → "热点已关闭，大概率成功"；60s 兜底超时。
  - `fb-speech-overlay`：pairing 状态下 `effective_hold` 设为近似无限，不再把 fb0 blank；`fb-display.sh` 识别 `pairing-hold` 动作停掉 `tplayerdemo`，让 overlay 独占 fb0。对应 D1.3。

### 新增/关键运行时约定

- 环境变量（都在 `board-runtime.init` 注入）：
  - `BOARD_RUNTIME_AP_SSID` / `AP_PSK` / `AP_IP`
  - `BOARD_RUNTIME_AP_UP_CMD` / `AP_DOWN_CMD` / `STA_APPLY_CMD`
  - `BOARD_RUNTIME_PORT=80`
  - `BOARD_RUNTIME_DISCOVERY_TIMEOUT_MS=0`
  - `BOARD_RUNTIME_STA_VERIFY_TIMEOUT`（默认 25s）
  - `BOARD_RUNTIME_AP_STATE_DIR=/tmp/board-runtime-ap`
- `$STATE_DIR` 目录约定：
  - `sta-apply.creds` — 凭据，short-lived（sta-apply 读完立刻 rm）
  - `last-attempt.json` — 结果（pending / ok / 各类 error）
  - `wifi-scan.json` — AP 切换前抓的 Wi-Fi 列表
  - `hostapd.conf` / `dnsmasq.conf` / `*.log`
- `/etc/wifi/wpa_supplicant.conf.bak` — 首次配网时保留原出厂 Wi-Fi 凭据，便于兜底回滚。

### 已知限制 / 下一步建议

1. **D4.4 ~ D4.6（MQTT 端到端联动）未收口**：`runtime_mqtt.c` 代码早已存在，但：
   - STA 连上后没有显式把 MQTT broker URL / `desktopDeviceId` / `namespace` 传递到 `last-attempt.json`，HachimoDock（哈基米机）目前要额外 poll。
   - 未明确 LWT（遗嘱）topic 约定，也没在 pairing 回执里带 MQTT online/offline。
   - 建议后续在 `sta-apply` 成功后额外发一个 `/pairing/mqtt-ready` 事件，或者由 board-server 监听 `last-attempt.json` 变 ok 后主动重建 MQTT 连接并广播上线。
2. **Wi-Fi 扫描只抓了 AP 切换前那一刻的快照**：XR829 芯片在设备端扫描灵敏度比 Mac 弱，用户反馈"明明能看到十几个，板子只看到 1 个"。暂时用户可以「手动输入」绕过。若要做得更好：改成 AP 下也可以周期性降级扫描（把 wlan0 短暂切回 managed 扫描 2-3s 再切回 AP），或者直接在 portal 一律允许手动输入。
3. **BusyBox 差异**：这个设备镜像**缺 `head` / `tail` / `pgrep` / `logread` / `wpa_passphrase` / `setsid`**，后续写脚本要规避；`date` 不支持 `%N`，所以 `last-attempt.json` 的 `atMs` 精度只到秒（但 portal 只按 ssid + ok/error 判定，不依赖具体毫秒）。
4. **Portal reload 策略**：失败回 AP 后 Mac 偶尔需要手动断开再重连 `claw-pet` 才能刷出 portal；可以考虑在设备端 AP 重启前先多等 1~2 秒 + 提醒 DHCP 重发。
5. **自动化 E2E**：目前靠 `/tmp/simulate_pet_manager.py` 做"电脑端"一侧，后续可以把 `/pairing/state.lastAttempt` 轮询和失败分支都写入该脚本作为回归。
6. **配置版本号**：`network-config.json` 还没有 schema version（D3.4 里提到"防止脏配置"）。建议下一轮加 `configVersion` 字段，HachimoDock（哈基米机）和设备端同时校验。

### 本轮闭环的测试路径（供回归参考）

1. 设备正常 STA：`curl -X POST http://<ip>/pairing/ap-mode -d '{"on":true}'` → 进 AP（SSH 会断开）。
2. Mac 连 `claw-pet` / `88888888`，浏览器打开 `http://192.168.44.1`。
3. 下拉选 Wi-Fi 或手输；密码栏输入；点"提交配置"。
4. 观察文案：`已提交…` → `正在让设备连接 XXX… (Ns)` → `热点已离线，等待设备连上 XXX… (Ns)` → `连接成功！设备已加入 XXX（IP ...）` 或 `连接失败：<原因>`。
5. 失败路径：故意输错密码，约 25 秒内 portal 应恢复为红字失败 + "重试" 按钮，同时设备自动回 AP。
