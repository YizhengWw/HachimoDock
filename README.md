# claw-pet-manager

`claw-pet-manager` 是 Pet Manager 的单仓库工作区，包含桌面端管理器
和小屏设备端运行时。

这个仓库当前分成两条主线：

- `ref/`：Tauri 2 + React 桌面端，用于设备绑定、桌宠形象管理、组件中心、
  agent 跟随、语音/按钮配置、USB/MQTT 下发和本地 bridge sidecar 管理。
- `board-runtime/`：Raspberry Pi / Radxa Cubie A7Z 设备端 runtime，用于显示
  宠物动画、接收桌面端 session 状态、处理输入、运行负一屏 widget 和配网页面。

## 目录结构

| 路径 | 说明 |
|---|---|
| `ref/` | 桌面端 Pet Manager。React UI 在 `src/`，Tauri/Rust 后端在 `src-tauri/`。 |
| `board-runtime/` | 设备端 runtime。C 主服务 + shell/Python 辅助进程，部署到 Raspberry Pi 或 Radxa Cubie A7Z。 |
| `scripts/` | 仓库级脚本，例如内置 `.clawpkg` 打包。 |
| `skills/` | 组件中心使用的 agent skill，包含 `.clawpkg` 生成流程。 |
| `docs/` | 架构说明、历史计划、语音链路、打包说明等文档。 |
| `AGENTS.md` / `CLAUDE.md` | agent 工作入口和协作约束。 |

## 桌面端开发

Windows 用户可以在仓库根目录双击 `open-manager.bat`。首次运行时，它会在需要时安装桌面端依赖、构建独立的 Tauri release 应用并打开；之后会直接打开已构建的 release exe，不需要先启动 `localhost:4173` 或执行 `npm run dev:web`。

```sh
cd ref
npm install
npm run dev
```

常用命令：

```sh
cd ref
npm test
npm run build:web
npm run build
```

`npm run dev` 会启动 Tauri 桌面应用；只调前端页面时可用：

```sh
cd ref
npm run dev:web
```

更多桌面端说明见 [ref/README.md](ref/README.md)。

## 设备端开发和部署

首次复刻到默认硬件 Radxa Cubie A7Z 时，先读
[board-runtime/DEPLOY.md](board-runtime/DEPLOY.md)。这篇是当前标准部署入口，
覆盖从空白 microSD 卡写系统、首次写入 Wi-Fi、SSH 部署、SPI 屏幕显示、
本地 MQTT/bridge 到 Pet Manager 绑定的完整流程。A7Z 系统镜像默认选择
Radxa 官方 GPT/A733 Unified 的 Debian 11 KDE R6（或未来更新的最新正式 `r*`
SD/eMMC KDE/Desktop release）；不要只复制下面的摘要命令。

本机快速编译 C 代码：

```sh
cd board-runtime
cmake -S . -B /tmp/board-runtime-build-check
cmake --build /tmp/board-runtime-build-check --target board-server
```

部署到 Raspberry Pi：

```sh
cd board-runtime
export BOARD_HOST="<pi-user>@<pi-ip>"
HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh
```

部署到 Radxa Cubie A7Z（macOS/Linux/WSL/Git Bash）：

```sh
cd board-runtime
MQTT_URL="mqtt://<pc-lan-ip>:1883" \
HOST=radxa@<board-ip> \
SUDO_PASSWORD=<sudo-password> \
CONFIGURE_SPI_LCD=1 \
sh scripts/deploy-radxa-a733.sh
```

部署到 Radxa Cubie A7Z（Windows PowerShell）：

```powershell
cd board-runtime
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -MqttUrl mqtt://<pc-lan-ip>:1883 `
  -ConfigureSpiLcd
```

设备 IP、用户和密码不要写死在文档或业务代码里。不同板子使用不同的
`BOARD_HOST` / `BOARD_IP` 即可；Radxa A7Z 首次显示、刷写系统、Wi-Fi、MQTT
和 SPI 屏幕 overlay 说明见
[board-runtime/DEPLOY.md](board-runtime/DEPLOY.md)。

更多设备端说明见 [board-runtime/README.md](board-runtime/README.md) 和
[board-runtime/DEPLOY.md](board-runtime/DEPLOY.md)。
安全部署与鉴权说明见
[board-runtime/docs/security-hardening.md](board-runtime/docs/security-hardening.md)。

## License

Copyright (C) 2026 wangwu50.

This project is licensed under the GNU General Public License version 3 only
(`GPL-3.0-only`). See [LICENSE](LICENSE) for the full license text and
[COPYRIGHT](COPYRIGHT) for the copyright notice. Open-source review notes are
tracked in [docs/open-source-compliance-prep.md](docs/open-source-compliance-prep.md).

## 大体链路

1. 桌面端 `ref/` 发现并管理本机 agent 通道（Codex、Claude Code 等）。
2. Tauri 后端和 bridge sidecar 读取 agent session JSONL，归一化状态、字幕和 token 数据。
3. 桌面端把当前选中 agent 的状态通过 USB serial 或 MQTT 下发给设备。
4. `board-runtime/` 的 `board-server` 接收状态，写入 `.current-state`、
   `.current-speech`、`.stats-display` 等本地文件。
5. 设备端显示进程、输入进程和 widget runtime 通过这些点文件协作，完成宠物动画、
   负一屏和按钮/触屏交互。

## 验证

桌面端测试：

```sh
cd ref
npm test
```

设备端 host 构建：

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

## 文档同步约定

- 改桌面端功能时，同步更新 `ref/.folder.md`、相关 README 或设计文档。
- 改设备端功能时，同步更新 `board-runtime/README.md`、`board-runtime/DEPLOY.md`
  或 `board-runtime/docs/*`。
- 跨端改动需要同时更新两边说明，避免桌面端和设备端契约漂移。
