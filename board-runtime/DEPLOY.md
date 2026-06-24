# Board Runtime 设备部署说明

当前设备端主线支持 Raspberry Pi + Raspberry Pi OS + systemd，以及
Radxa Cubie A7Z + Debian + systemd。

## 前置条件

- 本机能 SSH 到目标板，例如 `<pi-user>@<pi-ip>` 或 `radxa@<board-ip>`。
- 目标用户具备 sudo 权限。
- 目标板能访问 apt 源，用于安装 `cmake`、`gcc`、`ffmpeg`、字体和 Python 依赖。
- 本地在 `board-runtime/` 目录执行命令。
- macOS/Linux 可直接运行 shell 脚本；Windows 运行 shell 脚本建议使用 WSL 或 Git Bash。
- Radxa 既有 shell 部署脚本，也有 Windows PowerShell 部署脚本。

## Raspberry Pi 一键部署

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh
```

Windows 上部署 Raspberry Pi 时，建议在 WSL 或 Git Bash 中执行同一组命令。

## Radxa Cubie A7Z 一键部署

### 刷写系统

官方入口：

- [Radxa Cubie A7Z Downloads](https://docs.radxa.com/en/cubie/a7z/download)
- [Install System to microSD Card](https://docs.radxa.com/en/cubie/a7z/getting-started/install-system/microsd)
- [Quick Start / Booting the System](https://docs.radxa.com/en/cubie/a7z/getting-started/quickly-start)
- [Radxa Cubie A7Z image releases](https://github.com/radxa-build/radxa-cubie-a7z/releases)

推荐流程：

1. 从 Radxa Downloads 页面下载 A733/Cubie A7Z 官方 GPT 系统镜像。优先使用
   release 版本，不使用 test/pre-release 镜像，除非 Radxa 支持人员明确要求。
2. 下载后先解压镜像压缩包，得到 `.img` 文件。
3. 用 Balena Etcher 选择镜像文件和 microSD 卡，执行 `Flash!` 并等待校验完成。
4. 把 microSD 插入 A7Z，接 5V USB-C 电源启动。
5. 首次启动后确认 SSH 可用，例如 `ssh radxa@<board-ip>`。默认账号以镜像说明为准；
   当前实测 Debian 镜像常见为 `radxa` / `radxa`。

注意：Radxa 下载页里旧的 Cubie A7Z legacy 镜像不再更新；新部署优先走
`GPT System Image` / A733 unified image。CLI 旧镜像可用于无头调试，但如果官方
release 页只提供 Desktop/KDE 镜像，应优先使用官方当前支持的 release。

### 部署 runtime

macOS/Linux/WSL/Git Bash：

```sh
HOST=radxa@<board-ip> SUDO_PASSWORD=<sudo-password> CONFIGURE_SPI_LCD=1 sh scripts/deploy-radxa-a733.sh
```

Windows PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -ConfigureSpiLcd
```

## 安全部署（强烈建议）

自 2026-06 安全加固后，建议把 MQTT 凭证和管理接口 token 放到 systemd drop-in，
避免硬编码或被部署文件覆盖。详细背景见
[docs/security-hardening.md](docs/security-hardening.md)。

一次性初始化（每台设备执行一次）：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
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

之后常规部署仍然只需要：

Raspberry Pi：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh
```

Radxa Cubie A7Z：

macOS/Linux/WSL/Git Bash：

```sh
HOST=radxa@<board-ip> SUDO_PASSWORD=<sudo-password> CONFIGURE_SPI_LCD=1 sh scripts/deploy-radxa-a733.sh
```

Windows PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -ConfigureSpiLcd
```

部署脚本会执行完整流程：

Raspberry Pi：

1. `rsync` 当前源码到 `HOST:/opt/board-runtime-src`。
2. 在 Pi 上安装或补齐依赖。
3. 在 Pi 上运行 CMake/make 构建 native 二进制。
4. 停止 `board-runtime.service`。
5. 复制二进制、脚本、UI、assets、字体到 `/opt/board-runtime`。
6. 安装/刷新 systemd unit。
7. mask 会冲突的 `serial-getty@ttyGS0.service`。
8. 重启 `board-runtime.service`。
9. 写入 Raspberry Pi 的 SPI LCD / touch overlay 配置（首次写入后需要重启 Pi 才生效）。

Radxa Cubie A7Z：

1. 用 `tar` 打包当前源码并通过 `scp` 上传到设备。
2. 解压到 `/opt/board-runtime-src`。
3. 在 A7Z 上安装或补齐依赖。
4. 在 A7Z 上运行 CMake/make 构建 native arm64 二进制。
5. 停止 `board-runtime.service` 和 `board-widget-runtime.service`。
6. 复制二进制、脚本、UI、assets、字体到 `/opt/board-runtime`。
7. 安装/刷新 systemd unit 和 `/opt/board-runtime/start-radxa-a733.sh`。
8. 如果指定 `-ConfigureSpiLcd`，写入 A7Z 专用 SPI LCD overlay 并重启设备。
9. 如果未重启，则重启 `board-runtime.service` 和 `board-widget-runtime.service`。

可覆盖的环境变量：

Raspberry Pi shell 脚本：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `HOST` | `<pi-user>@<pi-ip>` | SSH 目标 |
| `SRC_DIR` | `/opt/board-runtime-src` | Pi 上源码构建目录 |
| `REMOTE_DIR` | `/opt/board-runtime` | Pi 上运行目录 |
| `BOARD_RUNTIME_ADMIN_TOKEN` | - | STA 模式下敏感管理接口 token（`/pairing/apply-config`、`/pairing/reset`、`/pairing/ap-mode`） |

Radxa PowerShell 脚本：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `-HostName` | 必填，例如 `radxa@<board-ip>` | SSH 目标 |
| `-RemoteDir` | `/opt/board-runtime` | A7Z 上运行目录 |
| `-SrcDir` | `/opt/board-runtime-src` | A7Z 上源码构建目录 |
| `-SudoPassword` | 空 | sudo 需要密码时传入 |
| `-ConfigureSpiLcd` | 关闭 | 部署后写入 A7Z SPI LCD overlay |
| `-LcdDriver` | `ili9341` | LCD framebuffer 驱动 |
| `-LcdDcPin` | `15` | A7Z physical pin 15 / PL7 |
| `-LcdResetPin` | `13` | A7Z physical pin 13 / PL6 |
| `-LcdBacklightPin` | `0` | `0` 表示 BLK 直连 3.3V，不由 GPIO 控制 |
| `-LcdSpeedHz` | `16000000` | SPI LCD 频率 |
| `-LcdRotate` | `90` | 屏幕旋转角度 |

Radxa shell 脚本：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `HOST` | 必填，例如 `radxa@<board-ip>` | SSH 目标 |
| `REMOTE_DIR` | `/opt/board-runtime` | A7Z 上运行目录 |
| `SRC_DIR` | `/opt/board-runtime-src` | A7Z 上源码构建目录 |
| `SUDO_PASSWORD` | 空 | sudo 需要密码时传入 |
| `CONFIGURE_SPI_LCD` | `0` | 设为 `1` 时写入 A7Z SPI LCD overlay |
| `LCD_DRIVER` | `ili9341` | LCD framebuffer 驱动 |
| `LCD_DC_PIN` | `15` | A7Z physical pin 15 / PL7 |
| `LCD_RESET_PIN` | `13` | A7Z physical pin 13 / PL6 |
| `LCD_BACKLIGHT_PIN` | `0` | `0` 表示 BLK 直连 3.3V，不由 GPIO 控制 |
| `LCD_SPEED_HZ` | `16000000` | SPI LCD 频率 |
| `LCD_ROTATE` | `90` | 屏幕旋转角度 |

示例：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
HOST="$BOARD_HOST" REMOTE_DIR=/opt/board-runtime sh scripts/deploy-rpi.sh
```

首次部署后建议在设备上持久化 token（避免后续部署覆盖）：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
ssh "$BOARD_HOST" "sudo systemctl edit board-runtime"
```

写入以下内容并重启服务：

```ini
[Service]
Environment=BOARD_RUNTIME_ADMIN_TOKEN=<YOUR_RANDOM_TOKEN>
```

如果同时需要写入 MQTT 凭证，建议改用上方“安全部署（强烈建议）”整段命令。

## AI 自动部署建议

为了后续“你不操心”，建议让 AI 固定按以下步骤执行：

1. 按板型运行 `scripts/deploy-rpi.sh`、`scripts/deploy-radxa-a733.sh` 或
   `scripts/deploy-radxa-a733.ps1` 完成代码部署。
2. 刷新 `/etc/systemd/system/board-runtime.service.d/10-security-env.conf`。
3. `daemon-reload` + 重启 `board-runtime`。
4. 运行安全回归（无 token 调敏感接口应失败，带 token 应成功）。

可直接给 AI 这句：

```text
按 board-runtime/docs/security-hardening.md 执行安全自动部署并完成鉴权回归检查。
```

## 部署后验证

Raspberry Pi：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
export BOARD_IP="<pi-ip>"
```

Radxa Cubie A7Z：

```sh
export BOARD_HOST="radxa@<board-ip>"
export BOARD_IP="<board-ip>"
```

通用验证命令：

```sh

# 服务是否启动
ssh "$BOARD_HOST" 'systemctl is-active board-runtime'
ssh "$BOARD_HOST" 'systemctl is-enabled board-runtime'

# 看 systemd 状态
ssh "$BOARD_HOST" 'sudo systemctl --no-pager --full status board-runtime'

# 看最近日志
ssh "$BOARD_HOST" 'sudo journalctl -u board-runtime -n 80 --no-pager'

# HTTP 服务
curl -fsS http://$BOARD_IP/board-runtime-config.json
curl -fsSI http://$BOARD_IP/

# widget runtime
ssh "$BOARD_HOST" 'sudo systemctl restart board-widget-runtime.service'
ssh "$BOARD_HOST" 'systemctl is-active board-widget-runtime.service'
```

正常日志里应该能看到：

- `board-selfcheck` 通过。
- `HTTP/WebSocket listening: http://0.0.0.0:80`。
- `local device` / `board device` 已生成。
- Raspberry Pi：`fb-display` 找到 `/dev/fb1`，`board-touch-input` 和
  `board-rotary-input` 启动。
- Radxa A7Z：`fb-display` 找到 `/dev/fb0`，`fb-rawvideo-blit.py /dev/fb0`
  正在运行；当前默认部署不启用触屏/旋钮输入。

## 运行目录

`/opt/board-runtime` 是运行目录，部署内容包括：

| 文件/目录 | 作用 |
|---|---|
| `board-server` | HTTP/WebSocket/MQTT/USB serial 主服务 |
| `board-touch-input` | 触屏输入 |
| `board-rotary-input` | GPIO 旋钮/按钮输入 |
| `fb-display.sh` | 视频播放驱动 |
| `fb-rawvideo-blit.py` | rawvideo 写 framebuffer |
| `fb-speech-overlay` | 32bpp framebuffer overlay |
| `board-widget-runtime.py` | `.clawpkg` widget runtime |
| `board-voice-ptt.py` | 按住说话 |
| `start-rpi.sh` | Raspberry Pi 启动编排 |
| `start-radxa-a733.sh` | Radxa Cubie A7Z 启动编排 |
| `board-runtime-rpi.env` | systemd 环境变量 |
| `board-runtime-radxa.env` | Radxa systemd 环境变量 |
| `board-runtime-rpi.service` | 主服务 unit 模板 |
| `board-widget-runtime.service` | widget runtime unit 模板 |
| `assets/` | 宠物素材和生成视频 |
| `ui/` | 本地 UI / 配网页面 |
| `unifont-*.hex.gz` | 字幕/中文字体资源 |

这些文件是设备现场状态，不应被普通部署覆盖：

- `device-config.json`
- `network-config.json`
- `stats/`

## Raspberry Pi 屏幕与触屏

当前 Pi 使用 ILI9341 320x240 SPI 屏和 XPT2046/ADS7846 触屏。`deploy-rpi.sh`
会自动写入 `/boot/firmware/config.txt`：

```text
dtoverlay=fbtft,spi0-0,rpi-display,reset_pin=27,dc_pin=22,led_pin=12,speed=32000000,rotate=270,fps=60
dtoverlay=ads7846,cs=1,penirq=5,penirq_pull=2,speed=2000000,xohms=150,swapxy=1
dtoverlay=googlevoicehat-soundcard
```

音频使用 `googlevoicehat-soundcard` 组合 overlay，同时暴露 I2S `MAX98357A`
播放和 `ADAU7002` 麦克风采集。运行时默认通过
`plughw:CARD=sndrpigooglevoi,DEV=0` 播放状态提示音与录音。`fb-display.sh`
会把提示音转换为 48kHz 双声道 PCM，并添加保守增益与短淡入，以避免直接播放 16kHz
单声道 WAV 时出现破音。`start-rpi.sh` 会保持 GPIO4 为高电平，避免短提示音触发功放反复启停噪声。

首次写入 overlay 后需要重启 Pi：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
ssh "$BOARD_HOST" 'sudo reboot'
```

验证：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
ssh "$BOARD_HOST" 'cat /sys/class/graphics/fb1/name'
ssh "$BOARD_HOST" 'cat /sys/class/graphics/fb1/virtual_size'
ssh "$BOARD_HOST" 'cat /proc/bus/input/devices | grep -A2 ADS7846'
```

## Radxa Cubie A7Z SPI 屏幕

Radxa Cubie A7Z 可以使用同一块 2.8 寸 Raspberry Pi 风格 ILI9341 SPI
屏，但不能直接照搬 Raspberry Pi 的 GPIO 语义。A7Z 上推荐的 LCD 接线是：

```text
LCD CLK  -> physical pin 23 / PD11 / SPI1-CLK
LCD MOSI -> physical pin 19 / PD12 / SPI1-MOSI
LCD MISO -> physical pin 21 / PD13 / SPI1-MISO
LCD CS1  -> physical pin 24 / PD10 / SPI1-CS0
LCD RES  -> physical pin 13 / PL6, active-low
LCD DC   -> physical pin 15 / PL7
LCD BLK  -> 3.3V direct, or use -BacklightPin only after verifying wiring
```

Do not use physical pin 26 as an LCD chip select on A7Z. It is
`PD14 / SPI1-HOLD`, not a Raspberry Pi-compatible `CE1`; keep it for the
touch controller side only if the touch driver is added later.

Configure the LCD overlay after deploying the runtime:

Windows PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\configure-radxa-a733-spi-lcd.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -Driver ili9341 `
  -SpiBus spi1 `
  -ChipSelect 0 `
  -DcPin 15 `
  -ResetPin 13 `
  -BacklightPin 0 `
  -SpeedHz 16000000 `
  -Rotate 90 `
  -Reboot
```

macOS/Linux PowerShell 7：

```sh
pwsh -NoProfile -ExecutionPolicy Bypass -File ./scripts/configure-radxa-a733-spi-lcd.ps1 \
  -HostName radxa@<board-ip> \
  -SudoPassword <sudo-password> \
  -Driver ili9341 \
  -SpiBus spi1 \
  -ChipSelect 0 \
  -DcPin 15 \
  -ResetPin 13 \
  -BacklightPin 0 \
  -SpeedHz 16000000 \
  -Rotate 90 \
  -Reboot
```

`configure-radxa-a733-spi-lcd.ps1` defaults to the verified A7Z settings:
ILI9341, SPI1 CS0, `RES=PL6` active-low, `DC=PL7`, no backlight GPIO,
16 MHz, rotate 90. Use `-ResetActiveHigh` only for a different display
module that is known to need high-active reset.

The Radxa deployment script can also configure this overlay in one run:

macOS/Linux/WSL/Git Bash：

```sh
HOST=radxa@<board-ip> SUDO_PASSWORD=<sudo-password> CONFIGURE_SPI_LCD=1 sh scripts/deploy-radxa-a733.sh
```

Windows PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -ConfigureSpiLcd
```

macOS/Linux PowerShell 7 也可运行 PowerShell 版本：

```sh
pwsh -NoProfile -ExecutionPolicy Bypass -File ./scripts/deploy-radxa-a733.ps1 \
  -HostName radxa@<board-ip> \
  -SudoPassword <sudo-password> \
  -ConfigureSpiLcd
```

After reboot, verify:

```sh
ssh radxa@<board-ip> 'cat /proc/fb'
ssh radxa@<board-ip> 'dmesg | grep -E "fb_ili9341|graphics fb0" | tail'
```

## 故障排查

**服务没起来**

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
ssh "$BOARD_HOST" 'sudo journalctl -u board-runtime -n 120 --no-pager'
ssh "$BOARD_HOST" 'sudo systemctl restart board-runtime'
```

**HTTP 不通**

确认服务监听 80 端口：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
ssh "$BOARD_HOST" 'sudo ss -ltnp | grep ":80"'
```

**屏幕黑屏**

Raspberry Pi：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
ssh "$BOARD_HOST" 'ls -l /dev/fb*'
ssh "$BOARD_HOST" 'cat /sys/class/graphics/fb1/name /sys/class/graphics/fb1/virtual_size'
ssh "$BOARD_HOST" 'sudo journalctl -u board-runtime -n 120 --no-pager | grep -E "fb|display"'
```

A7Z：

```sh
export BOARD_HOST="radxa@<board-ip>"
ssh "$BOARD_HOST" 'cat /proc/fb; ls -l /dev/fb*'
ssh "$BOARD_HOST" 'cat /sys/class/graphics/fb0/name /sys/class/graphics/fb0/virtual_size'
ssh "$BOARD_HOST" 'dmesg | grep -E "fb_ili9341|graphics fb0|spi1.0" | tail -40'
ssh "$BOARD_HOST" 'ps -ef | grep -E "fb-display|fb-rawvideo" | grep -v grep'
```

A7Z 仍白屏时，优先确认：

- 当前 overlay 是 `/boot/dtbo/radxa-a7z-spi28-rpi-pins-ili9341.dtbo`。
- `/proc/fb` 显示 `fb_ili9341`。
- `RES=PL6` 是 active-low，即 `reset-gpios = <&r_pio 0 6 1>`。
- `DC=PL7`，LCD CS 接 `PD10 / SPI1-CS0 / physical pin 24`。
- BLK 已直连 3.3V，或 `-BacklightPin` 与实际接线一致。
- 不要把 physical pin 26 当 LCD CS；A7Z 上它是 `PD14 / SPI1-HOLD`。

**触屏没反应**

Raspberry Pi：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
ssh "$BOARD_HOST" 'cat /proc/bus/input/devices | grep -A4 -i ADS7846'
ssh "$BOARD_HOST" 'sudo journalctl -u board-runtime -n 120 --no-pager | grep touch'
```

A7Z：

当前 A7Z 默认部署只配置 LCD，不启用 XPT2046/PEN 触摸 overlay。触摸线可先保留在
`CS2=PD14/SPI1-HOLD`、`PEN=PB2`，但不要把它作为 LCD 显示是否正常的依赖。
后续启用触摸时，需要单独增加 A7Z touch overlay 和 `/dev/input/event*` 验证。

**USB serial 冲突**

Raspberry Pi：

部署脚本会 mask `serial-getty@ttyGS0.service`。如果手动排查：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
ssh "$BOARD_HOST" 'systemctl status serial-getty@ttyGS0.service'
ssh "$BOARD_HOST" 'sudo systemctl mask serial-getty@ttyGS0.service'
```

A7Z：

当前 A7Z 部署默认走 Wi-Fi + SSH/MQTT，不依赖 Raspberry Pi 的 USB gadget
`/dev/ttyGS0`。如果无法 SSH，先从路由器 DHCP 列表、`arp -a`、板载屏幕或串口日志
确认 IP 和 Wi-Fi 状态。
