# [HachimoDock（哈基米机）](https://github.com/YizhengWw/HachimoDock)

[HachimoDock（哈基米机）](https://github.com/YizhengWw/HachimoDock) 是单仓库工作区，包含桌面端管理器
和小屏设备端运行时。

这个仓库当前分成两条主线：

- `ref/`：Tauri 2 + React 桌面端，用于设备绑定、桌宠形象管理、组件中心、
  agent 跟随、语音/按钮配置、USB/MQTT 下发和本地 bridge sidecar 管理。
- `board-runtime/`：Raspberry Pi / Radxa Cubie A7Z 设备端 runtime，用于显示
  宠物动画、接收桌面端 session 状态、处理输入、运行负一屏 widget 和配网页面。
  两种硬件方案的能力边界不同，见下方支持矩阵。

## 硬件方案和当前支持范围

| 方案 | 推荐用途 | 当前已验证 | 当前未默认启用 |
|---|---|---|---|
| 方案一：Radxa Cubie A7Z | 默认复刻硬件，性能更高，适合走 Wi-Fi + MQTT/SSH 部署 | Debian 11/12、SPI ILI9341 LCD、framebuffer 显示、HTTP/MQTT、负一屏 widget、桌面端状态同步 | XPT2046/PEN 触摸 overlay、GPIO 旋钮/按钮、板端语音 PTT、USB gadget `/dev/ttyGS0` |
| 方案二：Raspberry Pi Zero 2 W | 兼容方案，适合完整体验触摸、旋钮、语音和 USB 直连 | Raspberry Pi OS、SPI ILI9341 LCD、XPT2046/ADS7846 触摸、GPIO 旋钮/按钮、VoiceHAT 语音、USB gadget、HTTP/MQTT、负一屏 widget、桌面端状态同步 | 无线和音频效果仍取决于实际镜像、声卡和网络配置 |

`ESP32` 不属于当前 `board-runtime/` 已支持目标；如要使用，需要另起移植工程。

## 目录结构

| 路径 | 说明 |
|---|---|
| `ref/` | 桌面端 HachimoDock（哈基米机）。React UI 在 `src/`，Tauri/Rust 后端在 `src-tauri/`。 |
| `board-runtime/` | 设备端 runtime。C 主服务 + shell/Python 辅助进程，部署到 Raspberry Pi 或 Radxa Cubie A7Z。 |
| `scripts/` | 仓库级脚本，例如内置 `.clawpkg` 打包。 |
| `skills/` | 组件中心使用的 agent skill，包含 `.clawpkg` 生成流程。 |
| `docs/` | 架构说明、历史计划、语音链路、打包说明等文档。 |
| `AGENTS.md` / `CLAUDE.md` | agent 工作入口和协作约束。 |

## 桌面端开发

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
HOST=radxa@<board-ip> SUDO_PASSWORD=<sudo-password> CONFIGURE_SPI_LCD=1 sh scripts/deploy-radxa-a733.sh
```

部署到 Radxa Cubie A7Z（Windows PowerShell）：

```powershell
cd board-runtime
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\deploy-radxa-a733.ps1 `
  -HostName radxa@<board-ip> `
  -SudoPassword <sudo-password> `
  -ConfigureSpiLcd
```

设备 IP、用户和密码不要写死在文档或业务代码里。不同板子使用不同的
`BOARD_HOST` / `BOARD_IP` 即可；Radxa A7Z 刷写系统和 SPI 屏幕 overlay 说明见
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

1. 桌面端 `ref/` 发现并管理本机 agent 通道（Codex、Claude Code、OpenClaw 等）。
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

- 改桌面端功能时，同步更新相关 README、设计文档或当前仍纳入版本管理的目录说明。
- 改设备端功能时，同步更新 `board-runtime/README.md`、`board-runtime/DEPLOY.md`
  或 `board-runtime/docs/*`。
- 跨端改动需要同时更新两边说明，避免桌面端和设备端契约漂移。
