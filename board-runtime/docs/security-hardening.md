# board-runtime 安全加固说明（2026-06）

本文档记录本轮已落地的安全修复、运行配置要求和部署验收流程。目标是让设备端在
不影响 AP 物理配网体验的前提下，阻断远程未授权管理请求和凭证泄露。

## 1. 已修复问题

### 1.1 MQTT 凭证明文泄露（源码/文档）

- 问题：历史代码和调试文档中存在硬编码 MQTT 密码。
- 修复：
  - 运行时改为环境变量注入，不再写死密码。
  - 文档示例改为占位符（`$MQTT_PASSWORD`）。

当前读取顺序：

- 用户名：`PET_CLAW_MQTT_USERNAME` -> `MQTT_USERNAME` -> 默认 `device`
- 密码：`PET_CLAW_MQTT_PASSWORD` -> `MQTT_PASSWORD` -> 默认空串

### 1.2 敏感配网接口未认证（逻辑漏洞）

- 问题：`/pairing/*` 管理接口可被同网段未授权请求调用。
- 修复：新增 token 鉴权，策略如下。

鉴权策略：

- AP 配网模式（`pairingState=ap_fallback`）免认证。
- STA 模式下敏感接口必须认证。

受保护接口（STA 模式）：

- `POST /pairing/apply-config`
- `POST /pairing/reset`
- `POST /pairing/ap-mode`

支持的认证头：

- `X-Board-Token: <token>`
- `Authorization: Bearer <token>`

错误语义：

- 未配置 `BOARD_RUNTIME_ADMIN_TOKEN`：`503 {"ok":false,"error":"admin_token_not_configured"}`
- 缺 token 或 token 不匹配：`401 {"ok":false,"error":"unauthorized"}`

### 1.3 生成产物可能携带签名 URL（TOS AK 暴露风险）

- 问题：生成类 JSON 可能包含签名 URL（如 `X-Tos-Credential`）。
- 修复：新增忽略规则，阻止 `generated-anchor-images` 目录进入版本库。

相关规则见仓库根 `.gitignore`：

- `pet-claw/src/assets/pets/**/generated-anchor-images/`
- `**/generated-anchor-images/`

## 2. 必需配置项

设备端建议通过 systemd drop-in 持久化以下变量（不要把真实 secret 提交到仓库）：

| 变量 | 用途 |
|---|---|
| `PET_CLAW_MQTT_USERNAME` | MQTT 用户名 |
| `PET_CLAW_MQTT_PASSWORD` | MQTT 密码 |
| `BOARD_RUNTIME_ADMIN_TOKEN` | STA 模式敏感接口鉴权 token |

## 3. 部署（推荐流程）

### 3.1 一次性写入安全环境变量（每台设备一次）

```sh
export BOARD_HOST="<board-user>@<board-ip>"
export MQTT_USERNAME="device"
export MQTT_PASSWORD="<MQTT_PASSWORD>"
export BOARD_ADMIN_TOKEN="<RANDOM_LONG_TOKEN>"

ssh "$BOARD_HOST" "sudo install -d -m 0755 /etc/systemd/system/board-runtime.service.d"
ssh "$BOARD_HOST" "printf '%s\n' \
'[Service]' \
'Environment=PET_CLAW_MQTT_USERNAME=$MQTT_USERNAME' \
'Environment=PET_CLAW_MQTT_PASSWORD=$MQTT_PASSWORD' \
'Environment=BOARD_RUNTIME_ADMIN_TOKEN=$BOARD_ADMIN_TOKEN' \
| sudo tee /etc/systemd/system/board-runtime.service.d/10-security-env.conf >/dev/null"
ssh "$BOARD_HOST" "sudo systemctl daemon-reload && sudo systemctl restart board-runtime"
```

### 3.2 常规版本部署（可反复执行）

```sh
cd board-runtime
export BOARD_HOST="<pi-user>@<pi-ip>"
HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh
```

Radxa Cubie A7Z 使用 shell 或 PowerShell 部署脚本；需要配置 SPI 小屏时加
`CONFIGURE_SPI_LCD=1` 或 `-ConfigureSpiLcd`。macOS/Linux/WSL/Git Bash：

```sh
cd board-runtime
HOST=radxa@<board-ip> SUDO_PASSWORD=<sudo-password> CONFIGURE_SPI_LCD=1 sh scripts/deploy-radxa-a733.sh
```

Windows PowerShell：

```powershell
cd board-runtime
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -ConfigureSpiLcd
```

macOS/Linux PowerShell 7 也可运行 PowerShell 版本：

```sh
cd board-runtime
pwsh -NoProfile -ExecutionPolicy Bypass -File ./scripts/deploy-radxa-a733.ps1 \
  -HostName radxa@<board-ip> \
  -SudoPassword <sudo-password> \
  -ConfigureSpiLcd
```

说明：`deploy-rpi.sh`、`deploy-radxa-a733.sh` 和 `deploy-radxa-a733.ps1` 都会刷新
`/opt/board-runtime/*` 文件。密钥建议放在 systemd drop-in
（`/etc/systemd/system/board-runtime.service.d/*.conf`），避免被覆盖。

## 4. 安全验收（回归检查）

```sh
export BOARD_IP="<board-ip>"
export BOARD_ADMIN_TOKEN="<RANDOM_LONG_TOKEN>"

# STA 模式下：无 token 应失败（401 或 503）
curl -i -X POST "http://$BOARD_IP/pairing/reset"

# STA 模式下：带 token 应成功（200）
curl -i -X POST "http://$BOARD_IP/pairing/reset" \
  -H "X-Board-Token: $BOARD_ADMIN_TOKEN"

# AP 模式下：允许无 token 配网（物理接触前提）
curl -i -X POST "http://$BOARD_IP/pairing/apply-config" \
  -H "Content-Type: application/json" \
  -d '{"ssid":"<ssid>","password":"<psk>"}'
```

## 5. 给 AI 的自动部署执行模板

建议让 AI 按以下顺序执行，做到“部署 + 配置 + 验收”一次完成：

1. 按板型运行 `board-runtime/scripts/deploy-rpi.sh` 或
   `board-runtime/scripts/deploy-radxa-a733.sh` / `deploy-radxa-a733.ps1` 部署代码。
2. 通过 SSH 写入/刷新 `10-security-env.conf`（三项环境变量）。
3. `systemctl daemon-reload && systemctl restart board-runtime`。
4. 运行第 4 节回归命令并输出结果摘要。

给 AI 的示例指令：

```text
按 board-runtime/docs/security-hardening.md 执行安全部署到 BOARD_HOST，
自动刷新 systemd drop-in 的 MQTT 用户名/密码和 BOARD_RUNTIME_ADMIN_TOKEN，
重启服务后完成敏感接口鉴权回归检查，并输出检查结果。
```

## 6. 运维建议

- 已泄露过的 AK/SK 或 MQTT 凭证请立即轮换，不要只依赖“已过期”。
- `BOARD_RUNTIME_ADMIN_TOKEN` 建议使用高熵随机串（长度 >= 32）。
- 生产环境建议收敛管理口访问范围（例如仅可信网段可达）。
