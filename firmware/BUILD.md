# 固件构建与烧录 / Firmware build and flashing

目标为 ESP32-P4、32 MiB Flash；硬件参数见 `boards/`、`sdkconfig.defaults`、`partitions.csv`。完整技术说明见 [README.md](README.md)。

## 获取与构建 / Build

安装 Git LFS、Python 3、PlatformIO Core。项目通过 `platformio.ini` 固定平台版本，由其安装 ESP-IDF 和 P4 工具链；`dependencies.lock` 固定组件依赖。首次构建需要联网下载依赖。Python 的 PNG 预编译使用 Pillow（`python -m pip install Pillow`）。

```sh
# 仓库根目录 / repository root
git lfs pull
python firmware/tools/build_builtin_bundle.py --check
python -m unittest discover -s firmware/tests -p '*_test.py'
cd firmware
pio run -e esp32_p4_evboard
pio run -e esp32_p4_evboard_windows
```

`esp32_p4_evboard` 对应 macOS 的 UART 参数，Windows 使用 `esp32_p4_evboard_windows`。两种环境的输出目录彼此独立。Python 测试中的协议检查为独立脚本，另运行 `python tests/protocol_contract_test.py`。

## 完整出厂镜像 / Complete factory image

`factory-config.json` 读取 `../pc/public/terrier-clips/` 与 `../pc/builtin-clawpkgs/`，把形象和组件放入出厂存储。默认构建的 factory-image 后处理脚本生成完整镜像及清单；不要将仅应用分区的 `.bin` 当作完整烧录包。

在仓库根目录执行 / From the repository root:

```sh
node pc/scripts/package-factory-release.mjs \
  --build-dir firmware/.pio/build/esp32_p4_evboard \
  --output /path/to/empty-output-directory
```

输出 ZIP 内含中文烧录指南、完整镜像、分段 bin、形象/组件资料、校验清单及 macOS/Windows 烧录入口。首次烧录使用完整镜像及包内工具；日常 OTA 使用 PC 随包的应用升级镜像。

## 注意 / Caution

完整出厂烧录会覆盖设备上的原有配置和素材，操作前自行备份。烧录前核对板型、Flash 容量和 SHA-256，传输过程中不要拔线。Live-device tests and flashing are manual opt-in operations and are not run by source CI.

ESP-IDF 可能把本地驱动路径写入 `dependencies.lock`。提交前运行 `python tools/normalize_dependency_lock.py`，将本机路径恢复为 `$PET_P4_PROJECT_DIR/components/…`；勿提交绝对路径。
