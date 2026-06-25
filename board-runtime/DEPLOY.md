# Board Runtime 设备部署说明

本教程的默认目标设备是 **Radxa Cubie A7Z + Debian + systemd**。目标是让一个
全新用户从空白 microSD 卡开始，首次把本项目的画面显示到 A7Z 小屏硬件上。

Raspberry Pi + Raspberry Pi OS + systemd 仍受支持，但属于兼容路线；如果没有特别
说明，下文“首次部署”均指 Radxa Cubie A7Z。

## 阅读方式和总体原则

这篇文档按“先让系统活起来，再部署项目，再让项目画面接管屏幕，最后打通桌面联动”
的顺序写。全新用户不要跳步骤；AI 代执行时也要按每个检查点停下来确认结果。

总体原则：

1. **默认按 A7Z 路线执行。**Radxa Cubie A7Z 是当前复刻部署默认板型；Raspberry Pi
   是兼容路线，旧 T113/MangoPi 是历史路线。A7Z 不使用 PhoenixCard，不使用 T113
   镜像，也不依赖 Raspberry Pi 的 USB gadget `/dev/ttyGS0`。
2. **先系统联网，再 SSH 部署。**Wi-Fi 密码第一次写入的是 A7Z Debian 系统层；
   `/opt/board-runtime/network-config.json` 是项目 runtime 配置，不能替代首次联网。
3. **每层只验证一件事。**先验证 SSH，再验证 `board-runtime` HTTP，再验证屏幕，
   最后验证 MQTT/binding。上一层没通过，不要继续下一层。
4. **首次成功的核心验收是小屏显示项目画面。**先让 `board-runtime` 接管 SPI 屏；
   桌面端 agent 跟随和 MQTT/binding 是后续联动验收。
5. **A7Z 联动主链路是 Wi-Fi + HTTP + MQTT + binding。**桌面端显示 USB 未连接，
   不等于 A7Z 部署失败。
6. **不要写死个人环境。**文档和提交里只使用 `<board-ip>`、`<pc-lan-ip>`、
   `<wifi-ssid>` 等占位符。实际 IP 可以通过脚本探测或路由器 DHCP 固定。

命令位置约定：

| 标记 | 在哪里执行 | 典型命令 |
|---|---|---|
| 仓库根目录 | `claw-pet-manager/` | `powershell -File .\scripts\start-hachimo-link.ps1` |
| 设备端目录 | `claw-pet-manager/board-runtime/` | `scripts/deploy-radxa-a733.*` |
| A7Z 本机 | HDMI+键盘、串口或 SSH 登录后的 A7Z shell | `nmcli`、`systemctl`、`journalctl` |
| 桌面端目录 | `claw-pet-manager/ref/` | `npm install`、`npm run dev` |

AI 执行时，如果命令块没有明确当前目录，先不要猜；回到上表确认后再执行。

## 全局准备

这些是开始阅读和执行教程前的通用准备，不代表每一步都已经满足：

- 本地按命令块标注的目录执行命令；部署脚本在 `board-runtime/`，仓库级脚本在仓库根目录。
- macOS/Linux 可直接运行 shell 脚本；Windows 运行 shell 脚本建议使用 WSL 或 Git Bash。
- Radxa 既有 shell 部署脚本，也有 Windows PowerShell 部署脚本；Windows 首选 PowerShell 脚本。
- 全新 A7Z 首次部署一开始通常还不能 SSH；SSH、sudo、apt 可用性要在写入系统 Wi-Fi
  并拿到 `<board-ip>` 后再检查。

## Raspberry Pi 一键部署

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh
```

Windows 上部署 Raspberry Pi 时，建议在 WSL 或 Git Bash 中执行同一组命令。

## Radxa Cubie A7Z 一键部署

### 全新环境准备清单

开始前准备这些东西，缺一项就先补齐：

| 类别 | 需要准备 | 说明 |
|---|---|---|
| 写系统卡 | 新 microSD 卡 + 稳定读卡器 | 仅写入系统卡时，硬件至少需要这两样；如果系统显示 `No Media`，先重插卡和读卡器，不要继续写盘 |
| 烧录工具 | Balena Etcher | A7Z 官方 GPT Debian 镜像用 Etcher 写入，不用 PhoenixCard |
| 上板激活 | Radxa Cubie A7Z 开发板 | 这是默认部署目标；先确认丝印或购买型号，不要套用 T113/MangoPi 流程 |
| 显示硬件 | PCB/PSB 转接板、ILI9341 SPI 屏幕、必要排线 | 目标是首次把项目画面显示到屏幕；白屏/黑屏时优先查这条硬件链路 |
| 连接线 | 稳定 USB-C 数据/供电线 | 上电、SSH/调试和后续排查都依赖稳定供电；不要用只能充电且不稳定的数据线做排查 |
| 首次登录工具 | HDMI+键盘，或 USB-TTL 串口 | 首次 Wi-Fi 未写入前通常还不能 SSH，需要本地登录方式 |
| 网络 | 电脑和 A7Z 可加入同一局域网 | 避免访客 Wi-Fi、公司隔离网络、跨网段不可达网络 |
| Windows 工具 | PowerShell、OpenSSH `ssh/scp`、`tar`、Node.js | A7Z PowerShell 部署脚本和本地 MQTT/bridge 脚本需要这些工具 |
| 仓库依赖 | `ref/` 和 bridge 依赖已安装 | 桌面端联动需要完整 Tauri/bridge，不只是 Vite Web 页面 |

新手推荐先用同一个普通路由器的 Wi-Fi。2.4 GHz 或 5 GHz 本身不是关键，关键是电脑
和 A7Z 必须互相可达；很多访客网络或办公网络会阻止设备互访。

阶段边界：

- **写系统卡阶段**：只需要新 microSD 卡、读卡器、电脑和 Balena Etcher。
- **上板首次显示阶段**：至少需要 A7Z 开发板、PCB/PSB 转接板、SPI 屏幕、排线、
  稳定 USB-C 线都连接好。
- **桌面联动阶段**：在屏幕已显示项目画面后，再检查 MQTT/binding 和 Pet Manager。

### 下载清单和版本选择

不要只打开官网首页就随便下载。首次 A7Z 复刻按下面选择：

| 下载项 | 下载位置 | 应选择的版本/文件 | 不要选 |
|---|---|---|---|
| A7Z 系统镜像 | [Radxa Cubie A7Z Downloads](https://docs.radxa.com/en/cubie/a7z/download) 或 [A733 image releases](https://github.com/radxa-build/radxa-cubie-a7z/releases) | **GPT System Image / A733 Unified System Image / Radxa Cubie A7Z Debian 11 KDE R6 (Latest) (SD / eMMC)**。如果官方发布页未来更新到更高正式版，选择最新 `r*` release 的 SD/eMMC KDE/Desktop 镜像 | `test` / `pre-release` / `t*` beta 镜像、Legacy Images、Android、UFS、NVMe/SPI Nor 相关镜像、T113/MangoPi 镜像 |
| 无头调试镜像 | 同上 | 只有需要纯 SSH/串口调试时，才选择 **Radxa Cubie A7Z Debian 11 CLI R6 (Latest) (SD / eMMC)** 或未来最新 `r*` CLI release | 把 CLI 当默认新手镜像；它不适合用屏幕/桌面确认首次启动 |
| 写卡工具 | [Balena Etcher](https://etcher.balena.io/) | 当前系统对应的最新稳定版 Etcher | PhoenixCard、RKDevTool、`dd` 直接写未知镜像 |
| Node.js | [Node.js](https://nodejs.org/) | Node.js 20 或更新 LTS；`npm >= 9` | 很旧的 Node 16/18 环境 |
| Git / SSH | Git 官网或系统包管理器；Windows 可用系统 OpenSSH | Windows PowerShell 中 `git`、`ssh`、`scp`、`tar` 均可用 | 只装 Git GUI 但命令行不可用 |
| Rust / Tauri 构建工具 | [Rust](https://www.rust-lang.org/tools/install) 和 Tauri 2 官方要求 | 只需要构建/运行完整桌面端时安装；使用已构建 release 可跳过 | 把 Rust 当成写 SD 卡前置条件 |

截至 2026-06-25，Radxa A7Z 官方下载页标注的 latest 是 R6。教程写成
“R6 或未来更新的最新正式 `r*` release”，是为了避免将来官方发布 R7/R8 后还误下旧包。
如果 AI 执行到下载步骤，应先向用户确认下载文件名里包含：

```text
Cubie A7Z
Debian 11
KDE 或 Desktop
R6 或更新的 r* release
SD / eMMC
```

文件名或页面如果出现 `Legacy`、`Android`、`UFS`、`test`、`pre-release`、`t*`、
`T113`、`MangoPi`，应停下来重新确认，不要继续烧卡。

### 首次部署主流程

Radxa Cubie A7Z 的标准路线是：官方 Debian 系统镜像、系统层 Wi-Fi、
SSH 部署、A7Z SPI LCD overlay、`board-runtime` 接管屏幕、Wi-Fi/MQTT 绑定。
不要把 T113/PhoenixCard 或 Raspberry Pi USB gadget 流程混进来。

| 顺序 | 必做动作 | 验收标准 |
|---|---|---|
| 1 | 默认确认板型为 Radxa Cubie A7Z / A733 系列 | 不使用 T113/MangoPi 镜像，不使用 PhoenixCard |
| 2 | 下载官方 A733/Cubie A7Z GPT Debian 镜像 | 得到解压后的 `.img` |
| 3 | 用 Balena Etcher 写入 microSD 并等待校验完成 | Etcher 显示 flash/verify 成功 |
| 4 | 把 microSD 插入 A7Z，并连接开发板、PCB/PSB 板、屏幕、数据/供电线 | 板子上电，屏幕背光或系统启动迹象正常 |
| 5 | **写入系统 Wi-Fi 信息** | `nmcli` 显示已连接 SSID，A7Z 有 IPv4 |
| 6 | 找到 `<board-ip>` 并验证 SSH | 电脑上 `ping <board-ip>` 和 `ssh radxa@<board-ip>` 可用 |
| 7 | 确认电脑 LAN IP | `<pc-lan-ip>` 和 A7Z 在同一可达网络 |
| 8 | 启动电脑端 MQTT broker + bridge，或准备等价 MQTT broker | 电脑有可被 A7Z 访问的 `mqtt://<pc-lan-ip>:1883`，bridge 监听 `23333` |
| 9 | 部署 `board-runtime` 并写入 A7Z LCD overlay | `board-runtime` 和 `board-widget-runtime` 为 `active` |
| 10 | 验证项目首次显示 | `curl http://<board-ip>/board-runtime-config.json` 成功，`/proc/fb` 有 `fb_ili9341`，小屏不再停留在系统 login |
| 11 | 在 Pet Manager 中绑定设备和 agent source | A7Z 订阅 `desk/<desktop-device-id>/state/<source>` |
| 12 | 重启 A7Z 和电脑端链路做回归 | 项目画面、HTTP、MQTT、manager 状态都恢复 |

### 必做停顿点：写入系统 Wi-Fi

这一步不能省略。部署脚本需要先通过 SSH 登录 A7Z，而 SSH 的前提是 A7Z
已经连入电脑可达的网络。`/opt/board-runtime/network-config.json` 是 runtime
配置，不负责首次让 Debian 系统联网。

Wi-Fi 信息写入位置和工具：

| 配置 | 写入位置 | 谁写 | 工具 |
|---|---|---|---|
| 系统 Wi-Fi / 密码 | NetworkManager 配置，通常在 `/etc/NetworkManager/system-connections/` | 用户或部署者在 A7Z 上写 | `nmcli`、HDMI+键盘、串口终端 |
| runtime 网络/MQTT | `/opt/board-runtime/network-config.json` | 部署脚本或 Pet Manager 写 | `deploy-radxa-a733.*`、manager/bridge |

推荐在 A7Z 本机终端执行：

```sh
sudo nmcli dev wifi connect "<wifi-ssid>" password "<wifi-password>"
nmcli -t -f active,ssid dev wifi
ip -4 addr
hostname -I
```

如果还不能 SSH，需要临时接 HDMI + 键盘，或通过串口登录后执行上面的
`nmcli` 命令。完成后把 `hostname -I` 里和电脑同网段的 IPv4 记为 `<board-ip>`。

在电脑上验证：

```sh
ping <board-ip>
ssh radxa@<board-ip>
```

Windows PowerShell 也可以用：

```powershell
Test-Connection <board-ip> -Count 4
ssh radxa@<board-ip>
```

如果 `ping` 不通：

- 确认电脑和 A7Z 连的是同一个 SSID，或至少在同一个可互访 LAN。
- 确认不是访客 Wi-Fi、办公隔离网络或手机热点的客户端隔离模式。
- 从路由器 DHCP 列表查 A7Z 的 IP；必要时给电脑和 A7Z 做 DHCP 固定 IP。
- 电脑换 Wi-Fi 后 `<pc-lan-ip>` 会变，后面 MQTT URL 也要重新生成。

AI 执行规则：

- 执行到本节时必须停下来，提示用户写入 Wi-Fi 并返回 `<board-ip>`。
- 必须解释 Wi-Fi 密码写入的是 A7Z 的 Debian 系统，不是仓库文件。
- 拿到 `<board-ip>` 且 `ssh radxa@<board-ip>` 成功前，不得运行部署脚本。
- 如果用户只有屏幕或串口，没有网络，先完成系统联网，不要尝试用
  `network-config.json` 代替这一步。
- 继续前应向用户确认三件事：A7Z 当前 SSID、`<board-ip>`、电脑当前 SSID/LAN IP。

### 刷写系统

官方入口：

- [Radxa Cubie A7Z Downloads](https://docs.radxa.com/en/cubie/a7z/download)
- [Install System to microSD Card](https://docs.radxa.com/en/cubie/a7z/getting-started/install-system/microsd)
- [Quick Start / Booting the System](https://docs.radxa.com/en/cubie/a7z/getting-started/quickly-start)
- [Radxa Cubie A7Z image releases](https://github.com/radxa-build/radxa-cubie-a7z/releases)

推荐流程：

1. 从 Radxa Downloads 页面下载 A733/Cubie A7Z 官方 GPT 系统镜像。首次复刻默认选
   **Radxa Cubie A7Z Debian 11 KDE R6 (Latest) (SD / eMMC)**；如果官方已经发布
   更新的正式版，则选择最新 `r*` release 的 SD/eMMC KDE/Desktop 镜像。
2. 下载后先解压镜像压缩包，得到 `.img` 文件。
3. 用 Balena Etcher 选择镜像文件和 microSD 卡，执行 `Flash!` 并等待校验完成。
4. 把 microSD 插入 A7Z，接 5V USB-C 电源启动。
5. 按上方“必做停顿点”写入系统 Wi-Fi，确认 A7Z 和电脑在同一局域网。
6. 找到 `<board-ip>`，确认 SSH 可用，例如 `ssh radxa@<board-ip>`。默认账号以镜像说明为准；
   当前实测 Debian 镜像常见为 `radxa` / `radxa`。

注意：Radxa 下载页里旧的 Cubie A7Z legacy 镜像不再更新；新部署优先走
`GPT System Image` / A733 unified image。CLI 镜像只用于无头调试；默认首次显示
路线优先使用 KDE/Desktop 镜像。不使用 test/pre-release 镜像，除非 Radxa 支持人员
明确要求。

### 部署 runtime

### 运行部署脚本前检查

到这里才要求 SSH、sudo 和 apt 可用。没有通过本节检查，不要运行
`deploy-radxa-a733.*`。

在电脑上确认 SSH：

```sh
ssh radxa@<board-ip>
```

在 A7Z 上确认 sudo 和 apt 网络：

```sh
sudo true
ping -c 4 deb.debian.org
sudo apt-get update
```

期望：

- `ssh radxa@<board-ip>` 可以登录。
- `sudo true` 不报权限错误；如果需要密码，后续部署脚本要传 `SUDO_PASSWORD`
  或 `-SudoPassword`。
- `apt-get update` 能访问软件源；否则部署脚本无法安装 `cmake`、`gcc`、`ffmpeg`、
  字体和 Python 依赖。

如果使用本地电脑作为 MQTT broker，先准备桌面端和 bridge 依赖，再从仓库根目录
启动桌面链路。注意：`start-hachimo-link.ps1` 是 Windows PowerShell 脚本，命令
必须在仓库根目录执行，不是在 `board-runtime/` 目录执行。

首次克隆仓库后先安装依赖：

```powershell
# 仓库根目录
cd <repo-root>

# 桌面端依赖
cd .\ref
npm install

# bridge/MQTT 依赖
cd .\src-tauri\bridge\packages\clawd-backend-service
npm install
```

回到仓库根目录启动本地 broker + bridge。脚本会自动探测当前默认网关对应的
IPv4；如果电脑有多个网卡，也可以显式传 `-LanIp <pc-lan-ip>`。

```powershell
# 仓库根目录
cd <repo-root>
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\start-hachimo-link.ps1
```

成功时至少要看到：

- `MQTT URL: mqtt://<pc-lan-ip>:1883`
- `MQTT broker` 表格里有 `1883` 监听
- `Bridge` 表格里有 `23333` 监听
- `Bridge health` 能返回 JSON，而不是连接失败

可用这些命令复查电脑端状态：

```powershell
Get-NetTCPConnection -State Listen -LocalPort 1883,23333
Invoke-WebRequest -UseBasicParsing -TimeoutSec 3 http://127.0.0.1:23333/state
```

如果 Windows 防火墙询问是否允许 Node.js 访问网络，应允许当前可信局域网访问；
否则 A7Z 可能连不到 `mqtt://<pc-lan-ip>:1883`。

非 Windows 环境可以使用一个已有的局域网 MQTT broker，或自行启动等价 broker；
关键是电脑 bridge 和 A7Z 的 `MQTT_URL` 必须完全指向同一个 broker。

如果已经知道板子 SSH 目标，并希望同步板端 MQTT URL：

```powershell
# 仓库根目录
cd <repo-root>
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\start-hachimo-link.ps1 `
  -BoardHost radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -UpdateBoard
```

脚本输出的 `MQTT URL` 应该能从 A7Z 访问。部署 runtime 时把同一个 URL 传给
部署脚本，避免板子继续使用 `mqtt://broker.openclaw.example:1883` 这个占位默认值。

可以从 A7Z 上用 Python 做一次端口连通性检查：

```sh
python3 - <<'PY'
import socket
host = "<pc-lan-ip>"
port = 1883
with socket.create_connection((host, port), timeout=5):
    print("mqtt-port-ok")
PY
```

macOS/Linux/WSL/Git Bash：

```sh
# 设备端目录
cd <repo-root>/board-runtime
MQTT_URL="mqtt://<pc-lan-ip>:1883" \
HOST=radxa@<board-ip> \
SUDO_PASSWORD=<sudo-password> \
CONFIGURE_SPI_LCD=1 \
sh scripts/deploy-radxa-a733.sh
```

Windows PowerShell：

```powershell
# 设备端目录
cd <repo-root>\board-runtime
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -MqttUrl mqtt://<pc-lan-ip>:1883 `
  -ConfigureSpiLcd
```

部署脚本会在 A7Z 上读取当前已连接的 SSID，并在缺少配置时生成：

```text
/opt/board-runtime/network-config.json
```

这个文件记录 runtime 的 SSID、MQTT URL 和 namespace；它不是首次联网工具。

部署脚本如果写入了 LCD overlay，可能会重启 A7Z。等待 60-120 秒后重新确认
`<board-ip>`；如果 IP 变了，以路由器 DHCP 或 `hostname -I` 为准。

### A7Z 首次显示最小验收

先确认板端服务和 HTTP：

```sh
export BOARD_HOST="radxa@<board-ip>"
export BOARD_IP="<board-ip>"

ssh "$BOARD_HOST" 'systemctl is-active board-runtime board-widget-runtime'
curl -fsS "http://$BOARD_IP/board-runtime-config.json"
ssh "$BOARD_HOST" 'cat /proc/fb; ls -l /dev/fb*'
ssh "$BOARD_HOST" 'sudo journalctl -u board-runtime -n 80 --no-pager'
```

再确认 runtime 记录了正确 MQTT URL：

```sh
ssh "$BOARD_HOST" 'sudo cat /opt/board-runtime/network-config.json 2>/dev/null || true'
ssh "$BOARD_HOST" 'grep -E "MQTT_BROKER_URL|PET_CLAW_TARGET_SOURCE" /opt/board-runtime/board-runtime-radxa.env 2>/dev/null || true'
```

期望：

- `board-runtime` 和 `board-widget-runtime` 都是 `active`。
- `board-runtime-config.json` 能从电脑访问。
- `/proc/fb` 能看到 `fb_ili9341`。
- `network-config.json` 里的 `mqttUrl` 指向当前 `mqtt://<pc-lan-ip>:1883`。
- 小屏应显示项目画面或宠物状态，不应长期停留在 Debian login 控制台。

如果屏幕停在 Debian login，说明系统和 framebuffer 大概率已经通了，但项目显示驱动
没有接管屏幕；优先看 `fb-display.sh` 日志和 `/opt/board-runtime/.fb-display.lock`。

### A7Z 桌面联动和绑定验收

A7Z 首次显示成功后，再做桌面联动。联动不是看 USB 串口，而是看同一个 MQTT
broker 上是否同时有：

1. A7Z 发布 board online / hello。
2. 电脑 bridge 发布 `desk/<desktop-device-id>/state/<source>`。
3. A7Z 绑定到这个 `<desktop-device-id>` 和 `<source>`。

优先在 Pet Manager 里完成绑定：启动完整桌面端，进入设备/形象或 agent 绑定流程，
选择当前 agent source（例如 `codex`），把它设为设备跟随目标。

```powershell
# 桌面端目录
cd <repo-root>\ref
npm run dev
```

如果需要用 bridge 接口调试，先看 bridge state，找到 `desktopDeviceId` 和在线
`boardDeviceId`：

```powershell
Invoke-WebRequest -UseBasicParsing http://127.0.0.1:23333/state
```

然后发布绑定：

```powershell
$body = @{
  boardDeviceId = "<board-device-id>"
  binding = @{
    targetDeviceId = "<desktop-device-id>"
    targetSource = "codex"
  }
} | ConvertTo-Json -Depth 4

Invoke-WebRequest `
  -UseBasicParsing `
  -Method POST `
  -ContentType "application/json" `
  -Body $body `
  http://127.0.0.1:23333/publish-remote-binding
```

绑定成功后，A7Z 的配置接口或日志应能看到它订阅到类似：

```text
desk/<desktop-device-id>/state/codex
```

如果接口返回 `503` 或提示没有 matching online board，先查 broker/bridge 是否在跑、
A7Z 是否连同一个 `MQTT_URL`，以及 Windows 防火墙是否挡住了 `1883`。

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
MQTT_URL="mqtt://<pc-lan-ip>:1883" \
HOST=radxa@<board-ip> \
SUDO_PASSWORD=<sudo-password> \
CONFIGURE_SPI_LCD=1 \
sh scripts/deploy-radxa-a733.sh
```

Windows PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -MqttUrl mqtt://<pc-lan-ip>:1883 `
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
| `-MqttUrl` | `mqtt://broker.openclaw.example:1883` | runtime 使用的 MQTT broker；本地复刻建议传 `mqtt://<pc-lan-ip>:1883` |
| `-ConfigureSpiLcd` | 关闭 | 部署后写入 A7Z SPI LCD overlay |
| `-LcdDriver` | `ili9341` | LCD framebuffer 驱动 |
| `-LcdDcPin` | `15` | A7Z physical pin 15 / PL7 |
| `-LcdResetPin` | `13` | A7Z physical pin 13 / PL6 |
| `-LcdBacklightPin` | `0` | `0` 表示 BLK 直连 3.3V，不由 GPIO 控制 |
| `-LcdSpeedHz` | `16000000` | SPI LCD 频率 |
| `-LcdRotate` | `270` | 屏幕旋转角度 |

Radxa shell 脚本：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `HOST` | 必填，例如 `radxa@<board-ip>` | SSH 目标 |
| `REMOTE_DIR` | `/opt/board-runtime` | A7Z 上运行目录 |
| `SRC_DIR` | `/opt/board-runtime-src` | A7Z 上源码构建目录 |
| `SUDO_PASSWORD` | 空 | sudo 需要密码时传入 |
| `MQTT_URL` | `mqtt://broker.openclaw.example:1883` | runtime 使用的 MQTT broker；本地复刻建议传 `mqtt://<pc-lan-ip>:1883` |
| `CONFIGURE_SPI_LCD` | `0` | 设为 `1` 时写入 A7Z SPI LCD overlay |
| `LCD_DRIVER` | `ili9341` | LCD framebuffer 驱动 |
| `LCD_DC_PIN` | `15` | A7Z physical pin 15 / PL7 |
| `LCD_RESET_PIN` | `13` | A7Z physical pin 13 / PL6 |
| `LCD_BACKLIGHT_PIN` | `0` | `0` 表示 BLK 直连 3.3V，不由 GPIO 控制 |
| `LCD_SPEED_HZ` | `16000000` | SPI LCD 频率 |
| `LCD_ROTATE` | `270` | 屏幕旋转角度 |

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
dtoverlay=fbtft,spi0-0,rpi-display,reset_pin=27,dc_pin=22,led_pin=12,speed=32000000,rotate=90,fps=60
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
  -Rotate 270 `
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
  -Rotate 270 \
  -Reboot
```

`configure-radxa-a733-spi-lcd.ps1` defaults to the verified A7Z settings:
ILI9341, SPI1 CS0, `RES=PL6` active-low, `DC=PL7`, no backlight GPIO,
16 MHz, rotate 270. Use `-ResetActiveHigh` only for a different display
module that is known to need high-active reset.

The Radxa deployment script can also configure this overlay in one run:

macOS/Linux/WSL/Git Bash：

```sh
MQTT_URL="mqtt://<pc-lan-ip>:1883" \
HOST=radxa@<board-ip> \
SUDO_PASSWORD=<sudo-password> \
CONFIGURE_SPI_LCD=1 \
sh scripts/deploy-radxa-a733.sh
```

Windows PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -MqttUrl mqtt://<pc-lan-ip>:1883 `
  -ConfigureSpiLcd
```

macOS/Linux PowerShell 7 也可运行 PowerShell 版本：

```sh
pwsh -NoProfile -ExecutionPolicy Bypass -File ./scripts/deploy-radxa-a733.ps1 \
  -HostName radxa@<board-ip> \
  -SudoPassword <sudo-password> \
  -MqttUrl mqtt://<pc-lan-ip>:1883 \
  -ConfigureSpiLcd
```

After reboot, verify:

```sh
ssh radxa@<board-ip> 'cat /proc/fb'
ssh radxa@<board-ip> 'dmesg | grep -E "fb_ili9341|graphics fb0" | tail'
```

## 故障排查

### A7Z 首次部署常见卡点

| 现象 | 常见原因 | 处理方法 |
|---|---|---|
| 资料里出现 T113、MangoPi、PhoenixCard | 看到了旧设备/旧脚本资料，和当前 A7Z 主线混用 | A7Z 只使用官方 A733/Cubie A7Z Debian GPT 镜像 + Balena Etcher；不要拖拽固件，也不要用 PhoenixCard |
| 电脑看到 `VID_1f3a_PID_efe8` / FEL 类设备 | 板子进了 Allwinner USB/FEL/烧录模式，通常不是正常 Linux runtime 状态 | 先确认 SD 卡是 A7Z 官方镜像、卡槽接触、BOOT/FEL 按键或拨码；不要把开发板当读卡器隔空写 SD 卡 |
| 烧卡后无法 SSH | A7Z 还没有写入系统 Wi-Fi，或电脑和 A7Z 不在同一局域网 | 先用 HDMI+键盘或串口登录 A7Z，执行 `sudo nmcli dev wifi connect "<wifi-ssid>" password "<wifi-password>"`；再从路由器 DHCP、板载屏幕、串口日志或 `arp -a` 找 `<board-ip>` |
| `start-hachimo-link.ps1` 找不到模块或启动后 1883 没监听 | 全新环境还没安装 bridge 依赖，或命令不在仓库根目录执行 | 在 `ref/` 和 `ref/src-tauri/bridge/packages/clawd-backend-service/` 分别运行 `npm install`；回仓库根目录再运行脚本 |
| A7Z 连不上 `mqtt://<pc-lan-ip>:1883` | 电脑 IP 选错、Windows 防火墙拦 Node.js、电脑和板子不在同一 LAN | 用 `Get-NetTCPConnection` 确认 1883 监听；允许 Node.js 访问可信局域网；从 A7Z 用 Python socket 检查 `<pc-lan-ip>:1883` |
| 只打开了 `npm run dev:web` 页面，但设备状态不动 | Vite Web 只是前端页面，bridge/本地 agent 状态发布没有完整启动 | 用 `npm run dev` 启动完整 Tauri 应用；或用 `start-hachimo-link.ps1` 启动 headless broker + bridge |
| 电脑 USB 枚举异常，manager 显示 USB 未连接 | A7Z 标准路线不依赖 Raspberry Pi USB gadget `/dev/ttyGS0` | 不要把 USB 未连接当成 A7Z 部署失败；优先验证 Wi-Fi、HTTP、MQTT 和 remote binding |
| 屏幕完全不亮 | 供电、GND、BLK、排线方向或屏幕模块供电问题 | 先确认板子能 SSH；背光不亮优先查 BLK/3.3V/GND/排线，不要先重烧系统 |
| 屏幕亮白屏 | SPI LCD overlay、RES/DC/CS 接线、中间 PCB、排线接触问题 | 查 `/proc/fb`、`dmesg`、`fb_ili9341`；确认 `CS=PD10`、`RES=PL6`、`DC=PL7`、BLK 直连 3.3V；必要时绕过中间 PCB 或重新压屏幕排线 |
| 屏幕显示 Debian login 控制台，不显示宠物 | framebuffer 通了，但 `fb-display.sh` 没有接管屏幕 | 查 `sudo journalctl -u board-runtime -n 120 --no-pager`；如出现 stale lock，删除 `/opt/board-runtime/.fb-display.lock` 并重启 `board-runtime` |
| 日志里出现 `set: Illegal option -` | Windows CRLF 行尾被同步到 Linux shell 脚本 | 把脚本转成 LF 后重新部署；不要手工用会改行尾的方式覆盖 `/opt/board-runtime/*.sh` |
| 画面卡顿 | SPI 小屏带宽有限，FPS 配置过高，或 SPI 频率过低 | A7Z 默认 LCD 频率用 16 MHz；可把 `PET_CLAW_FB_FFMPEG_OUTPUT_FPS` 调到 12-15 之间试稳定性 |
| binding 接口返回 `503` 或 no matching board | bridge 还没看到 A7Z online/hello，或板子不在同一个 broker | 先确认 broker/bridge 在跑，再确认 `network-config.json` 的 `mqttUrl` 和脚本输出的 `MQTT URL` 一致，重启 `board-runtime` |
| 重启板子后 manager 又连不上 | 远程绑定没有持久化，或 board runtime 没重新订阅当前 broker | 确认 `/opt/board-runtime/network-config.json` 中有正确 `mqttUrl`、`desktopDeviceId` / `targetDeviceId`，并重启 `board-runtime` |
| 重启电脑后又连不上 | 电脑换了 Wi-Fi 或 LAN IP 变化，A7Z 仍连旧 MQTT URL | 让电脑回到同一局域网；运行 `scripts/start-hachimo-link.ps1 -UpdateBoard -BoardHost radxa@<board-ip>`，长期建议在路由器做 DHCP 固定 IP |
| MQTT broker 默认域名不可用 | `broker.openclaw.example` 是占位默认值，不是本地可用 broker | 开发/复刻时启动本地 broker + bridge，并给部署脚本传 `mqtt://<pc-lan-ip>:1883` |
| 读卡器显示 `No Media` | SD 卡没插实、读卡器状态没刷新，或卡/读卡器接触不良 | 拔插 SD 卡和读卡器；确认系统看到 16GB/实际容量的可移动盘后再写卡 |

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
