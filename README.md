<div align="center">

# Hachimiao

[English](README.en.md) | 简体中文

Agent 的专属小屏：桌面常驻、实体陪伴，把 CLI Agent 的状态与回应变成可见、可触碰的桌面宠物。

<p>
  <img src="https://img.shields.io/badge/license-GPL--3.0--only-blue" alt="License: GPL-3.0-only" />
  <img src="https://img.shields.io/badge/desktop-Tauri%202%20%2B%20React-24292f" alt="Desktop: Tauri 2 + React" />
  <img src="https://img.shields.io/badge/hardware-Raspberry%20Pi%20Zero%202%20WH-2ea44f" alt="Hardware: Raspberry Pi Zero 2 WH" />
  <img src="https://img.shields.io/badge/version-0.1.0-orange" alt="Version: 0.1.0" />
</p>

<img src="assets/image_01.png" alt="Hachimiao cover" />

</div>

## 目录

- [1. 项目简介](#1-项目简介)
- [2. 核心亮点](#2-核心亮点)
- [3. 软件开发](#3-软件开发)
- [4. 硬件复刻](#4-硬件复刻)
- [5. 使用指南](#5-使用指南)
- [6. 附录与维护](#6-附录与维护)

## 1. 项目简介

Hachimiao 是 Agent 的专属小屏。它将 PC 上运行的各类 Agent（Codex、Claude Code、OpenClaw 等）具象为工位上一只可见、可触碰的桌面宠物。

Agent 在思考，它跟着思考；Agent 在调工具，它开始工作；任务完成它庆祝，任务报错它发愁。它不是一个停留在屏幕里的“虚拟”助理，而是有状态、有回应、有存在感的 AI 搭子。

| 抬头可见 | 开口即用 |
| --- | --- |
| 把 Agent 的状态、进度、待决策事项和任务结果翻译成前台可见的宠物行为。看一眼设备上的表情、动作和短标签，就能知道 Agent 正在做什么。 | 不必先打开电脑窗口、寻找 IM 聊天框或切回终端，开口即可和 Agent 对话、下命令，覆盖办公、写作、开发、查资料和记录想法等场景。 |

![Desktop companion][img-intro-1]

## 2. 核心亮点

### Agent 状态跟随

桌面端与硬件屏宠物状态实时同步，用表情、动作、颜色和短标签表达 Agent 状态；在 Agent 状态变化时提供字幕、轻提醒和提示信息。

![Agent status demo][img-status-gif]

| 状态展示 | 信息提示 |
| --- | --- |
| ![Status screen 1][img-status-1] | ![Status screen 2][img-status-2] |

### 空闲与触摸反馈

在 Agent 空闲时，pet 会自己玩耍，呈现多种待机状态。用户也可以触摸屏幕和 pet 互动。

![Idle interaction][img-idle-gif]

### Agent 语音交互

支持通过设备的麦克风与 Agent 的任意 session 对话，减少在终端和聊天窗口之间切换的成本。

### 自定义形象

内置西高地小狗形象（16 个状态动画），也可以通过上传宠物照片、头像或原创角色生成新形象，并支持从本机 Codex pet 库或 pet 社区导入其它形象。

| 形象生成 | 形象集合 |
| --- | --- |
| ![Custom avatar poster][img-custom-avatar-1] | ![Custom avatar dense poster][img-custom-avatar-2] |

### 自定义组件

内置摸鱼倒计时、番茄钟、喝水提醒、Token 消耗四个组件；支持用自然语言一句话生成新组件并下发到设备。

| 组件中心 | 组件预览 |
| --- | --- |
| ![Component center][img-components-1] | ![Component preview][img-components-2] |

## 3. 软件开发

Hachimiao 软件由 PC 管理端和设备端运行时组成：

- **Pet Manager 桌面端**：基于 Tauri 2、React 和 Vite，负责设备绑定、Agent 状态跟随、形象管理、组件中心、按钮配置、语音入口和连接诊断。
- **设备端运行时**：运行在 Raspberry Pi 设备上，负责屏幕展示、触摸/旋钮/按钮输入、状态接收、组件运行和本地资源管理。
- **Agent 接入层**：从 Codex、Claude Code、OpenClaw 等本机 CLI Agent 获取 session 状态、工具调用、等待确认、token 消耗等信息，再通过 USB serial 或 MQTT 下发到设备。

```text
CLI Agent
  |  Codex / Claude Code / OpenClaw session state
  v
Pet Manager desktop app
  |  device binding, appearance, component, button config
  |  USB serial or MQTT
  v
Board runtime on Raspberry Pi
  |  state files, widget runtime, touch/rotary/button input
  v
Hachimiao hardware display
```

### 3.1 快速开始

> 下面命令面向开发者；如果只想复刻硬件并使用现成安装包，可跳到 [5. 使用指南](#5-使用指南)。

```bash
git clone <your-repo-url>
cd claw-pet-manager/ref
npm install
npm run dev
```

常用脚本：

| 命令 | 说明 |
| --- | --- |
| `npm run dev` | 启动 Tauri 桌面端开发环境 |
| `npm run dev:web` | 仅启动 Vite Web 预览 |
| `npm run build` | 构建 Tauri 桌面端安装包 |
| `npm run build:web` | 构建 Web 静态资源 |
| `npm run pack-builtins` | 打包内置 `.clawpkg` 组件 |

### 3.2 开发环境

- Node.js 与 npm。
- Rust 与 Tauri 2 构建环境。
- 可用的本机 CLI Agent：Codex、Claude Code、OpenClaw 等。
- 可连接设备的 USB serial 或可访问 MQTT broker 的局域网环境。
- Raspberry Pi Zero 2 WH 设备端运行环境。

### 3.3 仓库结构

```text
claw-pet-manager/
  ref/                  Pet Manager 桌面端（Tauri 2 + React + Vite）
    src/                React UI：设备向导、仪表盘、形象画廊、组件中心、语音配置
    src-tauri/          Tauri + Rust 后端：本地文件、bridge、USB/MQTT 下发、组件安装
    builtin-clawpkgs/   内置负一屏组件源目录
  board-runtime/        Raspberry Pi 设备端运行时（C / shell / Python）
    src/                board-server 等设备端服务
    builtin-clawpkgs/   设备端内置组件源目录
    ui/ assets/         配网页面、字体和设备端资源
  scripts/              仓库级脚本，例如内置 .clawpkg 打包
  skills/               组件中心使用的 Agent skill
  docs/                 架构、语音链路、打包和二次开发文档
```

### 3.4 开放规范

后续重点开放和维护四类规范：

1. **Agent 状态协议**：第三方 Agent 如何把工作状态、字幕、token、工具调用与等待确认状态传给桌宠与小屏。
2. **设备端通信 / SDK**：桌面端经 USB serial 行协议或 MQTT 下发状态，支持不同硬件、语言与 Agent 接入。
3. **形象 / 动画规范**：让社区制作的宠物资源可复用，并兼容 Codex Pet 生态。
4. **`.clawpkg` 组件规范**：让负一屏组件可以被生成、安装、分享和复用。

一个 `.clawpkg` 组件包建议包含：

```text
component.json        组件元数据（id / name / version / author / description）
buttons.json          按钮功能绑定
negative-screen.json  负一屏显示配置
runtime/widget.json   声明式状态机（vars / states / transitions / tick / dashboard）
share.json            分享 / 导出元数据
assets/               图标等资源
```

## 4. 硬件复刻

### 4.1 硬件 BOM

![Hardware overview][img-bom]

#### 方式一：立创商城一键下单

适合直接按底板工程与电子料 BOM 统一下单。正式发布前请替换为立创商城真实一键下单链接。

#### 方式二：按表格自行购买

按下方整机物料表逐项核对采购。

| 类别 | 模块 / 器件 | 位号 / 接口 | 单套数量 | 备货总量 | 备注 |
| --- | --- | --- | ---: | ---: | --- |
| 开发板 | [Raspberry Pi Zero 2 WH][buy-pi] | 插入 U1 | 1 | 50 | 需预焊 40pin 排针版本 |
| SD 卡 | [microSD 卡][buy-sd] | TF 卡槽 | 1 | 50 | 32GB 以上，A1/A2 级别优先 |
| 屏幕 | [中景园电子 2.8寸 240x320 SPI TFT 触摸屏][buy-screen] | 插入 U2 | 1 | 50 | 以 11PIN 带 XPT2046 触摸版本为例；8PIN 无触摸版本作为同尺寸备选；选焊接排针、ILI9341 版本 |
| 旋钮 | [可按下旋钮模块 / EC11 编码器模块][buy-knob] | 接 H2 | 1 | 50 | 需和外观 ID 确认轴长、旋钮高度、安装方向和固定方式 |
| 麦克风模块 | [INMP441 麦克风模块][buy-mic] | 接 H1 | 1 | 50 | 若麦克风元件贴片成本过高，可采用外接模块方案 |
| 喇叭 | [1224 小腔体喇叭，1.25 端子，带双面胶][buy-speaker] | 接 CN7 | 1 | 50 | 1224，8 欧，1-2 W，1.25P |
| 线材辅料 | [杜邦线][buy-wire] | 按钮 / 旋钮 / 麦克风 | 15 | 750 | 20 cm 左右，母对母 |
| 结构辅料 | [自攻螺丝 M2 * 8 mm][buy-screw] | 屏幕固定 | 4 | 200 | 用于固定屏幕 |
| 结构辅料 | [自攻螺丝 M2 * 5 mm][buy-screw] | 结构固定 | 8 | 400 | 用于固定其他构件 |
| 外壳结构 | [3D 打印 / CNC 外壳][buy-shell] | 整机结构 | 1 | 50 | 外壳、后盖、内部固定支架 |
| 转接线 | [Micro-USB 公转 Type-C 母转接线][buy-micro-usb] | 内部开发板引出 | 1 | 50 | 10 cm；MicroUSB 公上弯转 Type-C 母直 `[mic2-tpc1]` |
| 转接线 | [Type-C 转 Type-A 转接线][buy-typec-a] | 设备连接到电脑 | 1 | 50 | USB 2.0 即可，4 芯及以上，不能只是充电线 |
| 定制 PCB 底板 | 见工程内底板 BOM | 各模块连接中转，减少杜邦线 | - | - | 底板物料随 PCB 工程核对 |

> 采购说明：链接仅供复刻参考，价格、库存和售后以对应平台为准；批量制作前请先核对型号与供货。

### 4.2 结构件与装配

MakerWorld 模型下载链接将在发布前替换为真实页面。

![Assembly overview][img-assembly]

![Assembly detail][img-assembly-detail]

### 4.3 板子工艺信息

![PCB front/back][img-pcb-front]

![PCB render][img-pcb-back]

| 项目 | 参数 |
| --- | --- |
| 关键厚度 | 1.6 mm |
| 板子层数 | 双层板 |
| 尺寸 | 71 mm * 30.5 mm |
| 焊接 | 音频处理部分（功放、麦克风）可能需要加热台；其余器件使用烙铁即可。若不需要音频交互，可考虑打裸板自行焊接排针排母，不影响产品核心功能。 |

## 5. 使用指南

完整的软件安装、构建、配置、二次开发和故障排查，请以项目文档为准。本节面向已经拥有 Hachimiao 硬件设备，或已经按复刻教程完成装配的用户。

![Pet Manager overview][img-manager]

### 5.1 准备

- 设备端已安装并能正常启动。
- PC 端已构建或安装 Pet Manager。
- 有可用 Wi-Fi 网络，或可进入设备 AP 直连环境。
- 至少有一个本机 CLI Agent：Codex、Claude Code、OpenClaw 等。

### 5.2 绑定设备

接通设备电源，等待设备进入首次启动状态。未绑定设备会进入等待连接或配网状态，屏幕显示待连接提示或默认宠物画面。打开 Pet Manager 客户端后进入设备绑定向导。

绑定向导分 4 步：

1. 选择方式（网线或 Wi-Fi）。
2. 网络绑定。
3. 验证通信，确认 Bridge 在线。
4. 确认形象。

两种连接方式：

- **插网线绑定（直连）**：插线后点击“检测绑定”，无需输入密码。
- **Wi-Fi 配网**：电脑临时连接设备热点（默认 SSID `claw-pet`），管理端经设备 AP（`192.168.44.1`）下发 Wi-Fi、MQTT、桌面设备 ID 与 namespace，设备再回到用户局域网；电脑网络在配网完成后自动恢复。

### 5.3 Pet Manager 管理端主页

控制台聚合设备连接状态、渠道与形象、按钮配置和语音助手入口。

- **连接状态**：显示桌面设备 ID、USB 与 Wi-Fi 在线状态，可重新扫描串口。
- **渠道与形象**：Claude Code / Codex / OpenClaw 各为一个渠道，分别绑定形象；设备端展示当前正在使用的渠道形象。
- **更多操作**：发送测试消息、复制桌面设备 ID、设备返回主屏、强制同步形象、解绑设备。

### 5.4 形象画廊与自定义形象

形象画廊用于浏览默认形象与自定义形象。进入详情可逐个预览每个状态的动画与状态提示音，也可以上传自定义 WAV。

“添加形象”支持三种方式：

- 新建自定义形象。
- 从 Codex 导入。
- 从社区导入。

内置“西高地小狗”形象（`builtin://terrier-clips`）共 16 个状态动画：

- **连接 / 欢迎**：`welcome`
- **工作**：`working.thinking`、`working.typing`、`working.browsing`
- **结果**：`waiting_user`、`done`、`error`
- **触摸反馈**：`touch.lick`、`touch.what`
- **空闲**：`idle.playing`、`idle.wandering`、`idle.begging`、`idle.daydreaming`、`idle.eating`、`idle.reading`、`idle.traveling`

自定义形象向导：

1. 上传参考图（PNG / JPEG / WebP / GIF，GIF 取首帧）。
2. 填写形象名称与性格描述。
3. 生成配置并同步到设备。

### 5.5 组件中心（设备负一屏）

组件中心是设备负一屏组件库。内置 4 个组件，并支持用 AI 生成新组件。

| 组件 | ID | 说明 |
| --- | --- | --- |
| Token 消耗 | `token-usage` | 把当前 coding agent 的实时 Token 消耗推到设备屏，等价换算成几顿午餐。 |
| 摸鱼倒计时 | `slack-off-countdown` | 提醒今天还有多久下班。 |
| 番茄钟 | `tomato-clock` | 25 分钟专注 + 5 分钟休息循环，点击屏幕开始 / 暂停，长按重置。 |
| 喝水提醒 | `drink-reminder` | 每 45 分钟提醒一次喝水，点击屏幕确认已喝，长按暂停或恢复。 |

创建组件分 3 步：

1. **装 Skill**：把 `petAgent-ui-generator` 装到检测到的 Coding Agent。管理端会自动扫描 `~/.claude/`、`~/.codex/`、`~/.openclaw/`、`~/.gemini/`、`~/.cursor/`。
2. **描述生成**：用自然语言描述组件用途、显示什么数字 / 状态、点击与长按做什么，skill 调起 Agent 自动生成组件。
3. **自动更新 / 手动加入**：生成完成后组件中心自动刷新；也可把 `.clawpkg` 目录或 zip 拖入手动加入。

## 6. 附录与维护

### 6.1 Contributing

欢迎提交 issue、discussion 和 pull request。建议贡献方向：

- 修复 Pet Manager 或板端运行时问题。
- 适配新的硬件屏、主控板、结构件或外壳形态。
- 创作宠物资源、动画素材和字幕样式。
- 开发新的 `.clawpkg` 负一屏组件。
- 补充装配教程、烧录说明和故障排查。
- 改进 Agent 状态协议和第三方 Agent 接入。

### 6.2 License

本项目建议按内容类型分别声明许可证，不建议只使用一个笼统协议。

- 软件代码 License：`[待补充，例如 Apache-2.0 / MIT]`
- 硬件设计 License：`[待补充，例如 CERN-OHL-S-2.0 / CERN-OHL-P-2.0]`
- 3D 结构件 License：`[待补充，例如 CC BY-SA 4.0]`
- 官方宠物素材 License：`[待补充]`
- 第三方资源声明：`THIRD_PARTY_NOTICES.md`

### 6.3 Security Issues

如果发现安全问题，请不要直接公开敏感细节。请通过项目维护者提供的安全反馈渠道联系，我们会尽快确认和处理。

### 6.4 Contact

- Maintainer：`[待补充]`
- Collaborator：`[待补充]`
- GitHub：`[待补充]`
- 嘉立创项目页：`[待补充]`
- 社区 / 交流群：`[待补充]`
- 安全反馈邮箱：`[待补充]`

### 更多信息

- 项目主页、交流群、模型/资源下载入口可在发布前替换为真实链接。
- 感谢嘉立创 / 立创生态、OpenClaw、MiMo、Petdex / Codex Pet 及相关开源项目和社区贡献者。

<!-- Images -->

[img-intro-1]: assets/image_02.jpeg
[img-status-gif]: assets/image_04.gif
[img-status-1]: assets/image_05.png
[img-status-2]: assets/image_06.png
[img-idle-gif]: assets/image_07.gif
[img-custom-avatar-1]: assets/petclaw-custom-crowd-product-poster.png
[img-custom-avatar-2]: assets/petclaw-custom-crowd-product-poster-white-dense.png
[img-components-1]: assets/image_10.jpeg
[img-components-2]: assets/image_11.png
[img-bom]: assets/image_12.png
[img-assembly]: assets/1aa4e315-fe70-44ae-853e-b72996ee1aae.png
[img-assembly-detail]: assets/image_15.jpeg
[img-pcb-front]: assets/image_16.png
[img-pcb-back]: assets/image_17.png
[img-manager]: assets/image_18.png

<!-- Purchase links -->

[buy-pi]: https://item.taobao.com/item.htm?id=693613248231
[buy-sd]: https://detail.tmall.com/item.htm?id=848065818893
[buy-screen]: https://item.taobao.com/item.htm?id=526024381409
[buy-knob]: https://e.tb.cn/h.iForjxnIRX1llEz
[buy-mic]: https://e.tb.cn/h.izOC4n5sjGeIoAm
[buy-speaker]: https://e.tb.cn/h.ixBS9SgI6gFXrIx
[buy-wire]: https://so.szlcsc.com/global.html
[buy-screw]: https://item.taobao.com/item.htm?id=39761471376
[buy-shell]: https://www.jlc-3dp.cn/
[buy-micro-usb]: https://detail.tmall.com/item.htm?id=867489662609
[buy-typec-a]: https://item.taobao.com/item.htm?id=726410843702
