# 原生运行时依赖 / Native runtime dependencies

Node.js 使用对应系统/架构的官方可重定位发行包，通过 `PET_MANAGER_NODE_BIN` 指定可执行文件。不要使用依赖 Homebrew 动态库的 Node 构建打包分发。

FFmpeg 为独立可执行文件。可从HachimoDock **v0.1.52** 安装包取得配套文件（在干净环境解包，先核对 Release 的 SHA-256）。macOS DMG 中位于 `Pet Manager.app/Contents/Resources/tools/`；Windows 安装目录位于 `tools/`。源码压缩包也作为该 Release 独立附件提供。不需要安装或启动客户端来构建源码。

将下列环境变量指向对应系统与架构的文件：

| 变量 / Variable | 文件 / File |
|---|---|
| `PET_MANAGER_NODE_BIN` | 官方 Node 可执行文件 / official relocatable Node executable |
| `PET_MANAGER_FFMPEG_BIN` | `ffmpeg` 或 `ffmpeg.exe` |
| `PET_MANAGER_FFMPEG_README` | `ffmpeg.README` |
| `PET_MANAGER_FFMPEG_LICENSE` | `ffmpeg.LICENSE` |
| `PET_MANAGER_FFMPEG_SOURCE_ARCHIVE` | `FFmpeg-8.1.2-source.tar.xz`（或随包同内容源码） |
| `PET_MANAGER_ZLIB_LICENSE` | Windows 所需 `zlib.LICENSE` |

准备脚本会校验架构、版本和 SHA-256，并将文件复制到 `src-tauri/generated-runtime/`。该目录为自动生成的构建资源，无需加入版本控制。

需要自行重编译 FFmpeg 时，使用随包 README 的完整构建参数及对应源码，并在审查后更新 `scripts/prepare-desktop-resources.mjs` 中目标哈希；不要删除来源与许可校验。自行编译的文件可能具有不同的 SHA-256，需要同步更新校验值。

编译客户端不需要云服务 Key；运行语音识别或形象生成时，再在客户端配置所用服务。
