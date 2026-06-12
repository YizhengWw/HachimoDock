# board runtime

`board-runtime` 是 [HachimoDock（哈基米机）](https://github.com/YizhengWw/HachimoDock) 的设备端运行时。当前主线覆盖
Raspberry Pi（已在 Raspberry Pi Zero 2 W + Raspberry Pi OS 上使用）和
Radxa Cubie A7Z（Debian 11/12 系列镜像）。它负责把桌面端的
agent/session 状态、语音字幕、按钮/触屏输入和负一屏组件展示到 SPI 小屏上；
不同硬件方案默认启用的能力不同。

## 硬件方案支持矩阵

| 方案 | 定位 | 当前已验证 | 当前未默认启用 |
|---|---|---|---|
| 方案一：Radxa Cubie A7Z | 默认复刻硬件，性能更高，推荐走 Wi-Fi + MQTT/SSH | Debian 11/12、ILI9341 SPI LCD、`/dev/fb0` framebuffer 显示、HTTP/MQTT、桌面端状态同步、负一屏 widget | XPT2046/PEN 触摸 overlay、GPIO 旋钮/按钮、板端语音 PTT、USB gadget `/dev/ttyGS0` |
| 方案二：Raspberry Pi Zero 2 W | 兼容方案，适合完整体验 USB 直连、触摸、旋钮和语音 | Raspberry Pi OS、ILI9341 SPI LCD、XPT2046/ADS7846 触摸、GPIO 旋钮/按钮、VoiceHAT 语音、USB gadget、HTTP/MQTT、桌面端状态同步、负一屏 widget | 具体音频设备名和网络质量取决于镜像、声卡与现场配置 |

`ESP32` 不是当前 `board-runtime/` 的已支持部署目标；如需使用，需要单独做运行时移植。

运行时主体用 C 编写，配套少量 shell / Python。下面列的是仓库内模块，不代表每个硬件方案都会默认启动：

- C：`board-server`、触屏输入、旋钮/按键输入、framebuffer overlay。
- shell：启动编排、framebuffer 视频播放、Wi-Fi/AP/USB gadget 辅助脚本。
- Python：负一屏 widget runtime、语音 PTT。

## 快速构建

本机开发验证可以直接用 CMake 构建 host 版本：

```sh
cd board-runtime
cmake -S . -B /tmp/board-runtime-build-check
cmake --build /tmp/board-runtime-build-check --target board-server
```

在 Raspberry Pi 上完整构建和部署，使用 shell 部署脚本远端同步源码、安装依赖、
编译并重启服务。macOS/Linux 可直接运行；Windows 请在 WSL 或 Git Bash 中运行：

```sh
cd board-runtime
export BOARD_HOST="<pi-user>@<pi-ip>"
HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh
```

在 Radxa Cubie A7Z 上部署使用 `deploy-radxa-a733.sh` 或 PowerShell 脚本；可选的
LCD 配置会写入已验证的 ILI9341 SPI 屏 overlay。当前 A7Z 启动编排默认只启用
`board-server`、显示链路和 widget runtime，不启动触摸、旋钮和语音进程。

刷写系统前先参考官方资料：

- [Radxa Cubie A7Z Downloads](https://docs.radxa.com/en/cubie/a7z/download)
- [Install System to microSD Card](https://docs.radxa.com/en/cubie/a7z/getting-started/install-system/microsd)

推荐使用官方 GPT/A733 unified release 镜像，下载后解压 `.img`，再用 Balena Etcher
写入 microSD 卡。

macOS/Linux/WSL/Git Bash：

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

默认远端源码目录是 `/opt/board-runtime-src`，运行目录是 `/opt/board-runtime`。
Raspberry Pi 可通过环境变量覆盖：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
HOST="$BOARD_HOST" REMOTE_DIR=/opt/board-runtime SRC_DIR=/opt/board-runtime-src sh scripts/deploy-rpi.sh
```

Radxa Cubie A7Z 可通过 PowerShell 参数覆盖：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -RemoteDir /opt/board-runtime `
  -SrcDir /opt/board-runtime-src `
  -ConfigureSpiLcd
```

shell 脚本使用环境变量覆盖：

```sh
HOST=radxa@<board-ip> \
SUDO_PASSWORD=<sudo-password> \
REMOTE_DIR=/opt/board-runtime \
SRC_DIR=/opt/board-runtime-src \
CONFIGURE_SPI_LCD=1 \
sh scripts/deploy-radxa-a733.sh
```

## 设备运行

设备端通过 systemd 管理：

```sh
export BOARD_HOST="<board-user>@<board-ip>"
ssh "$BOARD_HOST" 'sudo systemctl status board-runtime'
ssh "$BOARD_HOST" 'sudo journalctl -u board-runtime -f'
ssh "$BOARD_HOST" 'sudo systemctl restart board-runtime'
```

本地 HTTP 服务默认监听 80 端口：

```text
http://<board-ip>/
http://<board-ip>/board-runtime-config.json
http://<board-ip>/debug/state
```

当前默认 MQTT broker 是 `mqtt://broker.openclaw.example:1883`，namespace 是 `desk`。
在设备上通常由 `/opt/board-runtime/board-runtime-rpi.env`、
`/opt/board-runtime/board-runtime-radxa.env` 和
`network-config.json` 提供运行配置。

## 大体框架

设备端是多进程架构，模块之间主要通过 `/opt/board-runtime` 下的点文件通信：

| 模块 | 作用 |
|---|---|
| `board-server` | HTTP/WebSocket/MQTT/USB serial，配网，设备状态机，统计和组件数据落盘 |
| `fb-display.sh` | 根据 `.current-state` 播放宠物动画，并用 ffmpeg 写 framebuffer |
| `board-touch-input` | 读取 `/dev/input` 触屏事件，写本地事件并上报输入 action |
| `board-rotary-input` | 读取 GPIO 旋钮和按钮，切换页面、触发语音 PTT 或 widget 事件 |
| `board-widget-runtime.py` | 解释 `.clawpkg` widget，生成负一屏展示 payload |
| `board-voice-ptt.py` | 顶部按钮按住说话，转成 `/input/action` |
| `fb-speech-overlay` | 32bpp framebuffer 上的字幕/负一屏 overlay；Pi 的 16bpp 小屏会跳过该进程 |

核心文件契约：

- `.current-state`：当前宠物状态，如 `idle`、`working`、`done`。
- `.current-speech`：主屏字幕。
- `.screen-page`：`main` 或 `stats`。
- `.stats-display`：负一屏统计或组件 dashboard payload。
- `.button-config`：桌面端下发的按钮功能配置。
- `.widget-events`：负一屏 widget 输入事件队列。

更完整的架构说明见 [docs/device-runtime-design.md](docs/device-runtime-design.md)。
安全加固和鉴权/凭证部署说明见 [docs/security-hardening.md](docs/security-hardening.md)。

## 配网和通信

设备支持两条通信路径，但硬件方案默认路径不同：

- USB direct-connect：`board-server` 集成 `/dev/ttyGS0` USB serial 行协议；当前主要用于 Raspberry Pi USB gadget 方案。
- MQTT：设备订阅 `desk/<targetDeviceId>/state/+` 和 `speech/text`，并发布
  `claw-pet/board/<boardDeviceId>/hello`、`availability`、`input/action`。
  当前 Radxa Cubie A7Z 默认走 Wi-Fi + MQTT/SSH。

配网状态和本地验证接口：

- `GET /pairing/state`
- `POST /pairing/apply-config`
- `POST /pairing/reset`
- `POST /input/action`

说明：`/pairing/apply-config`、`/pairing/reset`、`/pairing/ap-mode` 在 AP 配网模式
（`pairingState=ap_fallback`）下免认证；在 STA 模式下需要携带管理 token
（`X-Board-Token` 或 `Authorization: Bearer <token>`，token 由
`BOARD_RUNTIME_ADMIN_TOKEN` 配置）。

当没有有效网络配置时，设备可进入 AP 配网模式，默认 AP 为
`claw-pet` / `88888888`，配网页面为 `http://192.168.44.1/`。

## 自检

本机 host 构建后可跑：

```sh
/tmp/board-runtime-build-check/board-server --self-check .
/tmp/board-runtime-build-check/board-server --self-check-json .
```

设备端可跑：

```sh
export BOARD_HOST="<board-user>@<board-ip>"
ssh "$BOARD_HOST" 'cd /opt/board-runtime && ./board-server --self-check .'
ssh "$BOARD_HOST" 'curl -fsS http://127.0.0.1/board-runtime-config.json'
```

## 不要覆盖的设备文件

这些文件是设备现场状态，部署脚本应保留：

- `device-config.json`：设备身份。
- `network-config.json`：Wi-Fi、MQTT 和桌面端绑定。
- `stats/`：本地统计数据。
