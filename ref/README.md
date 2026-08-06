# Pet Manager Desktop (`ref/`)

`ref/` 是 Pet Manager 桌面端应用。它使用 Tauri 2 + React + Vite：

- `src/`：React UI，包括设备向导、设备仪表盘、宠物相册、组件中心、语音控制和
  当前展示配置。
- `src-tauri/`：Tauri/Rust 后端，负责本地文件、bridge 进程、USB/MQTT 下发、
  agent session 监听、组件安装和系统能力调用。
- `builtin-clawpkgs/`：内置负一屏组件源目录。
- `public/`：桌面端静态资源、内置形象素材和帮助图。
- `design.md`：客户端设计令牌、按钮层级、状态、弹窗和响应式规范。

## 开发启动

Windows：

```powershell
cd ref
npm install
cd ..
.\scripts\dev-manager.ps1
```

macOS / Linux：

```sh
cd ref
npm install
npm run dev
```

Windows 启动脚本会统一 UTF-8 控制台、Python 子进程和临时目录；需要换端口时使用 `.\scripts\dev-manager.ps1 -Port 4174`。

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

`npm run build` 会走 Tauri 桌面应用构建，并准备目标平台的 Node、校验过的静态
FFmpeg，以及已经转换完成的内置西高地 P4 ready pack。安装后的形象生成/导入会在
素材保存阶段立即生成对应 ready pack；点击设备同步时只校验并传输现成的
H.264 Annex-B/WAV，不再临时调用 FFmpeg，也不要求用户预装 Homebrew 或 winget。平台安装包
说明见 [../docs/desktop-packaging.md](../docs/desktop-packaging.md)。

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
| `src/ComponentCenter.jsx` / `src/component-center/*` | 负一屏组件中心，管理内置、正式本地与板端组件，并负责预览、同步和删除。 |
| `src/lib/*` | 共享业务逻辑，如设备绑定、形象分配、组件契约和本地状态。 |
| `src-tauri/src/lib.rs` | Tauri 命令入口，负责桥接本机能力和设备下发。 |
| `src-tauri/bridge/*` | 本地 bridge sidecar 和 agent session 监听相关代码。 |

## 设备通信

桌面端可以通过几条路径和设备端通信：

- ESP32-P4 native USB vendor bulk：通过 nonce + `boardDeviceId` 精确选择设备；当前用于查询或激活板上已缓存的完整形象包。
- USB serial / USB-UART：用于直连设备、按钮配置、状态同步、刷机日志，以及 native cache miss 时的完整 raw 形象包传输。
- MQTT：legacy Linux/Raspberry Pi runtime 的无线可达性、远程绑定和状态/语音同步路径。

P4 ready pack v9 使用 640x480、15 FPS、最多 225 帧、CRF 27 的 H.264 Annex-B；
aspect-fit 与黑边保持原始比例，不拉伸主体。内置西高地
在 PC 安装包构建时完成转换；其他形象在生成、上传、导入或单状态替换保存时立即转换。
点击同步时不会读取 MP4 或启动 FFmpeg，只传输已经完整校验的 ready pack；命中设备
A/B 形象缓存时则直接切换。源视频字节相同的多个动画状态共享一个 SHA-256 内容寻址
文件，manifest 保留状态映射，但 USB 只传一次共享资源。

USB-UART 完整同步路径优先以 4 Mbaud 连接 ESP32-P4，使用 8 KiB 的“JSON 头 + 原始
二进制”分块，不再承担 Base64 的 33% 膨胀与设备端解码开销；slot1 的 H.264 顺序写入
专用 raw 分区，manifest/WAV 仍保存在 SPIFFS，索引和 ready marker
均在完整校验后提交。完整同步前若 slot1 正在使用，桌面端会先激活有效 slot0，保证新包继续
写入高速 slot1。该能力由 hello capability 驱动，Windows 与 macOS 共用同一逻辑；旧固件
仍自动回退到 3 Mbaud/921600/115200 与兼容 Base64 协议。每次完整同步完成后，桌面日志会
打印实际 `effective_kib_s`，用于区分串口线速、Flash 写入和协议等待开销；当前 CH343
Windows 实机稳定结果约 132 KiB/s。native USB 在复用同一 raw writer 前不会尝试写入
cache-miss H.264 包，而是在任何擦除前提示切回 USB-UART。

P4 的按键语音是 USB-only 路径，不要求电脑具有 LAN 地址，也不会重复发布 MQTT
控制命令。音频启停和 PC 麦克风事件都绑定当前 board id；当前 PCB 无 ES7210 时，
设备会使用 ES8311 采集板载麦克风；单次录音最长 30 秒。若正式
`voice-service-node` 资源存在，PCM 继续交给该服务；资源缺失时，Windows 版使用
已安装的简体中文识别器，并将识别文本通过同一 Agent Session Bus 注入当前会话。
两条 STT 路径互斥，不会把一次按键语音重复发送给 Agent。

首次使用 SW1 长按语音前按以下顺序启用：

1. 通过 USB 连接 P4，并在“当前展示”选择要控制的 Agent 渠道。
2. 在“按钮配置”确认 `SW1 长按 = 语音输入`，点击“通过 USB OTA 下发按钮配置”。
3. 在“语音助手”打开“是否开启语音”，选择要续接的会话，再点击“启动按键语音”。
4. 按住 SW1 说话，松开后等待“设备识别文本”和“设备语音状态”返回；不要在识别完成前换板或断开 USB。

P4 的默认控制是：SW1 短按“返回（取消）”、长按语音输入，SW2 短按打开组件中心，
SW3 短按“确认进入下一级”；摇杆中键短按同样用于确认，长按默认不绑定，左推或右推切换
上一个/下一个会话。SW1/SW2/SW3 的短按与长按，以及摇杆上、下、左、右、中键短按、
长按都可以独立设置；按钮配置保存在 P4 NVS，设备重启后仍然有效。组件自己的
按键映射只在组件打开后生效，安装、切换或移除组件不会覆盖这份设备页面配置。

P4 的“Agent与形象”区域提供一个全局“显示活跃会话”开关，统一作用于 Codex、
Claude Code、MiMoCode 和 OpenClaw。开启后主屏卡片数量由当前 Agent 的运行中
对话自动决定，不再提供 1、2、3 个手动限制。`done` 和 `error` 会话进入结束态后
继续显示 60 秒，随后自动移除；`idle` 历史不下发到主屏，但仍保留在电脑端供语音
路由使用。关闭时设备主屏不展示会话。

P4 设备目标不要写死串口名、board id 或 USB VID/PID。调试 legacy Linux 设备时才使用：

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

`component.json.kind` 必须明确为 `game` 或 `tool`。默认组件按产品顺序展示双键接球、
Flappy Bird、像素方块、像素贪吃蛇、番茄钟、喝水提醒和 Token 仪表盘。两类组件共用
`p4-bounded-runtime-v3` 受限声明式运行时；需要坐标、移动、碰撞或边界行为时再使用
`p4-grid-scene-v1` 通用场景能力。退出跟随设备全局 `page_back` 设置（默认 SW1 短按），
新组件不声明退出动作，也不占用默认退出键；摇杆中键长按默认不绑定。旧包中的
`page_main` 仅作兼容读取并在下发时移除，旋钮事件名继续兼容摇杆左右。内置组件保持产品顺序，正式本地组件按
版本目录创建时间倒序排列，平台不支持时回退到修改时间。
组件 id 统一限制为 1–47 位、小写字母开头且只含 `a-z / 0-9 / - / _`，保证
前端校验、Linux、P4、安装和删除使用同一寻址规则。

内置组件可通过仓库级脚本打包：

```sh
cd ref
npm run pack-builtins
```

这七个内置组件的 runtime/button JSON 同时嵌入 PC 所携带的 P4 A/B 固件镜像。
通过 PC 完成固件升级并重启后，设备会按同一 id 刷新内置组件、补齐缺失项并移除
已退役的接住星星，同时保留用户自建组件和升级前正在使用的组件选择。

P4 只安装 `p4-bounded-runtime-v3` 受限声明式运行时，仍拒绝任意 reader/fetcher。内置
`token-usage` 是受控例外：桌面端下发前移除 Linux 文件 reader，P4 再用已经
校验的 `stats/update` 数据更新总量、输入、输出和缓存变量，不在设备上开放
文件或网络读取。Linux 版本则从本地统计 payload 的稳定独立字段读取同一组数据。

组件生成以 640×480 逻辑画布和槽位字节预算作为边界，不要求填满固定模板。
`game` 与 `tool` 共用同一套 `states / transitions / tick` 状态机，可表达番茄钟、
喝水提醒、计数器、问答和回合制玩法。需要连续移动或碰撞的小游戏可额外声明
`p4-grid-scene-v1` 固定内存场景，由矩形实体、速度、边界、碰撞规则和受限操作组合
出不同玩法，无需选择某个已有游戏。`blocks`、`snake` 与 `flappy` 只作为旧组件包的
兼容预设保留，不是新组件的三种引擎。运行时仍不支持任意脚本或联网对战。每个组件可携带最多 8 条
`buttons.json` 绑定，覆盖屏幕、SW2、SW3 和四向中按摇杆。退出不写入组件包，始终跟随
设备全局 `page_back` 设置（默认 SW1 短按）；旧包中的 `page_main/page_back` 下发时会被移除。
最终安装页可逐项调整其他功能键；只要组件声明了按键，安装时会把映射写入组件包，并且只在该组件打开后生效，
未声明实体事件会安全禁用。调整结果按组件保存，
重新打开组件中心时仍能看到并继续编辑该组件自己的映射。
组件中心本身沿用 Pet Manager 的通用管理界面；像素风只用于板端组件画面与预览。
`petui` 新生成的 `game` 和 `tool` 必须在初始 dashboard 与 runtime
dashboard 中使用 `visualStyle: "pixel"`。`classic` 仅保留为历史包读取兼容值，
不会用于新生成产物。
组件生成不再由 Pet Manager 拼接 prompt、启动 Agent CLI 或打开额外终端。用户在当前
Agent 对话中调用 `petui`；Skill 校验通过后把内容寻址版本原子发布到
`~/.claw-pet/components/library/<component-id>/<version-hash>/`，组件中心通过文件监听
自动发现，并以 30 秒轮询作为通知不可用时的兜底。`.staging` 只用于发布事务，不是草稿库。
组件中心会保留当前组件并显示“已启用”标记。活动记录按
`usb:<boardDeviceId>` 或 `ssh:<host>` 隔离，并保留内置包或正式库版本目录这一
精确来源；旧版无设备身份的记录只显示“上次启用 · 未确认设备”，不会开放删除。
删除确认框展示真实目标，后端只接受显式 USB/SSH 目标；USB 在发送和等待 ACK
期间持续校验 board id，避免拔插后误删另一台同名组件。板端确认删除包、持久状态
和活动标记后，客户端才清除该目标的“已启用”状态并回到桌宠主页。Linux 的
USB/SSH 安装与删除共用事务锁和 staging marker，逐文件校验大小/校验和并以
`transferId + path + index` ACK；删除通过 tombstone 原子下线，阻止残留 commit
把组件重新激活。本机“从组件库删除”只接受正式库根目录下精确到版本哈希的组件目录；
手动导入的 `.clawpkg/.zip` 会先经过安全校验和原子发布，不会直接成为删除目标。

组件中心顶部的设备概览展示板端实际保存的全部组件，并提供添加入口、逐包查看与移除；
下方组件库负责搜索、分类与安装。概览通过统一 `list_device_widgets` 命令读取实时清单：USB 使用
`widget/list` → `widget/inventory` 请求 ID 与 board id 双重校验；SSH 在共享读锁内
扫描 `/opt/board-runtime/widgets`，解析安全的 `component.json`，并排除
`.previous`、删除 tombstone 与符号链接。删除非当前组件只移除对应包，不会清空
当前 `.active-widget` 或把屏幕切回主页。

## 开发注意

- 桌面端 UI 变更同步更新 `ref/.folder.md`。
- Tauri 命令、USB bridge 或 P4 资产契约变更需要检查 `esp-p4-runtime/` 是否也要同步。
- Legacy Linux runtime 相关变更再检查 `legacy/board-runtime/`。
- 不要在业务代码里写死某一块设备的串口、IP、用户或 board id。
- selected agent、语音、USB active-state 下发要保持“只跟随当前选择 agent”的语义。
