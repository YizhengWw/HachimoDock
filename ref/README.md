# [HachimoDock（哈基米机）](https://github.com/YizhengWw/HachimoDock) Desktop (`ref/`)

`ref/` 是 HachimoDock（哈基米机）桌面端应用。它使用 Tauri 2 + React + Vite：

- `src/`：React UI，包括设备向导、设备仪表盘、宠物相册、组件中心、语音控制和
  当前展示配置。
- `src-tauri/`：Tauri/Rust 后端，负责本地文件、bridge 进程、USB/MQTT 下发、
  agent session 监听、组件安装和系统能力调用。
- `builtin-clawpkgs/`：内置负一屏组件源目录。
- `public/`：桌面端静态资源、内置形象素材和帮助图。

## 开发启动

```sh
cd ref
npm install
npm run dev
```

只启动 Web UI：

```sh
cd ref
npm run dev:web
```

## 构建

```sh
cd ref
npm run build:web
npm run build
```

`npm run build` 会走 Tauri 桌面应用构建。平台安装包说明见
[../docs/desktop-packaging.md](../docs/desktop-packaging.md)。

## 测试

```sh
cd ref
npm test
```

当前测试使用 Node 内置 test runner 扫描 `src/**/*.test.js`。新增 UI 逻辑时优先补
静态/单元测试，覆盖状态映射、组件组合、下发 payload 和用户流程关键分支。

## 大体框架

| 区域 | 说明 |
|---|---|
| `src/App.jsx` | 桌面端主壳，组织侧边栏和主要页面。 |
| `src/DeviceDashboard.jsx` / `src/dashboard/*` | 设备详情、当前显示、按钮/语音配置、连接状态。 |
| `src/ComponentCenter.jsx` / `src/components/*` | 负一屏组件中心，内置组件、生成组件、安装/替换/删除。 |
| `src/lib/*` | 共享业务逻辑，如设备绑定、形象分配、组件契约、agent prompt。 |
| `src-tauri/src/lib.rs` | Tauri 命令入口，负责桥接本机能力和设备下发。 |
| `src-tauri/bridge/*` | 本地 bridge sidecar 和 agent session 监听相关代码。 |

## 设备通信

桌面端可以通过两条路径和设备端通信，具体取决于硬件方案：

- USB serial：主要用于 Raspberry Pi USB gadget 直连、按钮配置、`.clawpkg`
  安装和状态同步。
- MQTT：用于无线可达性、远程绑定和状态/语音同步；当前 Radxa Cubie A7Z
  默认走 Wi-Fi + MQTT/SSH。

设备目标不要写死 IP。调试具体板子时使用：

```sh
export BOARD_HOST="<pi-user>@<pi-ip>"
export BOARD_IP="<pi-ip>"
```

## 组件中心

组件以 `.clawpkg` 为安装单元，核心契约包括：

- `component.json`
- `buttons.json`
- `negative-screen.json`
- `share.json`
- `runtime/widget.json`

内置组件可通过仓库级脚本打包：

```sh
cd ref
npm run pack-builtins
```

## 开发注意

- 桌面端 UI 变更同步更新相关 README、设计文档或当前仍纳入版本管理的目录说明。
- Tauri 命令或 bridge 契约变更需要检查 `board-runtime/` 是否也要同步。
- 不要在业务代码里写死某一块设备的 IP、用户或 board id。
- selected agent、语音、USB active-state 下发要保持“只跟随当前选择 agent”的语义。
