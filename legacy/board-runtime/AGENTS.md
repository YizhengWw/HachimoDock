# Codex — board-runtime

本文是设备端目录的工作说明。当前首次复刻部署默认目标是 Radxa Cubie A7Z，
目标是把项目画面首次显示到 A7Z 小屏硬件上；Raspberry Pi 是兼容路线。

## 硬件平台

- **默认设备**: Radxa Cubie A7Z（arm64）
- **兼容设备**: Raspberry Pi（当前实测为 `zero2w`，armv7l）
- **系统**: Raspberry Pi OS 或 Radxa Debian / systemd
- **运行目录**: `/opt/board-runtime`
- **源码同步目录**: `/opt/board-runtime-src`
- **屏幕**: ILI9341 320x240 SPI framebuffer；Pi 通常是 `/dev/fb1`，Radxa 通常是 `/dev/fb0`
- **A7Z 首次显示硬件**: 新 microSD 卡 + 读卡器用于写系统卡；上板激活至少需要
  A7Z 开发板、PCB/PSB 转接板、SPI 屏幕和稳定 USB-C 数据/供电线连接好
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

`scripts/deploy-rpi.sh` 会同步源码到 `/opt/board-runtime-src`，在 Pi 上安装依赖、
运行 CMake/make，复制产物到 `/opt/board-runtime`，安装 systemd unit，并重启服务。
如果需要传入 sudo 密码，使用 `SUDO_PASSWORD="<password>"`；脚本会用 askpass 认证，
避免把密码写进通过 stdin 生成的远端配置文件（例如 `/etc/asound.conf`）。

Radxa Cubie A7Z 上完整构建可用 shell 或 PowerShell 部署脚本完成。
macOS/Linux/WSL/Git Bash 使用 shell 脚本：

```sh
MQTT_URL="mqtt://<pc-lan-ip>:1883" \
HOST=radxa@<board-ip> \
SUDO_PASSWORD=<sudo-password> \
CONFIGURE_SPI_LCD=1 \
sh scripts/deploy-radxa-a733.sh
```

Windows 使用 `powershell`：

如果 A7Z 还没有系统，先按官方文档刷写 microSD：下载页
<https://docs.radxa.com/en/cubie/a7z/download>，microSD 安装说明
<https://docs.radxa.com/en/cubie/a7z/getting-started/install-system/microsd>。
优先使用官方 GPT/A733 unified release 镜像，下载后解压 `.img` 并用 Balena Etcher
写入 microSD。
首次启动 A7Z 后必须先在 Debian 系统层写入 Wi-Fi 信息，确认 `ssh radxa@<board-ip>`
可用后再运行部署脚本；AI 执行部署教程时应在这一步暂停并要求用户提供 `<board-ip>`。
完整新手部署路径、MQTT/binding 验收和常见卡点以 `DEPLOY.md` 为准。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -MqttUrl mqtt://<pc-lan-ip>:1883 `
  -ConfigureSpiLcd
```

macOS/Linux 也可使用 PowerShell 7 的 `pwsh`：

```sh
pwsh -NoProfile -ExecutionPolicy Bypass -File ./scripts/deploy-radxa-a733.ps1 \
  -HostName radxa@<board-ip> \
  -SudoPassword <sudo-password> \
  -MqttUrl mqtt://<pc-lan-ip>:1883 \
  -ConfigureSpiLcd
```

Radxa 的 `-ConfigureSpiLcd` 会写入 A7Z 专用 ILI9341 overlay：
SPI1 CS0、`RES=PL6` active-low、`DC=PL7`、默认不控制 BLK GPIO。
不要把 A7Z physical pin 26 当 Raspberry Pi `CE1` 使用，它是 `PD14/SPI1-HOLD`。

## 部署和验证

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
export BOARD_IP="<pi-ip>"

# 完整部署
HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh

# Radxa A7Z 完整部署
MQTT_URL="mqtt://<pc-lan-ip>:1883" \
HOST=radxa@<board-ip> \
SUDO_PASSWORD=<sudo-password> \
CONFIGURE_SPI_LCD=1 \
sh scripts/deploy-radxa-a733.sh

# Radxa A7Z on Windows PowerShell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -MqttUrl mqtt://<pc-lan-ip>:1883 `
  -ConfigureSpiLcd

# Radxa A7Z on macOS/Linux
pwsh -NoProfile -ExecutionPolicy Bypass -File ./scripts/deploy-radxa-a733.ps1 \
  -HostName radxa@<board-ip> \
  -SudoPassword <sudo-password> \
  -MqttUrl mqtt://<pc-lan-ip>:1883 \
  -ConfigureSpiLcd

# 服务状态
ssh "$BOARD_HOST" 'systemctl is-active board-runtime'
ssh "$BOARD_HOST" 'sudo systemctl --no-pager status board-runtime'

# 日志
ssh "$BOARD_HOST" 'sudo journalctl -u board-runtime -n 80 --no-pager'

# HTTP 验证
curl -fsS http://$BOARD_IP/board-runtime-config.json
curl -fsSI http://$BOARD_IP/

# widget runtime 单独重启
ssh "$BOARD_HOST" 'sudo systemctl restart board-widget-runtime.service'
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
| `fb-speech-overlay` | 32bpp framebuffer 负一屏/debug overlay；主屏字幕默认不渲染，Pi 的 16bpp 小屏通常跳过 |

模块之间优先通过 `/opt/board-runtime` 下的点文件通信，例如
`.current-state`、`.current-speech`、`.screen-page`、`.stats-display`、
`.button-config`、`.widget-events`。`.current-speech` 保留为语音文本/配网提示点文件，
主屏默认不把它渲染成字幕。不要让输入进程直接控制播放器，也不要让
`fb-display.sh` 直接订阅 MQTT。

## 常用设备操作

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"

# 重启主服务
ssh "$BOARD_HOST" 'sudo systemctl restart board-runtime'

# 看进程
ssh "$BOARD_HOST" 'ps -eo pid,comm,args | grep -E "board-|fb-|widget" | grep -v grep'

# 查看运行目录
ssh "$BOARD_HOST" 'ls -la /opt/board-runtime | sed -n "1,80p"'

# 恢复 AP 配网模式
ssh "$BOARD_HOST" 'sudo rm -f /opt/board-runtime/network-config.json && sudo systemctl restart board-runtime'
```

## 文件不要覆盖

以下文件是设备现场状态，部署或手动拷贝时不要随意覆盖：

- `device-config.json` — 设备身份。
- `network-config.json` — Wi-Fi、MQTT 和桌面端绑定。
- `stats/` — token 统计数据。
