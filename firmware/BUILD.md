# 固件构建与烧录 / Firmware build and flashing

当前源码版本 **0.7.103-p4-r179**，配套桌面端 **0.1.198**。版本号不是源码完全对应已发布安装包的证明；197 的具体对应关系见 [Release 源码说明](https://github.com/YizhengWw/HachimoDock/releases/tag/v0.1.197)。设备功能、输入、分区和出厂组件见 [固件说明](README.md)，字段契约见 [protocol.md](protocol.md)。

## 选择构建目标

| 环境 | 芯片修订范围 | 日常串口 |
| --- | --- | --- |
| `esp32_p4_evboard` | 0.1–1.99 | 4 Mbaud |
| `esp32_p4_evboard_v3` | 3.0–3.99 | 4 Mbaud |
| `esp32_p4_evboard_windows` | v1 兼容命令别名 | 与默认 v1 环境相同 |

固件按芯片区分，不按电脑系统区分。macOS 与 Windows 对同一芯片使用相同应用镜像。编译器、平台和 SDK 以 [platformio.ini](platformio.ini) 及锁文件为准；不要混用不同环境的 sdkconfig、芯片门槛或分区表。v1 保持 360 MHz，v3 使用 400 MHz。

微雪 ESP32-P4-WIFI6-M 的 M 表示焊接排针，不能据此判断芯片版本。**v1 与 v3 的镜像不可互刷，具体版本可以咨询客服进行确认。** 也不要通过修改最低芯片版本或强制烧录绕过检查。每次切换环境均使用对应的 `sdkconfig.<环境名>`，不要复制另一个环境的 sdkconfig。

其他命名环境用于功能候选或诊断，并非默认发布配置。开启实验标志不等于获得实机性能或外设权限保证；能力以编译条件、资源配额和设备实际声明为准。

## 获取与构建

只想使用设备，无需编译：下载与芯片匹配的 [完整烧录包](https://github.com/YizhengWw/HachimoDock/releases/latest)。包内工具要求 Python **3.10+**，使用 **esptool 5.4.0**，检查芯片后才擦除。不要用 `--force` 绕过检查。

自行构建需要 Git、Python 3.10+、PlatformIO Core **6.1.19**、pytest 和 Pillow。Core 6.1.18 不支持当前 v3 平台。素材保存在普通 Git 中，不需要 Git LFS。建议使用独立 Python 环境：

```sh
python -m pip install platformio==6.1.19 pytest Pillow
# 仓库根目录
python firmware/tools/build_builtin_bundle.py --check
# 完整主机检查还需真实 v1/v3 SDK 编译上下文，见下文。
python -m pytest firmware/tests
cd firmware
python -m platformio run -e esp32_p4_evboard
python -m platformio run -e esp32_p4_evboard_v3
```

首次构建会下载锁定平台、ESP-IDF 与工具链。Windows 建议纯 ASCII 的短工作路径，并使用 PowerShell；`tools/p4.ps1` 的默认目标为 v1，`setup` 和 `doctor` 可检查工具环境。测试依赖缺失应按实际报错处理，不要以空文件跳过资源检查。

## 配套桌面资源

两种芯片均构建完成后，将对应的应用镜像分别复制到：

- `pc/src-tauri/firmware/esp32-p4/firmware.bin`：v1
- `pc/src-tauri/firmware/esp32-p4/firmware-v3.bin`：v3

桌面准备脚本检查版本与芯片范围；仅应用分区镜像可用于 OTA。默认出厂形象及全部 29 个组件由 `factory-config.json` 从 PC 内置资源读取，不按某台测试机当前安装列表裁剪。

## 完整出厂镜像

```sh
# firmware 目录；v1 将环境名替换为 esp32_p4_evboard
python -m platformio run -e esp32_p4_evboard_v3 -t factory
# 回到仓库根目录
cd ..
node pc/scripts/package-factory-release.mjs \
  --build-dir firmware/.pio/build/esp32_p4_evboard_v3 \
  --output /path/to/empty-output-directory
```

完整包包含 Bootloader、分区表、应用、OTA 元数据、默认形象与组件、校验清单和双平台烧录入口。仅有 `firmware.bin` 不是完整烧录包。发布前分别检查 v1/v3 文件、芯片范围和实际 SHA-256；保持两端桌面安装包与对应固件资源一致。

## 升级、分区与接线

日常使用客户端 OTA：仅更新应用分区，保留用户数据，不修改分区表。新设备或恢复出厂使用完整烧录包，**会清除设备设置、自定义形象与组件**。要保留数据，应先备份；不要仅覆盖分区表或直接回写不匹配的 SPIFFS 镜像。

当前 32 MiB 布局为双 OTA 各 3 MiB、storage 8.875 MiB、appearance0/1 各 8.5 MiB，详见 [partitions.csv](partitions.csv)。旧设备普通 OTA 不会获得新的分区容量。应用镜像继续要求至少 128 KiB OTA 余量；分区增加不代替运行时堆、DMA、栈和帧耗时检查。

屏幕接 **DSI**，不是 CSI；插拔排线前断电。原生 USB 和 USB-UART 的选择取决于板卡切换电路，见 [USB 连接方式](README.md#usb-连接方式)。不确定板卡型号或芯片修订时先核对硬件资料。

## 验证与发布边界

- 主机测试验证模型与协议逻辑；目标编译验证芯片配置、链接资源和静态预算。
- 硬件验证分别检查输入/触控、资源显示、USB 连续传输、OTA/恢复、帧耗时、堆与栈水位。
- 音量、提示音、录放音、AEC 共存与真实听感需要设备和人工声学验证。
- 任何未运行、跳过或缺硬件的项目均不表示通过；不要把诊断配置的单次结果作为默认发布包或全部设备的保证。

修改传输代码时，区分明确拒绝与结果未知。超时不允许自动重放变更、自动清除旧事务或把重新连接当作提交成功；先通过只读状态与身份核对确定后续操作。

## 完整主机检查的 SDK 与 WASM 输入

CI 使用 Python 3.12、Node.js 22、PlatformIO Core 6.1.19，以及 LLVM/LLD **20.1.8**。
`CLANG` 指定该版本的 clang，配套 wasm-ld 位于其工具目录。生成的 WASM 与技能镜像
按字节检查，不应使用其他编译器版本重写产物来绕过检查。

完整测试会读取锁定 SDK 的真实源码、目标编译器和每个环境的编译数据库。
`tools/prepare_ci_sdk.py` 在 GitHub Actions 中分别准备 v1 与 v3 的 PlatformIO core，
通过 `compiledb` 生成真实上下文，并将路径写入 `GITHUB_ENV`。它不连接或烧录设备。
本机运行时，同样需要准备两个目标环境，将 `PET_P4_V1_COMPILE_COMMANDS`、
`PET_P4_V3_COMPILE_COMMANDS` 指向各自的编译数据库，并按测试要求提供对应的 SDK 路径。
不要用手写数据库或空 SDK 文件替代这些输入。
