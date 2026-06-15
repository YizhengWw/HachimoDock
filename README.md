<div align="center">
  <img src="ref/src/assets/logo/hachimodock-mark.png" width="112" alt="HachimoDock（哈基米机）logo" />
  <h1>HachimoDock（哈基米机）</h1>
  <p><strong>放在桌上的 Agent 小屏</strong></p>
  <p>
    把 Codex、Claude Code、OpenClaw 等 CLI Agent 的状态变成桌上小宠物的表情、动作、字幕和提醒。
  </p>
  <p>
    <a href="#快速开始">快速开始</a>
    · <a href="#硬件方案">硬件方案</a>
    · <a href="#硬件端资料">硬件端资料</a>
    · <a href="#图集">图集</a>
    · <a href="#软件架构">软件架构</a>
    · <a href="#复刻与部署">复刻与部署</a>
    · <a href="#常见问题">常见问题</a>
  </p>
  <p>
    <img alt="GPL-3.0-only" src="https://img.shields.io/badge/license-GPL--3.0--only-2a2620" />
    <img alt="Desktop" src="https://img.shields.io/badge/desktop-macOS%20%7C%20Windows-db3b2b" />
    <img alt="Runtime" src="https://img.shields.io/badge/runtime-Radxa%20A7Z%20%7C%20Raspberry%20Pi-e8a23a" />
  </p>
  <p>
    <a href="https://github.com/YizhengWw/HachimoDock/stargazers"><img alt="GitHub stars" src="https://img.shields.io/github/stars/YizhengWw/HachimoDock?style=social" /></a>
    <a href="https://github.com/YizhengWw"><img alt="Follow @YizhengWw" src="https://img.shields.io/github/followers/YizhengWw?label=Follow%20%40YizhengWw&style=social" /></a>
  </p>
  <p>
    <a href="https://oshwhub.com/eda_gqvzlprk/project_cnbmkbjc#3-%E7%A1%AC%E4%BB%B6%E5%A4%8D%E5%88%BB"><strong>硬件端资料 / PCB / BOM / 复刻教程</strong></a>
  </p>
</div>

![HachimoDock（哈基米机）产品图](docs/assets/readme/product-hero.webp)

## 项目简介

HachimoDock（哈基米机）是一套桌面端管理器 + 小屏设备端固件/运行时 + 开源硬件方案。它把电脑里正在运行的 Agent 状态同步到桌面小屏上，让 AI 工作状态从终端窗口里走出来，变成一只可以抬头看见、可以触摸互动、可以语音唤起的小搭子。

这个 GitHub 仓库主要托管软件端和固件端代码；硬件端资料在 OSHWHub 维护。

| 端 | 入口 | 说明 |
|---|---|---|
| 软件端 | [`ref/`](ref/) | Tauri 2 + React 桌面端。负责设备绑定、Agent 检测与跟随、形象管理、组件中心、语音入口、按钮配置、USB/MQTT 下发和本地 bridge sidecar。 |
| 固件端 / 设备运行时 | [`board-runtime/`](board-runtime/) | Raspberry Pi / Radxa Cubie A7Z 设备端运行时。负责显示宠物动画、接收桌面端状态、处理输入、运行负一屏 widget 和配网页面。 |
| 硬件端 | [OSHWHub 硬件复刻页面](https://oshwhub.com/eda_gqvzlprk/project_cnbmkbjc#3-%E7%A1%AC%E4%BB%B6%E5%A4%8D%E5%88%BB) | PCB、BOM、结构/装配和硬件复刻资料。 |

## 图集

<table>
  <tr>
    <td width="50%">
      <img src="docs/assets/readme/live-photo.webp" alt="HachimoDock（哈基米机）实拍图" />
      <br />
      <sub>桌面小屏实拍：把 Agent 状态放到工位视野里。</sub>
    </td>
    <td width="50%">
      <img src="docs/assets/readme/desktop-ui.webp" alt="HachimoDock（哈基米机）桌面端界面" />
      <br />
      <sub>桌面端管理：设备绑定、Agent 跟随、形象和组件配置。</sub>
    </td>
  </tr>
</table>

## 核心亮点

| 能力 | 说明 |
|---|---|
| Agent 状态跟随 | Agent 思考、执行工具、等待确认、完成或报错时，设备屏会显示对应表情、动作、颜色和短标签。 |
| 桌面小屏常驻 | 不用切窗口，抬头就能看到当前 Agent 是否还在工作、是否需要用户决策。 |
| 自定义宠物形象 | 内置西高地小狗状态动画，也可以导入或生成自己的宠物形象。 |
| 负一屏组件 | 内置摸鱼倒计时、番茄钟、喝水提醒、Token 消耗等 `.clawpkg` 组件，并支持自然语言生成新组件。 |
| 多链路通信 | Raspberry Pi 方案支持 USB gadget 直连；Radxa A7Z 方案当前默认走 Wi-Fi + MQTT/SSH。 |
| 语音与实体交互 | Raspberry Pi 方案已验证触摸、旋钮/按钮和语音链路；Radxa A7Z 方案保留硬件与软件扩展位。 |

### 状态跟随与互动

<table>
  <tr>
    <td width="34%">
      <img src="docs/assets/readme/status-follow.gif" alt="HachimoDock（哈基米机）状态跟随动图" />
      <br />
      <sub>Agent 状态变化时，小屏同步切换表情和动作。</sub>
    </td>
    <td width="33%">
      <img src="docs/assets/readme/status-example-1.webp" alt="HachimoDock（哈基米机）状态示例一" />
      <br />
      <sub>一眼识别当前状态，不必盯着终端窗口。</sub>
    </td>
    <td width="33%">
      <img src="docs/assets/readme/status-example-2.webp" alt="HachimoDock（哈基米机）状态示例二" />
      <br />
      <sub>字幕和短标签提示 Agent 正在做什么。</sub>
    </td>
  </tr>
</table>

### 自定义形象与组件

<table>
  <tr>
    <td width="50%">
      <img src="docs/assets/readme/custom-avatar.webp" alt="HachimoDock（哈基米机）自定义形象" />
      <br />
      <sub>上传宠物照片、头像或原创角色，生成自己的桌面搭子。</sub>
    </td>
    <td width="50%">
      <img src="docs/assets/readme/component-example.webp" alt="HachimoDock（哈基米机）组件示例" />
      <br />
      <sub>负一屏组件承载番茄钟、喝水提醒、Token 消耗等轻量工具。</sub>
    </td>
  </tr>
</table>

## 硬件方案

本仓库给出软件和固件配套；PCB、BOM、硬件装配和复刻资料请看：
[OSHWHub 硬件端页面](https://oshwhub.com/eda_gqvzlprk/project_cnbmkbjc#3-%E7%A1%AC%E4%BB%B6%E5%A4%8D%E5%88%BB)。

| 方案 | 推荐用途 | 当前已验证 | 当前未默认启用 |
|---|---|---|---|
| 方案一：Radxa Cubie A7Z | 默认复刻硬件，性能更高，适合走 Wi-Fi + MQTT/SSH 部署 | Debian 11/12、SPI ILI9341 LCD、framebuffer 显示、HTTP/MQTT、负一屏 widget、桌面端状态同步 | XPT2046/PEN 触摸 overlay、GPIO 旋钮/按钮、板端语音 PTT、USB gadget `/dev/ttyGS0` |
| 方案二：Raspberry Pi Zero 2 W | 兼容方案，适合完整体验触摸、旋钮、语音和 USB 直连 | Raspberry Pi OS、SPI ILI9341 LCD、XPT2046/ADS7846 触摸、GPIO 旋钮/按钮、VoiceHAT 语音、USB gadget、HTTP/MQTT、负一屏 widget、桌面端状态同步 | 无线和音频效果仍取决于实际镜像、声卡和网络配置 |

`ESP32` 不是当前 `board-runtime/` 已支持目标；如需使用，需要另起移植工程。

<table>
  <tr>
    <td width="50%">
      <img src="docs/assets/readme/hardware-bom.webp" alt="HachimoDock（哈基米机）硬件 BOM 平铺图" />
      <br />
      <sub>方案一 Radxa A7Z 的核心物料示意。</sub>
    </td>
    <td width="50%">
      <img src="docs/assets/readme/usage-ui.webp" alt="HachimoDock（哈基米机）使用界面" />
      <br />
      <sub>桌面端负责绑定、跟随、形象、组件和诊断。</sub>
    </td>
  </tr>
</table>

## 硬件端资料

硬件端不只是一张接线图，包含从主控、屏幕、PCB、外壳到装配的复刻路径：

| 内容 | 入口 |
|---|---|
| 硬件复刻总入口 | [OSHWHub 项目页](https://oshwhub.com/eda_gqvzlprk/project_cnbmkbjc#3-%E7%A1%AC%E4%BB%B6%E5%A4%8D%E5%88%BB) |
| 软件端和固件端 | 本 GitHub 仓库 |
| 桌面端开发说明 | [ref/README.md](ref/README.md) |
| 设备端部署说明 | [board-runtime/DEPLOY.md](board-runtime/DEPLOY.md) |

## 软件架构

```mermaid
flowchart LR
  agent["CLI Agent<br/>Codex / Claude Code / OpenClaw"]
  desktop["HachimoDock（哈基米机）桌面端<br/>Tauri + React"]
  bridge["Bridge sidecar<br/>session / MQTT / USB"]
  board["board-runtime<br/>C service + shell/Python helpers"]
  screen["桌面小屏<br/>宠物动画 / 字幕 / 负一屏组件"]

  agent --> desktop
  desktop --> bridge
  bridge -->|"USB serial 或 MQTT"| board
  board --> screen
```

桌面端读取 Agent session、归一化状态和字幕，再通过 USB serial 或 MQTT 下发到设备端。设备端 `board-server` 写入 `.current-state`、`.current-speech`、`.stats-display` 等本地状态文件，显示进程、输入进程和 widget runtime 通过这些文件协作。

## 快速开始

### 启动桌面端

```sh
git clone https://github.com/YizhengWw/HachimoDock.git
cd HachimoDock/ref
npm install
npm run dev
```

只调试前端页面时：

```sh
cd ref
npm run dev:web
```

构建桌面应用：

```sh
cd ref
npm run build
```

更多说明见 [ref/README.md](ref/README.md)。

### 编译设备端

```sh
cd board-runtime
cmake -S . -B /tmp/board-runtime-build-check
cmake --build /tmp/board-runtime-build-check --target board-server
```

更多说明见 [board-runtime/README.md](board-runtime/README.md)。

## 复刻与部署

### Raspberry Pi

```sh
cd board-runtime
export BOARD_HOST="<pi-user>@<pi-ip>"
HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh
```

### Radxa Cubie A7Z

macOS / Linux / WSL / Git Bash：

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

设备 IP、用户名、密码、board id 和 desktop id 不要写死在文档或业务代码里。调试具体板子时使用 `BOARD_HOST="<pi-user>@<pi-ip>"` 和 `BOARD_IP="<pi-ip>"`。

部署细节见 [board-runtime/DEPLOY.md](board-runtime/DEPLOY.md)，安全部署与鉴权见 [board-runtime/docs/security-hardening.md](board-runtime/docs/security-hardening.md)。

## 常见问题

### 这个 GitHub 仓库包含什么？

包含软件端和固件端：

- 软件端：`ref/`，也就是 HachimoDock（哈基米机）桌面管理器。
- 固件端 / 设备运行时：`board-runtime/`，部署到 Raspberry Pi 或 Radxa Cubie A7Z。
- 硬件端：PCB、BOM、装配和复刻资料在 [OSHWHub 硬件端页面](https://oshwhub.com/eda_gqvzlprk/project_cnbmkbjc#3-%E7%A1%AC%E4%BB%B6%E5%A4%8D%E5%88%BB)。

### 一定要用 Radxa Cubie A7Z 吗？

不是。当前文档同时照顾 Radxa Cubie A7Z 和 Raspberry Pi Zero 2 W。A7Z 是默认复刻硬件，性能更高；Pi 方案更适合完整体验 USB gadget、触摸、旋钮/按钮和语音链路。

### 屏幕、麦克风、喇叭和旋钮能换型号吗？

可以，但需要确认接口和驱动。屏幕建议保持 2.8 英寸 240×320 SPI、ILI9341；Raspberry Pi 方案已验证 XPT2046/ADS7846 触摸和 EC11/按钮。Radxa A7Z 方案当前默认验证 LCD 显示，触摸、旋钮和语音需要后续 overlay、GPIO 和声卡适配。

### 屏幕一定要带触摸吗？

不是必须。无触摸版本也能用于状态显示；带 XPT2046 触摸屏可以体验触摸反馈和更多交互。

### 必须做定制 PCB 底板吗？

不是必须。定制 PCB 主要是为了减少杜邦线、让结构更整洁。调试阶段可以先用杜邦线直连；想做成完整产品形态时再按 OSHWHub 的硬件资料打样。

### 大概成本多少？

方案一 Radxa A7Z 的电子料大约在几百元级别，具体取决于主控、屏幕、音频模块、外壳和打样方式。方案二 Raspberry Pi Zero 2 W 的成本受主板供货、声卡和转接件影响更大。

### 支持哪些 Agent？

面向 CLI Agent 设计，内置适配 Codex、Claude Code、OpenClaw 等。状态协议开放，第三方 Agent 也可以接入。

### 设备一定要联网吗？

取决于方案。Raspberry Pi 可走 USB gadget 直连；Radxa A7Z 当前默认走 Wi-Fi + MQTT/SSH，需要网络。

### 语音交互怎么用？

当前完整验证的是 Raspberry Pi 方案：按住按钮说话，板端把语音转成文本后通过输入事件送到桌面端，再注入目标 Agent。语音识别链路需要可用网络和 STT 服务。Radxa A7Z 方案当前默认不启动板端语音 PTT。

### 能自己加组件吗？

可以。组件中心使用 `.clawpkg` 结构，内置 skill 可以根据自然语言生成负一屏组件，并在 USB 或 SSH/MQTT 链路可用时下发到设备。

### 用什么开源协议？

软件代码按 GPL-3.0-only 发布；硬件设计、3D 结构件、官方宠物素材和第三方资源请以仓库内 `LICENSE`、`COPYRIGHT`、`docs/open-source-compliance-prep.md` 以及对应资源随附声明为准。

### 怎么反馈问题或参与贡献？

可以在 GitHub 提 Issue、Discussion 或 PR。硬件复刻相关问题建议同时参考 [OSHWHub 硬件端页面](https://oshwhub.com/eda_gqvzlprk/project_cnbmkbjc#3-%E7%A1%AC%E4%BB%B6%E5%A4%8D%E5%88%BB)。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/developer-setup_zh_Hans.md](docs/developer-setup_zh_Hans.md) | 从零搭建桌面端和设备端开发环境。 |
| [ref/README.md](ref/README.md) | 桌面端结构、开发命令和通信说明。 |
| [board-runtime/README.md](board-runtime/README.md) | 设备端模块、构建、部署和调试入口。 |
| [board-runtime/DEPLOY.md](board-runtime/DEPLOY.md) | Raspberry Pi / Radxa A7Z 部署细节。 |
| [docs/voice-architecture.md](docs/voice-architecture.md) | 桌面端、Agent bus 和板端语音链路设计。 |
| [docs/open-source-compliance-prep.md](docs/open-source-compliance-prep.md) | 开源合规与第三方资源检查记录。 |

## 验证

```sh
cd ref
npm test
npm run build
```

```sh
cd board-runtime
cmake -S . -B /tmp/board-runtime-build-check
cmake --build /tmp/board-runtime-build-check --target board-server
```

设备端部署后检查：

```sh
export BOARD_HOST="<board-user>@<board-ip>"
export BOARD_IP="<board-ip>"
ssh "$BOARD_HOST" 'systemctl is-active board-runtime'
curl -fsS http://$BOARD_IP/board-runtime-config.json
```

## 作者 / Author

HachimoDock 由 **Yizheng Wang**（[@YizhengWw](https://github.com/YizhengWw)）与 **Wenchao Wang** 共同设计与开发。

- 📧 邮箱：skyler.wang98@gmail.com
- 💬 微信交流群：扫描下方二维码加入

<img src="docs/assets/readme/group-qrcode.png" width="180" alt="HachimoDock 微信交流群二维码" />

如果 HachimoDock 对你有帮助，欢迎给仓库点一个 ⭐ **Star**、点击 **Follow** 关注后续更新，也欢迎提 Issue / PR 一起把它做得更好。

## License

Copyright (C) 2026 Yizheng Wang.

This project is licensed under the GNU General Public License version 3 only (`GPL-3.0-only`). See [LICENSE](LICENSE) for the full license text and [COPYRIGHT](COPYRIGHT) for the copyright notice.
