# PC 客户端 / Desktop client

当前源码版本 **0.1.199**，配套固件版本 **0.7.103-p4-r179**。`main` 可包含安装包发布后的修正；安装包与源码归档见 [0.1.199 Release](https://github.com/YizhengWw/HachimoDock/releases/tag/v0.1.199)。该归档对应 `v0.1.199` 标签。公开构建不提供服务 Key，运行时凭据由用户在 API 配置中保存。

Pet Manager 用于连接设备、管理形象和组件，以及将 Agent 状态与语音输入同步到桌面小屏。支持 macOS arm64 与 Windows x64。

Agent 能力及平台限制见 [接入矩阵](docs/agent-integration-audit.md)。当前源码支持 [WorkBuddy](docs/workbuddy.md)：独立形象绑定、状态与气泡同步、当前可见对话的按键语音草稿及组件生成 Skill 安装。历史会话需在 WorkBuddy 手动打开。已发布安装包的能力以对应版本为准。

直接使用请前往 [下载页面](https://github.com/YizhengWw/HachimoDock/releases/latest)。以下内容面向需要自行编译或修改客户端的开发者，技术栈为 Tauri 2 + React。

## 环境与测试 / Requirements and tests

安装 Git、Node.js 22+、Rust stable，以及 Tauri 对应平台的系统依赖。当前源码素材使用普通 Git 保存，不需要 Git LFS 额度。Windows 需要 MSVC C++ Build Tools 和 WebView2；macOS 需要 Xcode Command Line Tools。

从仓库根目录运行 / From the repository root:

```sh
cd pc
npm ci
npm test
npm run test:bridge
npm run build:web
```

`npm run dev:web` 只预览网页 UI，不提供设备、系统权限及本机 Agent 功能。完整客户端使用 `npm run dev`，先按下文准备运行时依赖。Web-only development is not a substitute for the native application.

完整开发模式需要显式允许开发网页来源：macOS 使用 `PET_MANAGER_ALLOW_DEV_ORIGIN=1 npm run dev`；PowerShell 先运行 `$env:PET_MANAGER_ALLOW_DEV_ORIGIN="1"` 再运行 `npm run dev`。正式发布不要设置此变量。

### 框架离线验收入口

依赖已在本机准备好后，从仓库根目录执行；输出目录必须显式指定，脚本会在其中
新建独立目录，保存 `report.json`、各阶段日志和测试结果，不覆盖旧验收记录。

```sh
node pc/scripts/test-framework-offline.mjs --output /absolute/evidence/directory
# 可选：真实合成 UI 浏览器测试；浏览器必须已安装，不会自动下载。
node pc/scripts/test-framework-offline.mjs --output /absolute/evidence/directory --browser --browser-executable /absolute/path/to/chromium
# 可选：仅检查完整桌面 Rust 类型接线，不执行 Rust 测试或启动客户端。
node pc/scripts/test-framework-offline.mjs --output /absolute/evidence/directory --desktop-check
```

默认执行入口自身测试、`npm test`、`npm run test:bridge`、明确白名单中的固件
宿主测试，以及真实 workflow / screenshot 模块的轻量 Rust harness。白名单包括
资源与生命周期、冷激活与存档、表单/导航/视口、流程、程序层门禁和传输边界。
程序层仅跑 native/fuzz 门禁，不自动寻找设备、编译目标固件或执行全 crate 的
Rust 测试；尤其不执行 ignored 实机用例。Cargo / npm 强制离线，pytest 关闭
自动加载第三方插件。依赖不足会报告 `blocked`，不安装或下载工具。

`--browser` 只启动回环地址的 Vite 合成页面和 headless 浏览器，测试拦截外部
HTTP/WebSocket，没有 Tauri IPC 或用户数据；退出时关闭该临时服务。`--port`
可指定未占用端口。`--node`、`--npm`、`--python`、`--cargo`、`--git` 可指定工具
路径，也可使用相应的 `PET_FRAMEWORK_NODE/NPM/PYTHON/CARGO/GIT` 环境变量。
`CARGO_HOME` 沿用调用者已有缓存，`--target-dir`（或 `PET_FRAMEWORK_TARGET_DIR`）
可复用编译缓存；缺缓存同样阻断，不联网补齐。
桌面类型检查沿用调用者的 `TAURI_CONFIG`，报告只记录是否存在覆盖配置，不记录
配置内容。它是 compile-only 检查，不验证 bundle 资源、安装包或签名。

每阶段输出 `passed/failed/skipped/blocked` 和退出码；实际测试中的 skip/ignored
不会被当作通过，缺少可解析结果也会阻断。独立阶段默认继续，依赖失败的阶段
不执行；`--fail-fast` 会把剩余阶段标记为阻断。记录源码 commit、dirty 状态和
前后内容摘要，运行期间源码改变则阻断本次验收。
摘要范围是 PC 源码/脚本/petui 参考/合成 UI 夹具、固件源码/测试，以及包锁、
Cargo、Tauri、CMake、PlatformIO 和协议配置；报告列出覆盖路径，**不是整个仓库
所有素材的隐私或完整性扫描**。浏览器阶段与前端测试独立，前端存在 skip 不会
阻止浏览器执行，但仍会使本次选中阶段未全部通过。

退出码 `0` **仅代表本次选中的离线阶段全部通过**，`1` 为测试/命令失败，`2` 为
缺依赖、未验证、跳过测试或源码变化。未选择的浏览器/类型检查仍显示 skipped，
因此默认报告的整体状态为 `incomplete`，不能声称所有离线项目均已通过。
即便所有可选项均通过，也只得到 `offline_passed`：报告中的硬件和声学始终是
`pending`，`passed` 与 `releaseReady` 始终为 false。该入口不表示三层架构已全面
开放或实机性能、语音/AEC 已验收；不会录音、播放、刷机、调用模型、打包或发布。

入口逻辑可单独验证（假 runner，不执行真实工具或连接设备）：

```sh
node --test pc/scripts/test-framework-offline.test.mjs
```

## 完整客户端依赖 / Native runtime dependencies

应用打包需要目标平台的可重定位 Node.js 和 FFmpeg 8.1.2、对应许可证与源码。默认会校验准备脚本记录的已审核 FFmpeg 构建哈希。准备方式见 [runtime-dependencies.md](docs/runtime-dependencies.md)。

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
- `src-tauri/firmware/esp32-p4/` 保存配套升级镜像。macOS 与 Windows 共用同一份 4M UART 固件，不需要按电脑系统选择固件。
- 使用语音识别或形象生成时，在客户端“API 配置”页填写自己的服务 Key。
- 本机 bridge 仅绑定回环地址；不要通过代理、端口转发或公网监听暴露它。安全说明见 [SECURITY.md](../SECURITY.md)。

许可及商用授权见根目录 [LICENSE](../LICENSE)，第三方材料保留独立许可。

### 受限程序离线创作验证（实验，不是已开放组件）

`skills/petui/scripts/program-preview.mjs` 可从 stdin 接受有界整数程序和合成事件，
使用固定产品 C 解释器 WASM 执行。不会读取用户配置、发起模型调用、接设备、
安装组件或开放权限。十类业务/游戏纯逻辑向量在原生 C 与预览中逐一比较；
不能据此宣称设备已经支持可安装程序、后台调度或任意原生外设。
详见 [程序预览说明](skills/petui/references/program-preview.md)。

旧整数/选择器表单的 PC 预览和 JS 工具复用现有固件 C 核心，固定 64 KiB、零 imports，
仅表单声明需要时创建并复用实例。存档、实体键位、草稿/确认/取消语义不变，
不因此开放通用程序或外设能力；见 [表单契约](skills/petui/references/app-form.md)。

### 共享 WASM 运行时

预览运行时由同一份 C 核心生成。重建与 `--check` 使用 LLVM/LLD **20.1.8**，
通过 `CLANG` 指定 clang；配套 wasm-ld 必须可用。编译器版本会影响输出字节，
请使用固定工具链保留原有镜像一致性检查。普通客户端开发不需要重建这些已提交产物。
