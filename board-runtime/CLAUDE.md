# Claude Code — board-runtime

本文是设备端目录的工作说明。当前设备端主线支持 Raspberry Pi 和 Radxa Cubie A7Z。

## 硬件平台

- **设备**: Raspberry Pi（当前实测为 `zero2w`，armv7l）；Radxa Cubie A7Z（arm64）
- **系统**: Raspberry Pi OS 或 Radxa Debian / systemd
- **运行目录**: `/opt/board-runtime`
- **源码同步目录**: `/opt/board-runtime-src`
- **屏幕**: ILI9341 320x240 SPI framebuffer；Pi 通常是 `/dev/fb1`，Radxa 通常是 `/dev/fb0`
- **触屏**: XPT2046 / ADS7846，通过 `/dev/input/event*`；Radxa 触屏配置尚未作为默认部署启用
- **输入**: GPIO 旋钮、顶部按钮、触屏
- **服务管理**: `board-runtime.service`、`board-widget-runtime.service`

## 构建

本机 host 编译用于快速检查 C 代码：

```sh
cmake -S . -B /tmp/board-runtime-build-check
cmake --build /tmp/board-runtime-build-check --target board-server
```

Raspberry Pi 上完整构建由部署脚本完成：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh
```

Radxa Cubie A7Z 上完整构建可用 shell 或 PowerShell 部署脚本完成；
`CONFIGURE_SPI_LCD=1` / `-ConfigureSpiLcd` 会写入 A7Z 专用 ILI9341 SPI 屏
overlay。macOS/Linux/WSL/Git Bash 使用 shell 脚本：

```sh
HOST=radxa@<board-ip> SUDO_PASSWORD=<sudo-password> CONFIGURE_SPI_LCD=1 sh scripts/deploy-radxa-a733.sh
```

Windows 使用 `powershell`：

如果 A7Z 还没有系统，先按官方文档刷写 microSD：下载页
<https://docs.radxa.com/en/cubie/a7z/download>，microSD 安装说明
<https://docs.radxa.com/en/cubie/a7z/getting-started/install-system/microsd>。
优先使用官方 GPT/A733 unified release 镜像，下载后解压 `.img` 并用 Balena Etcher
写入 microSD。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -ConfigureSpiLcd
```

macOS/Linux 也可使用 PowerShell 7 的 `pwsh`：

```sh
pwsh -NoProfile -ExecutionPolicy Bypass -File ./scripts/deploy-radxa-a733.ps1 \
  -HostName radxa@<board-ip> \
  -SudoPassword <sudo-password> \
  -ConfigureSpiLcd
```

Radxa LCD 默认使用 SPI1 CS0、`RES=PL6` active-low、`DC=PL7`、BLK 直连
3.3V/不由 GPIO 控制。不要把 A7Z physical pin 26 当 Raspberry Pi `CE1` 使用，
它是 `PD14/SPI1-HOLD`。

## 部署和验证

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
export BOARD_IP="<pi-ip>"

# 完整部署
HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh

# Radxa A7Z 完整部署
HOST=radxa@<board-ip> SUDO_PASSWORD=<sudo-password> CONFIGURE_SPI_LCD=1 sh scripts/deploy-radxa-a733.sh

# Radxa A7Z on Windows PowerShell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -ConfigureSpiLcd

# Radxa A7Z on macOS/Linux
pwsh -NoProfile -ExecutionPolicy Bypass -File ./scripts/deploy-radxa-a733.ps1 \
  -HostName radxa@<board-ip> \
  -SudoPassword <sudo-password> \
  -ConfigureSpiLcd

# 服务状态和日志
ssh "$BOARD_HOST" 'systemctl is-active board-runtime'
ssh "$BOARD_HOST" 'sudo journalctl -u board-runtime -n 80 --no-pager'

# HTTP 验证
curl -fsS http://$BOARD_IP/board-runtime-config.json
curl -fsSI http://$BOARD_IP/
```

## 大体框架

| 模块 | 责任 |
|---|---|
| `board-server` | HTTP/WebSocket/MQTT/USB serial、配网、状态机、统计、控制命令 |
| `fb-display.sh` | 播放宠物动画；Pi 上用 ffmpeg 输出 rawvideo 到 framebuffer |
| `fb-rawvideo-blit.py` | 把 ffmpeg rawvideo 写入 `/dev/fb*` |
| `board-touch-input` | 触屏 tap/long press/swipe 输入 |
| `board-rotary-input` | GPIO 旋钮和按钮输入 |
| `board-widget-runtime.py` | `.clawpkg` widget 解释和负一屏 payload |
| `board-voice-ptt.py` | 顶部按钮语音输入 |
| `fb-speech-overlay` | 32bpp framebuffer overlay；Pi 的 16bpp 小屏通常跳过 |

模块之间优先通过 `/opt/board-runtime` 下的点文件通信，例如
`.current-state`、`.current-speech`、`.screen-page`、`.stats-display`、
`.button-config`、`.widget-events`。

## USB virtual topic 一览

USB direct-connect 模式下，桌面端通过 USB serial 发
`{"topic":"...","payload":{...}}\n` 的行协议；设备端 `br_handle_usb_message`
在 `src/board_server.c` 中分派。

| Topic | Direction | 用途 |
|---|---|---|
| `state/<source>` | host -> board | 转发桌面端状态，等价于 MQTT `<ns>/<deviceId>/state/<source>` |
| `speech/text` | host -> board | 转发桌面端说话 |
| `control/command` | host -> board | 控制命令 |
| `control/remote-cli-binding` | host -> board | 远程 CLI 绑定 |
| `control/screen-page` | host -> board | 切板端屏幕页面 |
| `control/apply-wifi` | host -> board | 以 `{ssid, password}` 切换 Wi-Fi；调用 `board-sta-apply.sh`，最长约 25s；分阶段通过 `apply-wifi-ack` 回报 |
| `asset/begin/chunk/file/commit` | host -> board | 视频/图片/WAV 资产 OTA；`asset/file` 逐文件校验 size/checksum 并回 `asset/ack`，最终 `asset/commit` 校验文件数和总字节后才激活 |
| `widget/begin/chunk/commit` | host -> board | Widget OTA |
| `widget-install-ack` | board -> host | Widget OTA 阶段确认，payload `{transferId, phase:"begin"|"commit", ok, msg}`；chunk 成功时不回 ack，只在失败时回。客户端注册 waiter 等 begin/commit ack；早期实现用裸 `{"type":"widget_install_ack"}` 会被客户端 SerialMessage 解析丢弃，从 2026-06-01 起统一走 topic 包装。 |
| `apply-wifi-ack` | board -> host | Wi-Fi 切换状态，payload `{ok, stage:"applying"|"connected"|"failed", ip, error}` |
| `hello`, `ack`, `availability` | bidirectional | 握手与可达性 |

## 文件不要覆盖

以下文件是设备现场状态，部署或手动拷贝时不要随意覆盖：

- `device-config.json` — 设备身份。
- `network-config.json` — Wi-Fi、MQTT 和桌面端绑定。
- `stats/` — token 统计数据。
