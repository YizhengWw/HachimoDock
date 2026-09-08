# 固件构建与烧录 / Firmware build and flashing

目标为 ESP32-P4、32 MiB Flash；硬件参数见 `boards/`、`sdkconfig.defaults`、`partitions.csv`。完整技术说明见 [README.md](README.md)。

## 获取与构建 / Build

安装 Git LFS、Python 3 和 PlatformIO Core **6.1.19**（`python -m pip install platformio==6.1.19 pytest Pillow`）。项目通过 `platformio.ini` 固定平台版本，由其安装 ESP-IDF 和 P4 工具链。v1 使用 `dependencies.lock`，v3 使用 `dependencies.v3.lock`；首次构建需要联网下载依赖。

```sh
# 仓库根目录 / repository root
git lfs pull
python firmware/tools/build_builtin_bundle.py --check
python -m pytest firmware/tests
cd firmware
pio run -e esp32_p4_evboard
pio run -e esp32_p4_evboard_v3
```

`esp32_p4_evboard` 构建旧版芯片固件（0.1～1.99）；`esp32_p4_evboard_v3` 构建 v3.0～3.99 固件，使用 ESP-IDF 5.5.4。`esp32_p4_evboard_windows` 仅是默认环境的兼容别名。**固件按芯片版本区分，不按 Windows/macOS 区分**；两端都使用 4M UART。

微雪 ESP32-P4-WIFI6-M 的 M 表示焊接排针，不能据此判断芯片版本。v1 与 v3 的镜像不可互刷，也不要通过修改最低芯片版本或强制烧录绕过检查。每次切换环境均使用对应的 `sdkconfig.<环境名>`，不要复制另一个环境的 sdkconfig。

CPU 时钟遵循 ESP-IDF 的芯片配置：v1 保持 360 MHz，v3 使用 400 MHz（v3 配置不提供 360 MHz 选项）。没有为旧芯片开启实验性超频。画面、游戏与输入继续按原有时间逻辑运行，UART 速率不变。参见 [ESP-IDF 5.5.4 时钟配置](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_system/port/soc/esp32p4/Kconfig.cpu)。

PC 打包前，将两个环境的 `firmware.bin` 分别放入 `pc/src-tauri/firmware/esp32-p4/firmware.bin` 和 `firmware-v3.bin`。资源检查要求版本一致、芯片范围正确且来自干净提交；不要用空文件或旧镜像代替。客户端按实际芯片版本选择升级文件，未知版本不会猜测。

v3 的硬件验收尚需使用实际 v3 板完成：冷启动、画面颜色与动画、按键/摇杆、麦克风、完整形象下发、OTA、断线重连及断电恢复。编译与主机测试通过不等于实机验收通过。

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
