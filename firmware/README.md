# Pet Manager ESP32-P4 设备端固件

本目录是 HachimoDock 当前使用的 ESP32-P4 固件源码，与 [PC 客户端](../pc/) 配合使用：PC 负责 Agent 状态监听、语音识别服务对接、素材预处理与传输；设备负责 USB 通信、按键与摇杆输入、屏幕渲染、形象播放及组件运行。

为设备安装固件，请下载 [完整烧录包](https://github.com/YizhengWw/HachimoDock/releases/latest)，解压后按中文指南操作。已能连接 Pet Manager 的设备，可直接使用客户端内的“固件升级”。需要自行编译时，请参考 [构建与烧录指南](BUILD.md)。

## 目标硬件

| 项目 | 配置 |
| --- | --- |
| 主控 | ESP32-P4，RISC-V 双核；高性能核心当前运行于 360 MHz，低功耗核心为 40 MHz |
| 片上内存 | 768 KB L2MEM、32 KB 低功耗 SRAM、8 KB TCM |
| PSRAM | 32 MB 封装内堆叠内存 |
| Flash | 32 MB QSPI NOR Flash |
| 显示屏 | 480 × 640 ST7701S MIPI 屏幕；固件使用 640 × 480 横向逻辑画布，再旋转输出 |
| 输入 | SW1 / SW2 / SW3、摇杆、GT911 触摸屏 |
| USB | Type-C，可通过板上切换电路连接 CH343 USB-UART 或 ESP32-P4 原生 USB HS OTG |

板型、引脚与分区配置以 [boards/](boards/)、[sdkconfig.defaults](sdkconfig.defaults) 和 [partitions.csv](partitions.csv) 为准。不同硬件版本烧录前需核对配置。

## 已支持的功能

- **USB 通信与设备识别**：USB-UART / USB Serial/JTAG 的 JSON Lines 握手，以及 `pet-usb-native-v1` 原生 USB 协议。设备身份由基础 MAC 派生，上报实际能力、固件版本、构建标识及协议版本。
- **会话气泡与形象**：接收 Agent 状态、进度和语音文本，按当前选中气泡的状态播放形象；会话结束后气泡保留 60 秒。
- **动画与音效**：播放预处理后的 H.264 Annex-B 动画及受限 PCM WAV 提示音，保留旧 MJPEG 素材兼容路径。
- **声明式游戏和工具**：支持组件状态持久化、移动与碰撞规则、PNG 精灵、动画和分层绘制；不执行组件自带的任意 JavaScript。
- **组件安装与恢复**：分块校验，文件与目录采用独立 A/B 代际提交，完整校验后生效；启动时选择有效版本，必要时回退。
- **可配置输入**：按键、长按与摇杆动作可由 PC 配置并保存至 NVS，跨重启保留；组件退出始终遵循全局返回设置。
- **设备麦克风输入**：通过 USB 传输 16 kHz、单声道 S16LE 音频；有 ES7210 时使用其采集，否则走 ES8311 路径。识别和 Agent 输入由 PC 完成。
- **A/B 固件升级**：分块确认与重试、SHA-256 校验、ESP-IDF 项目标识校验、启动健康确认和回滚。
- **传输异常恢复**：可查询、中止断线后残留的 OTA 事务；传输期间显示轻量传输画面，未完成的素材不会替换有效形象。
- **诊断与截图**：查询重启原因、堆内存、PSRAM、SPIFFS、输入及音频状态，并通过协议获取调试截图。

### 当前功能边界

- P4 固件尚未提供原生 Wi-Fi / MQTT 运行路径，当前以 USB 直连为主。
- 设备不直接播放 MP4；PC 先生成设备可用的 H.264 素材包。
- 设备不运行完整 HTML / JavaScript 页面，也不接受组件自带的任意网络读取器。
- ASR / TTS 不在设备本地执行，需要 PC 端及用户配置的服务。
- 原生 USB 可复用已缓存的形象包；包含 H.264 / MJPEG 的首次完整素材下发仍使用 USB-UART。

## 页面与默认操作

宠物主界面 `main` 和组件中心 `components` 是同级页面，顶部用两个圆点表示位置；组件运行页 `app` 是组件中心的子页面，运行组件时不显示这组切换圆点。

| 操作 | 默认行为 |
| --- | --- |
| SW1 短按 | 确认 / 进入 |
| SW1 长按 | 语音输入；识别文字保留在 Agent 输入框中，不自动发送 |
| SW2 短按 | 切换宠物主界面与组件中心；组件运行中若绑定了 SW2，则执行组件动作 |
| SW2 长按 | 不绑定 |
| SW3 短按 | 返回 / 取消；组件内按全局返回动作退出 |
| SW3 长按 | 不绑定 |
| 摇杆中键短按 | 确认 |
| 摇杆中键长按 | 不绑定 |
| 摇杆向上 / 向下 | 上一个 / 下一个；主界面选择会话气泡，组件中心选择组件 |
| 摇杆方向（组件内） | 按当前组件的动作映射执行 |

以上均为默认值，用户可在 PC 端修改。组件包只定义游戏或工具动作，不应另行定义固定退出键，也不能覆盖全局退出配置。输入经过软件消抖、长按识别及有界事件队列处理，避免阻塞渲染与传输。

## 默认组件

默认顺序为：**双键接球、蛙蛙养成、Flappy Bird、方块游戏、贪吃蛇、番茄钟、喝水提醒、Token 仪表盘**。

双键接球是默认列表的第一项，**不是固定置顶项**；用户新安装的组件会排在默认组件前面。设备开机进入宠物主界面，组件中心初始选择双键接球。

Token 仪表盘展示当前跟随 Agent 在本地自然日内的累计用量，不局限于单个前台会话。

固件应用镜像内包含默认组件及其受限精灵素材。普通 PC 固件升级后，设备更新同 ID 的内置组件，在容量允许时补入缺失项，同时保留无关的用户组件；无需为此覆盖整个文件系统分区。

## 构建固件

先安装 Git LFS、Python 3、PlatformIO Core 和 Pillow。首次构建需要联网下载 ESP-IDF、编译器及锁定版本的依赖。

### macOS / Linux

在仓库根目录执行：

```sh
git lfs pull
python -m pip install Pillow
python firmware/tools/build_builtin_bundle.py --check
cd firmware
python -m platformio run -e esp32_p4_evboard
```

### Windows

在仓库根目录执行：

```powershell
git lfs pull
cd firmware
.\tools\p4.ps1 setup
.\tools\p4.ps1 doctor
.\tools\p4.ps1 build
```

`setup` 配置纯 ASCII 路径的 PlatformIO 缓存和 Python UTF-8 模式；`doctor` 检查仓库、Python、工具链和临时目录，避免非 ASCII 路径导致工具链响应文件损坏。需要自定义缓存时使用 `-PlatformIoCoreDir`，路径应为纯 ASCII 且不超过 48 个字符。

两种桌面端的固件环境见 [platformio.ini](platformio.ini)：

| 构建环境 | 串口运行参数 |
| --- | --- |
| `esp32_p4_evboard` | macOS 使用，4 Mbaud |
| `esp32_p4_evboard_windows` | Windows 使用，2 Mbaud |

可执行 `python -m platformio run -e esp32_p4_evboard_windows` 构建 Windows 对应版本。两种环境使用独立输出目录；不要混用随 PC 分发的固件文件。

## 烧录与升级

### 日常升级

已完成出厂初始化的设备，优先使用 Pet Manager 内的固件升级。普通 OTA 只写应用分区，保留形象、用户组件和配置；新固件启动后执行内置组件迁移。

需要串口救援时，Windows 可执行以下受保护操作。请将 `COM5` 换为实际端口：

```powershell
cd firmware
.\tools\p4.ps1 flash -Port COM5
```

工具核对分区布局、校验写入结果并检查启动后的版本。请先确认设备已采用当前 A/B 分区布局。

### 首次烧录 / 恢复出厂

首次安装应使用**完整烧录包**，不能只刷应用分区的 `firmware.bin`。完整包包含引导程序、分区表、固件、OTA 元数据、西高地形象、内置组件、校验清单及烧录工具。

生成出厂镜像：

```powershell
cd firmware
.\tools\p4.ps1 factory
```

默认输出为 `.pio/build/esp32_p4_evboard/pet-manager-p4-factory.bin`，附带记录 SHA-256 和分段布局的 JSON 清单。生成可分发 ZIP 的步骤见 [构建与烧录指南](BUILD.md)。

**出厂烧录会覆盖设备原有配置、形象和组件，请先备份。** 工具要求显式提供 `-FactoryReset`：

```powershell
cd firmware
.\tools\p4.ps1 factory-flash -Port COM5 -FactoryReset
```

旧版单应用、16 MiB SPIFFS 分区设备不能直接用普通上传命令迁移。专用工具先验证旧分区，读取并校验备份，再重新初始化当前出厂布局：

```powershell
cd firmware
.\tools\migrate_to_ab.ps1 -Port COM5 -FactoryReset
```

旧文件系统不会原样写回较小的新分区，备份仅供手动恢复或提取。请保留备份，直到设备诊断、形象及组件均确认正常。

## Flash 分区

实际配置以 [partitions.csv](partitions.csv) 为准：

| 分区 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- |
| `nvs` | `0x009000` | 24 KiB | 设备与输入配置 |
| `phy_init` | `0x00F000` | 4 KiB | PHY 初始化数据 |
| `ota_0` | `0x010000` | 2.5 MiB | A/B 应用镜像 |
| `ota_1` | `0x290000` | 2.5 MiB | A/B 应用镜像 |
| `otadata` | `0x510000` | 8 KiB | A/B 启动选择元数据 |
| `storage` | `0x520000` | 6.875 MiB | SPIFFS 清单、音效、组件及元数据 |
| `appearance0` | `0xC00000` | 10 MiB | 受保护的西高地内置动画 |
| `appearance1` | `0x1600000` | 10 MiB | 可替换的自定义动画 |

正常启动不会自动格式化 SPIFFS。挂载失败时保留数据并记录错误，避免暂时性故障造成素材和组件被静默删除。

## USB 连接方式

对于采用相应 USB 切换电路的 Waveshare ESP32-P4-WIFI6 板：

- 跳线断开：Type-C 连接 CH343 USB-UART，用于烧录、串口日志、调试及完整形象下发。
- 跳线短接：Type-C 连接 ESP32-P4 原生 USB HS OTG，用于 TinyUSB 自定义批量传输。

切换的是数据通路，不改变供电输入；实际操作请核对所用板卡说明。原生接口通过 Microsoft OS 2.0 描述符匹配系统 WinUSB，无需单独安装 INF 或 Zadig。

macOS 默认采用 4 Mbaud、8 KiB 逻辑分块及分段节流；Windows 保持 2 Mbaud，可从较大的 raw 分块开始，完整性检查失败后以 2 KiB 分块重启传输。协议保留低速探测和 Base64 JSON 兼容回退，实际速度以日志统计为准。

原生 USB 打开目标前使用随机挑战值核对设备身份和协议版本；身份不唯一、回复过期或不支持身份检查时拒绝写入，可改用 USB-UART。

## 通信协议

完整字段与交互约定见 [protocol.md](protocol.md)。协议标识保留英文，以便与代码对应：

| 消息 | 用途 |
| --- | --- |
| `hello` | 设备身份、版本与能力握手 |
| `state/<agent>` | Agent 生命周期状态 |
| `session/current` | 当前选中会话及气泡队列快照 |
| `speech/text` | 气泡标题、正文与状态 |
| `control/screen-page` | 页面控制 |
| `stats/update` | Token 等受限统计数据 |
| `input/config` | 校验并持久化全局按键映射 |
| `widget/begin` / `widget/chunk` / `widget/commit` | 分块安装组件 |
| `widget/list` / `widget/delete` | 查询或删除组件 |
| `miniapp/event` / `miniapp/query` | 组件动作及运行状态查询 |
| `asset/*` | 形象素材传输、校验与提交 |
| `firmware/*` | 固件升级、状态查询、中止与提交 |
| `debug/screenshot` | 分块返回调试截图 |
| `diagnostics/query` | 查询重启、资源及外设状态 |
| `system/reset-inputs` | 重置输入配置，不删除素材 |
| `system/reboot` | 确认后延迟重启 |

`hello` 和 `diagnostics/status` 上报 `buildId`、`gitSha`、`buildDirty` 及 `protocolSchema`，用于区分同版本号的不同构建；缺失字段视为未知。

未知或不支持的消息返回明确 NACK；请求包含 `requestId` 时，回复回传该标识。PC 应按设备上报的能力启用功能，不应仅凭版本号猜测。

## 形象素材格式

PC 在形象创建、导入或视频替换后预处理素材；同步时只读取并校验结果，不再临时转码：

```text
p4/
├── manifest.json
├── families/sha256-<digest>.h264
└── audio/<family>.wav
```

- 动画采用 `p4-h264-v1`、H.264 Annex-B 和 Baseline 兼容参数。
- 画布为 640 × 480，按原比例缩放并补黑边，不拉伸图像。
- 采样上限为 15 FPS，每个动作最多 225 帧，按原视频时长分配帧。
- 相同源视频复用同一个哈希路径，避免重复下发。
- 提示音为 16 kHz、单声道、16 位 PCM WAV，每条最大 1 MiB。
- 设备采用受限 PSRAM 缓存和逐帧解码，减少每帧 Flash 读取；清单只在加载素材时解析。
- 动画存入专用形象分区，清单和音效存入 SPIFFS；数据校验后才提交索引和就绪标记。

## 测试与设备自检

在仓库根目录执行主机测试：

```sh
python -m unittest discover -s firmware/tests -p '*_test.py'
python firmware/tests/protocol_contract_test.py
python firmware/tools/build_builtin_bundle.py --check
```

Windows 也可使用 `.\tools\p4.ps1 test`，再运行 `build`。源代码测试不会自动烧录设备；物理设备测试及清空操作需手动执行。

烧录后建议检查：

- 版本、构建标识、分区布局与预期一致。
- 日志出现 `loaded P4 asset manifest`、`P4 asset cache loaded`，动画持续正常播放。
- H.264 路径出现 `P4 H264 decoder=software-dual output=I420`；旧 MJPEG 包可能显示硬件 JPEG 解码日志。
- `debug/lcd` 的初始化、背光和渲染状态正常；截图不全黑，连续截图校验值随画面变化。
- 按键、摇杆、触摸及音频采集正常，页面切换及组件退出符合全局配置。
- 中断传输后可重新连接、查询状态并恢复更新，原有效素材保持可用。

## 字体与许可

设备界面使用 MiSans Medium，来自[小米澎湃 OS 官方字体网站](https://hyperos.mi.com/font/zh/)，遵循 MiSans 字体知识产权许可协议。仓库不分发原始 OTF 文件，包含为设备生成的 16 px、1 bpp 中文字形数据。

项目自有源码采用根目录 [HachimoDock 项目许可证](../LICENSE)，仅限许可范围内的非商业用途；第三方驱动、字体与依赖继续遵循各自许可证，见 [第三方声明](../THIRD_PARTY_NOTICES.md)。
