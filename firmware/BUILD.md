# 固件构建与烧录 / Firmware build and flashing

目标为 ESP32-P4、32 MiB Flash；硬件参数见 `boards/`、`sdkconfig.defaults`、`partitions.csv`。完整技术说明见 [README.md](README.md)。

## 获取与构建 / Build

只想使用设备，无需安装编译环境：下载对应芯片的 [完整烧录包](https://github.com/YizhengWw/HachimoDock/releases/latest)，按包内中文指南操作。烧录需要 Python **3.10+**，包内脚本会安装并检查 **esptool 5.4.0**；不要用旧版工具或 `--force` 绕过芯片检查。

自行构建需要 Git LFS、Python 3.10+、PlatformIO Core **6.1.19**（6.1.18 不支持 v3 平台）。在独立 Python 环境中安装：`python -m pip install platformio==6.1.19 pytest Pillow`。项目通过 `platformio.ini` 固定平台版本，由其安装 ESP-IDF 和 P4 工具链；首次构建需要联网下载依赖。

| 项目 | v1 构建 | v3 构建 |
| --- | --- | --- |
| PlatformIO 环境 | `esp32_p4_evboard` | `esp32_p4_evboard_v3` |
| pioarduino 平台 | `55.03.32` | `55.03.38-1` |
| ESP-IDF | `5.5.1` | `5.5.4` |
| RISC-V 工具链 | `14.2.0_20250730` | `14.2.0_20260121` |
| 依赖锁 | `dependencies.lock` | `dependencies.v3.lock` |
| 出厂烧录工具（不同于编译环境自带版本） | esptool **5.4.0** | esptool **5.4.0** |

两种环境请依次构建，不要在同一工作目录并行构建，避免依赖环境相互覆盖。Windows 建议把仓库和构建缓存放在较短的纯英文路径（例如 `C:\work\HachimoDock`），并在 PowerShell 使用下方相同的 `python -m platformio` 命令；`tools/p4.ps1` 的默认构建目标仍是 v1。

```sh
# 仓库根目录 / repository root
git lfs pull
python firmware/tools/build_builtin_bundle.py --check
python -m pytest firmware/tests
cd firmware
python -m platformio run -e esp32_p4_evboard
python -m platformio run -e esp32_p4_evboard_v3
```

`esp32_p4_evboard` 构建旧版芯片固件（0.1～1.99）；`esp32_p4_evboard_v3` 构建 v3.0～3.99 固件，使用 ESP-IDF 5.5.4。`esp32_p4_evboard_windows` 仅是默认环境的兼容别名。**固件按芯片版本区分，不按 Windows/macOS 区分**；两端都使用 4M UART。

微雪 ESP32-P4-WIFI6-M 的 M 表示焊接排针，不能据此判断芯片版本。**v1 与 v3 的镜像不可互刷，具体版本可以咨询客服进行确认。** 也不要通过修改最低芯片版本或强制烧录绕过检查。每次切换环境均使用对应的 `sdkconfig.<环境名>`，不要复制另一个环境的 sdkconfig。

CPU 时钟遵循 ESP-IDF 的芯片配置：v1 保持 360 MHz，v3 使用 400 MHz（v3 配置不提供 360 MHz 选项）。没有为旧芯片开启实验性超频。画面、游戏与输入继续按原有时间逻辑运行，UART 速率不变。参见 [ESP-IDF 5.5.4 时钟配置](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_system/port/soc/esp32p4/Kconfig.cpu)。

PC 打包前，将两个环境的 `firmware.bin` 分别放入 `pc/src-tauri/firmware/esp32-p4/firmware.bin` 和 `firmware-v3.bin`。资源检查要求版本一致、芯片范围正确且来自干净提交；不要用空文件或旧镜像代替。客户端按实际芯片版本选择升级文件，未知版本不会猜测。

屏幕排线必须接 **DSI**（显示输出），不能接 CSI（摄像头输入）；请断电后插拔排线。针对配套屏幕的 v3 固件采用 24 MHz 显示时序，相关修复已在 v3 板确认恢复宠物动画。未连接摇杆或启动校准无效时，方向输入停用，连接摇杆并保持居中后重启可重新校准。

当前公开固件为 0.7.64-p4，包含双向音频前端、实时字幕、独立于形象的系统提示音及 PC 下发的实时行情组件。v1/v3 均提供独立构建及完整烧录包。历史 v3 显示时序、形象播放验证，以及部分 v1 音频测试，不代表本版所有板型和声学环境均完成实机验收；按键、麦克风、近远场打断、完整资源传输和断电恢复仍需按实际硬件检查。详见 [双向语音验证边界](../pc/docs/realtime-duplex.md)。

## 完整出厂镜像 / Complete factory image

先运行 `pio run -e esp32_p4_evboard_v3 -t factory`（旧芯片改用 `esp32_p4_evboard`）。PC/设备仍使用同一套形象、组件、分区和交互配置。

`factory-config.json` 读取 `../pc/public/terrier-clips/` 与 `../pc/builtin-clawpkgs/`，把形象和组件放入出厂存储。默认构建的 factory-image 后处理脚本生成完整镜像及清单；不要将仅应用分区的 `.bin` 当作完整烧录包。

在仓库根目录执行 / From the repository root:

```sh
node pc/scripts/package-factory-release.mjs \
  --build-dir firmware/.pio/build/esp32_p4_evboard_v3 \
  --output /path/to/empty-output-directory
```

输出 ZIP 名称包含 v1/v3，内含中文烧录指南、完整镜像、分段 bin、形象/组件资料、校验清单及 macOS/Windows 烧录入口。工具读取实际芯片版本，与 Bootloader 和应用镜像都匹配后才擦除。首次烧录使用完整镜像及包内工具；日常 OTA 使用 PC 随包的应用升级镜像。

## 注意 / Caution

完整出厂烧录会覆盖设备上的原有配置和素材，操作前自行备份。烧录前核对板型、Flash 容量和 SHA-256，传输过程中不要拔线。Live-device tests and flashing are manual opt-in operations and are not run by source CI.

ESP-IDF 可能把本地驱动路径写入 `dependencies.lock`。提交前运行 `python tools/normalize_dependency_lock.py`，将本机路径恢复为 `$PET_P4_PROJECT_DIR/components/…`；勿提交绝对路径。
