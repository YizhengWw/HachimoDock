# 本地开发指南

本文档用于从零构建 Pet Manager PC 端，并将板端 runtime 部署到
Raspberry Pi 或 Radxa Cubie A7Z 设备。

## 项目结构

```text
claw-pet-manager/
├── ref/                 # 桌面端：Pet Manager Tauri 应用
├── board-runtime/       # 设备端：Raspberry Pi / Radxa Cubie A7Z runtime
├── scripts/             # 项目辅助脚本
└── docs/                # 项目文档
```

## 前置环境

### 桌面端通用要求

Windows 和 macOS 都需要安装：

- Git
- Node.js >= 20.0.0
- npm >= 9.0.0
- Rust stable toolchain / Cargo
- Tauri 2 本地构建依赖

说明：只开发 Web UI 时，可以只启动 Vite；需要调试 USB、MQTT、本地文件、
agent session 监听和 sidecar 时，应启动完整 Tauri 应用。

### Windows 额外要求

Windows 构建 Tauri 桌面应用通常还需要：

- Microsoft C++ Build Tools
- Windows SDK
- WebView2 Runtime

桌面端开发和构建可以在 PowerShell、CMD 或 Git Bash 中执行。

Raspberry Pi 部署脚本是 shell 脚本，Windows 用户建议使用 WSL 或 Git Bash
执行 `board-runtime/scripts/deploy-rpi.sh`，不建议直接在 PowerShell 中运行该脚本。

Radxa Cubie A7Z 同时提供 Windows PowerShell 部署脚本，可直接在 Windows
PowerShell 中执行 `board-runtime/scripts/deploy-radxa-a733.ps1`。如果使用 WSL
或 Git Bash，也可以执行 `board-runtime/scripts/deploy-radxa-a733.sh`。

### macOS 额外要求

macOS 构建 Tauri 桌面应用通常需要 Xcode Command Line Tools：

```bash
xcode-select --install
```

macOS 可以直接使用系统终端执行桌面端开发、板端编译、Raspberry Pi 部署和
Radxa Cubie A7Z shell 部署命令。macOS 如需运行 PowerShell 版本的 Radxa
部署脚本，需要额外安装 PowerShell 7，并使用 `pwsh` 执行。

### 设备端部署要求

部署到 Raspberry Pi 前，需确认：

- Raspberry Pi Zero 2 W 已经安装好系统，安装方法见
  [Raspberry Pi 官方安装文档](https://www.raspberrypi.com/documentation/installation/installing-images/)
- 本机能 SSH 到 Pi，例如 `<pi-user>@<pi-ip>`
- Pi 用户具备 sudo 权限
- Pi 能访问 apt 源，用于安装 `cmake`、`gcc`、`ffmpeg`、字体和 Python 依赖
- 本机可用 `ssh`、`rsync`、`sh`

部署到 Radxa Cubie A7Z 前，需确认：

- Radxa Cubie A7Z 已经刷入官方 Debian 镜像。下载入口见
  [Radxa Cubie A7Z Downloads](https://docs.radxa.com/en/cubie/a7z/download)；
  也可查看
  [Radxa Cubie A7Z image releases](https://github.com/radxa-build/radxa-cubie-a7z/releases)
- 本机能 SSH 到 A7Z，例如 `radxa@<board-ip>`
- A7Z 用户具备 sudo 权限
- A7Z 能访问 apt 源，用于安装 `cmake`、`gcc`、`ffmpeg`、字体和 Python 依赖
- Windows PowerShell 部署需要本机可用 `ssh`、`scp`、`tar`
- macOS / Linux / WSL / Git Bash 部署需要本机可用 `ssh`、`scp`、`tar`、`sh`

正式文档和脚本中不要写死某个人的用户名、IP、Wi-Fi 名称或 COM 口。

## 获取代码

```bash
git clone <repo-url> claw-pet-manager
cd claw-pet-manager
```

如果已经 clone 过：

```bash
cd claw-pet-manager
git pull origin main
```

## 桌面端开发

桌面端位于 `ref/`，使用 Tauri 2 + React + Vite 开发。

### 1. 安装依赖

```bash
cd ref
npm install
```

### 2. 启动完整桌面端

```bash
npm run dev
```

`npm run dev` 会通过 Tauri 配置自动启动 Web 前端和桌面窗口，适合联调：

- 设备绑定和设备仪表盘
- USB serial / MQTT 下发
- 宠物形象管理
- 组件中心和 `.clawpkg` 安装
- agent session 跟随
- 语音、按钮和当前展示配置

一般不需要提前单独执行 `npm run dev:web`。如果已经手动启动过 Web 服务，再执行
`npm run dev` 可能会遇到端口占用。

### 3. 只启动 Web UI

```bash
npm run dev:web
```

默认 Web 调试地址：

```text
http://127.0.0.1:4173/
```

### 4. 桌面端构建

```bash
# 只构建 Web 产物
npm run build:web

# 构建 Tauri 桌面应用
npm run build
```

Windows 构建产物通常位于：

```text
ref/src-tauri/target/release/bundle/nsis/
ref/src-tauri/target/release/bundle/msi/
```

macOS 构建产物通常位于：

```text
ref/src-tauri/target/release/bundle/macos/
ref/src-tauri/target/release/bundle/dmg/
```

平台安装包和发布说明可查看 `docs/desktop-packaging.md`。

### 5. 桌面端测试

```bash
cd ref
npm test
```

当前测试使用 Node 内置 test runner 扫描 `src/**/*.test.js`。新增 UI 逻辑时，
建议补充状态映射、设备下发 payload、组件契约和关键用户流程测试。

### 6. 打包内置组件

```bash
cd ref
npm run pack-builtins
```

内置组件源目录在 `ref/builtin-clawpkgs/`，打包脚本会生成可安装的 `.clawpkg`
组件包。

## 设备端开发

设备端位于 `board-runtime/`，当前主线支持 Raspberry Pi + Raspberry Pi OS +
systemd，以及 Radxa Cubie A7Z + Debian + systemd。设备端负责显示宠物动画、
接收桌面端状态、处理触屏/旋钮/按钮输入、运行负一屏 widget 和配网页面。

### 1. 本机编译检查

本机开发验证需要：

- CMake >= 3.16
- C 编译器（Clang 或 GCC）
- zlib
- Python 3
- Node.js（用于部分 JS 测试，可选）

在 macOS / Linux / WSL 中执行：

```bash
cd board-runtime

cmake -S . -B /tmp/board-runtime-build-check
cmake --build /tmp/board-runtime-build-check --target board-server
```

如果要完整构建全部目标：

```bash
cmake --build /tmp/board-runtime-build-check
```

### 2. 本机自检

```bash
/tmp/board-runtime-build-check/board-server --self-check .
/tmp/board-runtime-build-check/board-server --self-check-json .
```

### 3. 运行测试

```bash
cd board-runtime

cmake -S . -B /tmp/board-runtime-build-check
cmake --build /tmp/board-runtime-build-check
ctest --test-dir /tmp/board-runtime-build-check --output-on-failure
```

## Raspberry Pi 准备

确保 Raspberry Pi 已经：

- 正常开机
- 已连接网络
- 已开启 SSH
- 当前电脑可以通过 SSH 登录
- 登录用户具有 sudo 权限

本文档使用以下占位符表示设备信息：

```text
<pi-user>  Raspberry Pi 登录用户名
<pi-ip>    Raspberry Pi IP 地址
```

先测试 SSH：

```bash
ssh <pi-user>@<pi-ip>
```

如果可以登录，再继续部署。

## Radxa Cubie A7Z 准备

确保 A7Z 已经：

- 使用官方 Debian 镜像刷入 microSD 卡并正常开机
- 已连接 2.4 GHz Wi-Fi 或其它可用网络
- 已开启 SSH
- 当前电脑可以通过 SSH 登录
- 登录用户具有 sudo 权限

本文档使用以下占位符表示 A7Z 设备信息：

```text
<board-ip>       Radxa Cubie A7Z IP 地址
<sudo-password>  A7Z 登录用户的 sudo 密码；免密 sudo 时可以不传
```

刷机建议流程：

1. 从 [Radxa Cubie A7Z Downloads](https://docs.radxa.com/en/cubie/a7z/download)
   或 [Radxa Cubie A7Z image releases](https://github.com/radxa-build/radxa-cubie-a7z/releases)
   下载官方 A733/Cubie A7Z GPT Debian 镜像。
2. 使用 Balena Etcher 选择镜像文件和 microSD 卡，执行 `Flash!` 并等待校验完成。
3. 把 microSD 插入 A7Z，接 5V USB-C 电源启动。
4. 从路由器 DHCP 列表、板载屏幕、串口日志或 `arp -a` 找到 `<board-ip>`。
5. 测试 SSH：

```bash
ssh radxa@<board-ip>
```

如果可以登录，再继续部署。默认账号以镜像说明为准；常见官方 Debian 镜像为
`radxa` / `radxa`，正式文档、脚本和提交中不要写死真实密码。

## 部署设备端到 Raspberry Pi

### macOS / Linux / WSL / Git Bash

进入设备端目录：

```bash
cd board-runtime
```

设置设备地址并部署：

```bash
export BOARD_HOST="<pi-user>@<pi-ip>"
HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh
```

部署脚本会完成以下工作：

1. 同步源码到 Pi 的 `/opt/board-runtime-src`
2. 安装或补齐 Pi 上依赖
3. 在 Pi 上运行 CMake / make 构建 native 二进制
4. 复制运行产物到 `/opt/board-runtime`
5. 安装或刷新 systemd unit
6. 重启 `board-runtime.service`

默认运行目录是 `/opt/board-runtime`，默认源码构建目录是
`/opt/board-runtime-src`。如需覆盖：

```bash
export BOARD_HOST="<pi-user>@<pi-ip>"
HOST="$BOARD_HOST" REMOTE_DIR=/opt/board-runtime SRC_DIR=/opt/board-runtime-src sh scripts/deploy-rpi.sh
```

### 首次部署后的重启

如果部署脚本提示写入了 USB gadget、屏幕或触屏相关 boot 配置，需要重启
Raspberry Pi：

```bash
ssh "$BOARD_HOST" 'sudo reboot'
```

等待设备重新开机后，再继续检查。

## 部署后检查

设置检查用变量：

```bash
export BOARD_HOST="<pi-user>@<pi-ip>"
export BOARD_IP="<pi-ip>"
```

检查服务是否运行：

```bash
ssh "$BOARD_HOST" 'systemctl is-active board-runtime board-widget-runtime'
```

期望输出：

```text
active
active
```

检查板端配置接口：

```bash
curl -fsS "http://$BOARD_IP/board-runtime-config.json"
```

检查 USB gadget 是否存在：

```bash
ssh "$BOARD_HOST" 'test -c /dev/ttyGS0 && echo ttyGS0-ok'
```

检查 USB UDC 状态：

```bash
ssh "$BOARD_HOST" 'cat /sys/class/udc/*/state 2>/dev/null || true'
```

当 USB 数据线连接电脑后，期望看到：

```text
configured
```

检查默认宠物素材是否存在：

```bash
ssh "$BOARD_HOST" 'find -L /opt/board-runtime/terrier-clips -maxdepth 1 -name "*.mp4" | wc -l'
```

期望输出大于 0。

检查当前屏幕页面：

```bash
ssh "$BOARD_HOST" 'cat /opt/board-runtime/.screen-page 2>/dev/null || true'
```

期望输出：

```text
main
```

更多部署说明可查看：

- `board-runtime/DEPLOY.md`
- `board-runtime/docs/security-hardening.md`

## 部署设备端到 Radxa Cubie A7Z

### Windows PowerShell

进入设备端目录：

```powershell
cd board-runtime
```

部署并同时写入 A7Z SPI LCD overlay：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -ConfigureSpiLcd
```

如果 A7Z 已配置免密 sudo，可以省略 `-SudoPassword`。

### macOS / Linux / WSL / Git Bash

进入设备端目录：

```bash
cd board-runtime
```

部署并同时写入 A7Z SPI LCD overlay：

```bash
HOST=radxa@<board-ip> SUDO_PASSWORD=<sudo-password> CONFIGURE_SPI_LCD=1 sh scripts/deploy-radxa-a733.sh
```

如果 A7Z 已配置免密 sudo，可以省略 `SUDO_PASSWORD`。

部署脚本会完成以下工作：

1. 打包当前 `board-runtime/` 源码并上传到 A7Z
2. 解压源码到 `/opt/board-runtime-src`
3. 安装或补齐 A7Z 上依赖
4. 在 A7Z 上运行 CMake / make 构建 native arm64 二进制
5. 复制运行产物到 `/opt/board-runtime`
6. 安装或刷新 systemd unit 和 `/opt/board-runtime/start-radxa-a733.sh`
7. 设定 `CONFIGURE_SPI_LCD=1` 或 `-ConfigureSpiLcd` 时，写入 A7Z 专用 SPI LCD overlay
8. 重启设备，或重启 `board-runtime.service` 和 `board-widget-runtime.service`

### A7Z SPI 屏幕接线

Radxa Cubie A7Z 可以使用同一块 2.8 寸 Raspberry Pi 风格 ILI9341 SPI 屏，
但 GPIO 名称和 Raspberry Pi 不完全等价。当前验证过的 A7Z 接线是：

```text
LCD CLK   physical pin 23 / PD11 / SPI1-CLK
LCD MOSI  physical pin 19 / PD12 / SPI1-MOSI
LCD MISO  physical pin 21 / PD13 / SPI1-MISO
LCD CS1   physical pin 24 / PD10 / SPI1-CS0
LCD RES   physical pin 13 / PL6
LCD DC    physical pin 15 / PL7
LCD BLK   直接接 3.3V
VCC       physical pin 17 / 3.3V
GND       physical pin 25 / GND
```

不要把 physical pin 26 当作 LCD 片选使用；A7Z 上该脚是
`PD14 / SPI1-HOLD`，不是 Raspberry Pi 兼容的 `CE1`。当前 A7Z 默认部署只配置
LCD，不启用 XPT2046/PEN 触摸 overlay。

### A7Z 部署后检查

设置检查用变量：

```bash
export BOARD_HOST="radxa@<board-ip>"
export BOARD_IP="<board-ip>"
```

检查服务是否运行：

```bash
ssh "$BOARD_HOST" 'systemctl is-active board-runtime board-widget-runtime'
```

检查板端配置接口：

```bash
curl -fsS "http://$BOARD_IP/board-runtime-config.json"
```

检查 framebuffer：

```bash
ssh "$BOARD_HOST" 'cat /proc/fb; ls -l /dev/fb*'
ssh "$BOARD_HOST" 'cat /sys/class/graphics/fb0/name /sys/class/graphics/fb0/virtual_size'
```

A7Z 默认使用 Wi-Fi + SSH / MQTT，不依赖 Raspberry Pi 的 USB gadget
`/dev/ttyGS0`。

## 双端联调

### 1. 启动设备端

先确保设备上的 `board-runtime` 正常运行。

Raspberry Pi：

```bash
export BOARD_HOST="<pi-user>@<pi-ip>"
ssh "$BOARD_HOST" 'systemctl is-active board-runtime'
```

Radxa Cubie A7Z：

```bash
export BOARD_HOST="radxa@<board-ip>"
ssh "$BOARD_HOST" 'systemctl is-active board-runtime'
```

设备本地 HTTP 服务默认监听 80 端口：

```text
http://<board-ip>/
http://<board-ip>/board-runtime-config.json
http://<board-ip>/debug/state
```

### 2. 启动桌面端

```bash
cd ref
npm run dev
```

### 3. 配置设备目标

调试具体板子时使用环境变量记录目标，不要把设备 IP、用户名或 board id 写死到
业务代码和文档里：

```bash
export BOARD_HOST="<pi-user>@<pi-ip>"
export BOARD_IP="<pi-ip>"
```

A7Z：

```bash
export BOARD_HOST="radxa@<board-ip>"
export BOARD_IP="<board-ip>"
```

桌面端和设备端支持两条通信路径：

- USB serial：用于直连设备、按钮配置、组件安装和状态同步。
- MQTT：用于无线可达性、远程绑定和状态/语音同步。

### 4. 检查基础链路

```bash
# 设备 HTTP 可访问
curl -fsS http://$BOARD_IP/board-runtime-config.json

# 设备服务正常
ssh "$BOARD_HOST" 'systemctl is-active board-runtime'

# 查看设备端最近状态
ssh "$BOARD_HOST" 'ls -la /opt/board-runtime | sed -n "1,80p"'
ssh "$BOARD_HOST" 'sudo journalctl -u board-runtime -n 80 --no-pager'
```

## 推荐的新手完整流程

### 1. 启动桌面端

```bash
git clone <repo-url> claw-pet-manager
cd claw-pet-manager/ref
npm install
npm run dev
```

### 2. 部署设备端

Raspberry Pi：另开一个 macOS / Linux / WSL / Git Bash 终端：

```bash
cd claw-pet-manager/board-runtime

export BOARD_HOST="<pi-user>@<pi-ip>"
HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh
```

Radxa Cubie A7Z：Windows PowerShell 终端：

```powershell
cd claw-pet-manager\board-runtime

powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -ConfigureSpiLcd
```

Radxa Cubie A7Z：macOS / Linux / WSL / Git Bash 终端：

```bash
cd claw-pet-manager/board-runtime

HOST=radxa@<board-ip> SUDO_PASSWORD=<sudo-password> CONFIGURE_SPI_LCD=1 sh scripts/deploy-radxa-a733.sh
```

如果脚本提示需要重启：

```bash
ssh "$BOARD_HOST" 'sudo reboot'
```

### 3. 验证设备端

Raspberry Pi 重启后执行：

```bash
export BOARD_HOST="<pi-user>@<pi-ip>"
export BOARD_IP="<pi-ip>"

ssh "$BOARD_HOST" 'systemctl is-active board-runtime board-widget-runtime'
curl -fsS "http://$BOARD_IP/board-runtime-config.json"
ssh "$BOARD_HOST" 'test -c /dev/ttyGS0 && echo ttyGS0-ok'
```

A7Z 重启后执行：

```bash
export BOARD_HOST="radxa@<board-ip>"
export BOARD_IP="<board-ip>"

ssh "$BOARD_HOST" 'systemctl is-active board-runtime board-widget-runtime'
curl -fsS "http://$BOARD_IP/board-runtime-config.json"
ssh "$BOARD_HOST" 'cat /proc/fb; ls -l /dev/fb*'
```

### 4. 在桌面端连接设备

在 Pet Manager 中进入设备页面，确认：

- USB 显示已连接，或 Wi-Fi 显示在线
- 可以扫描到设备
- 可以切换 Agent
- 可以更换形象并同步到设备端

## 常见问题

### `npm run dev` 端口占用

现象：Tauri 启动失败，或提示 Vite 端口 `4173` 已被占用。

处理：

- 检查是否已经单独运行了 `npm run dev:web`
- 关闭旧的 Vite 进程后重新执行 `npm run dev`
- 只调 Web UI 时保留 `npm run dev:web`，不要再启动完整 Tauri 应用

### Windows 上部署脚本无法运行

现象：PowerShell 中执行 `sh scripts/deploy-rpi.sh` 失败，或找不到 `rsync`。

处理：

- 优先使用 WSL
- 或使用 Git Bash，并确认 `ssh`、`rsync`、`sh` 可用
- 桌面端开发仍可继续使用 PowerShell、CMD 或 Git Bash

部署 A7Z 时，Windows 优先使用 PowerShell 脚本：

```powershell
cd board-runtime
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -ConfigureSpiLcd
```

如果提示缺少 `ssh`、`scp` 或 `tar`，先确认 Windows OpenSSH 客户端可用，
并在 PowerShell 中单独执行 `ssh radxa@<board-ip>` 验证连接。

### macOS 构建 Tauri 失败

现象：缺少编译工具、链接失败或找不到系统 SDK。

处理：

```bash
xcode-select --install
```

安装后重新打开终端，再执行：

```bash
cd ref
npm run build
```

### Windows 构建 Tauri 失败

现象：提示缺少 C++ 编译器、Windows SDK 或 WebView2。

处理：

- 安装 Microsoft C++ Build Tools
- 安装 Windows SDK
- 安装 WebView2 Runtime
- 重新打开终端后再执行 `npm run build`

### Raspberry Pi SSH 不通

现象：`ssh <pi-user>@<pi-ip>` 连接失败。

处理：

- 确认 Pi 已开机并连接网络
- 确认 SSH 已开启
- 确认电脑和 Pi 在同一网络或路由可达
- 确认 `<pi-user>`、`<pi-ip>` 填写正确

### Radxa Cubie A7Z SSH 不通

现象：`ssh radxa@<board-ip>` 连接失败。

处理：

- 确认 A7Z 已开机并连接 2.4 GHz Wi-Fi 或其它可用网络
- 从路由器 DHCP 列表、板载屏幕、串口日志或 `arp -a` 确认 `<board-ip>`
- 确认 SSH 已开启，且电脑和 A7Z 在同一网络或路由可达
- 确认账号以当前镜像说明为准，不要假设所有镜像账号密码完全一致

### 部署脚本提示权限不足

现象：远端执行安装依赖、写入 `/opt/board-runtime` 或安装 systemd unit 失败。

处理：

- 确认目标设备登录用户具备 sudo 权限
- 单独测试 `ssh "$BOARD_HOST" 'sudo true'`
- Raspberry Pi shell 部署如果 sudo 需要密码，按终端提示输入密码
- A7Z 部署如果 sudo 需要密码，传入 `SUDO_PASSWORD=<sudo-password>` 或
  `-SudoPassword <sudo-password>`

### `board-runtime` 服务未启动

现象：`systemctl is-active board-runtime` 输出不是 `active`。

处理：

```bash
export BOARD_HOST="<pi-user>@<pi-ip>"
ssh "$BOARD_HOST" 'sudo systemctl --no-pager --full status board-runtime'
ssh "$BOARD_HOST" 'sudo journalctl -u board-runtime -n 120 --no-pager'
ssh "$BOARD_HOST" 'sudo systemctl restart board-runtime'
```

### HTTP 接口不通

现象：`curl http://$BOARD_IP/board-runtime-config.json` 失败。

处理：

```bash
export BOARD_HOST="<pi-user>@<pi-ip>"
ssh "$BOARD_HOST" 'systemctl is-active board-runtime'
ssh "$BOARD_HOST" 'sudo ss -ltnp | grep ":80"'
ssh "$BOARD_HOST" 'sudo journalctl -u board-runtime -n 120 --no-pager'
```

### USB 连接后没有识别到设备

现象：桌面端 USB 不显示已连接，或 Pi 上 USB gadget 未进入 configured 状态。

处理：

```bash
export BOARD_HOST="<pi-user>@<pi-ip>"
ssh "$BOARD_HOST" 'test -c /dev/ttyGS0 && echo ttyGS0-ok'
ssh "$BOARD_HOST" 'cat /sys/class/udc/*/state 2>/dev/null || true'
ssh "$BOARD_HOST" 'systemctl status serial-getty@ttyGS0.service --no-pager || true'
```

如果 `serial-getty@ttyGS0.service` 冲突，可以执行：

```bash
ssh "$BOARD_HOST" 'sudo systemctl mask serial-getty@ttyGS0.service'
ssh "$BOARD_HOST" 'sudo systemctl restart board-runtime'
```

### 屏幕黑屏、白屏或触屏无响应

Raspberry Pi 屏幕排查：

```bash
export BOARD_HOST="<pi-user>@<pi-ip>"
ssh "$BOARD_HOST" 'ls -l /dev/fb*'
ssh "$BOARD_HOST" 'cat /sys/class/graphics/fb1/name /sys/class/graphics/fb1/virtual_size'
ssh "$BOARD_HOST" 'sudo journalctl -u board-runtime -n 120 --no-pager | grep -E "fb|display"'
```

触屏排查：

```bash
ssh "$BOARD_HOST" 'cat /proc/bus/input/devices | grep -A4 -i ADS7846'
ssh "$BOARD_HOST" 'sudo journalctl -u board-runtime -n 120 --no-pager | grep touch'
```

首次写入屏幕、触屏或 USB gadget boot 配置后，通常需要重启 Pi：

```bash
ssh "$BOARD_HOST" 'sudo reboot'
```

A7Z 屏幕排查：

```bash
export BOARD_HOST="radxa@<board-ip>"
ssh "$BOARD_HOST" 'cat /proc/fb; ls -l /dev/fb*'
ssh "$BOARD_HOST" 'cat /sys/class/graphics/fb0/name /sys/class/graphics/fb0/virtual_size'
ssh "$BOARD_HOST" 'dmesg | grep -E "fb_ili9341|graphics fb0|spi" | tail -80'
```

A7Z 仍白屏时，优先确认：

- `CONFIGURE_SPI_LCD=1` 或 `-ConfigureSpiLcd` 已执行
- SPI 使用 `SPI1`，LCD CS 使用 physical pin 24 / PD10 / SPI1-CS0
- `RES=PL6`、`DC=PL7`
- `BLK` 已直接接到 3.3V，或使用正确的 backlight GPIO
- 不要把 physical pin 26 当 LCD CS；A7Z 上它是 `PD14 / SPI1-HOLD`

当前 A7Z 默认部署只配置 LCD，不启用 XPT2046/PEN 触摸 overlay。触摸线可先保留在
`MISO=PD13`、`PEN=PB2`，后续启用触摸时需要单独增加 A7Z touch overlay。

## 项目配置

### 桌面端配置

- 桌面端主应用：`ref/src/`
- Tauri 后端：`ref/src-tauri/`
- Tauri 配置：`ref/src-tauri/tauri.conf.json`
- bridge sidecar：`ref/src-tauri/bridge/`
- 内置组件：`ref/builtin-clawpkgs/`

### 设备端配置

- 设备端源码：`board-runtime/src/`
- systemd 环境变量：`board-runtime/board-runtime-rpi.env`
- Radxa systemd 环境变量：`board-runtime/board-runtime-radxa.env`
- 主服务 unit 模板：`board-runtime/board-runtime-rpi.service`
- widget runtime unit 模板：`board-runtime/board-widget-runtime.service`
- 设备运行目录：`/opt/board-runtime`
- 设备源码构建目录：`/opt/board-runtime-src`

### 设备现场文件

以下文件是设备现场状态，部署或手动拷贝时不要随意覆盖：

- `device-config.json`：设备身份
- `network-config.json`：Wi-Fi、MQTT 和桌面端绑定
- `stats/`：本地统计数据

## 参考文档

- `README.md`
- `ref/README.md`
- `board-runtime/README.md`
- `board-runtime/DEPLOY.md`
- `docs/desktop-packaging.md`
