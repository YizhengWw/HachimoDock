# PC 客户端 / Desktop client

Pet Manager 用于连接设备、管理形象和组件，以及将 Agent 状态与语音输入同步到桌面小屏。支持 macOS arm64 与 Windows x64。

直接使用请前往 [下载页面](https://github.com/YizhengWw/HachimoDock/releases/latest)。以下内容面向需要自行编译或修改客户端的开发者，技术栈为 Tauri 2 + React。

## 环境与测试 / Requirements and tests

安装 Git LFS、Node.js 22+、Rust stable，以及 Tauri 对应平台的系统依赖。Windows 需要 MSVC C++ Build Tools 和 WebView2；macOS 需要 Xcode Command Line Tools。

从仓库根目录运行 / From the repository root:

```sh
git lfs pull
cd pc
npm ci
npm test
npm run test:bridge
npm run build:web
```

`npm run dev:web` 只预览网页 UI，不提供设备、系统权限及本机 Agent 功能。完整客户端使用 `npm run dev`，先按下文准备运行时依赖。Web-only development is not a substitute for the native application.

完整开发模式需要显式允许开发网页来源：macOS 使用 `PET_MANAGER_ALLOW_DEV_ORIGIN=1 npm run dev`；PowerShell 先运行 `$env:PET_MANAGER_ALLOW_DEV_ORIGIN="1"` 再运行 `npm run dev`。正式发布不要设置此变量。

## 完整客户端依赖 / Native runtime dependencies

应用打包需要目标平台的可重定位 Node.js 和 FFmpeg 8.1.2、对应许可证与源码。默认会校验 FFmpeg 的官方发布构建哈希。准备方式见 [runtime-dependencies.md](docs/runtime-dependencies.md)。

```sh
npm run prepare:desktop
npm run dev
```

完整构建 / Build installers:

```sh
# macOS arm64
npm run build:mac:local
# Windows x64，在 Windows 开发终端执行 / on Windows
npm run build:win
```

Rust 检查需要先准备完整资源：`cargo test --manifest-path src-tauri/Cargo.toml --lib`。
macOS 本地包采用 ad-hoc 签名，不等于 Developer ID 签名或公证。Windows 发布签名也需发行者自行配置。

## 目录说明 / Directory guide

- `builtin-clawpkgs/` 保存内置组件源码，`../firmware/` 构建会读取这里。
- `public/terrier-clips/` 是出厂形象及对应预转换素材。
- `src-tauri/firmware/esp32-p4/` 保存配套升级镜像。Mac/Windows UART 参数不同，勿混用。
- 使用语音识别或形象生成时，在客户端“API 配置”页填写自己的服务 Key。
- 本机 bridge 仅绑定回环地址；不要通过代理、端口转发或公网监听暴露它。安全说明见 [SECURITY.md](../SECURITY.md)。

许可及商用授权见根目录 [LICENSE](../LICENSE)，第三方材料保留独立许可。
