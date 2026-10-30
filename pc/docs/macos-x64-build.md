# macOS x64（Intel）构建说明

## 为什么单独需要一份审计哈希

`scripts/prepare-desktop-resources.mjs` 只分发审计过哈希的 LGPL FFmpeg 运行时
（`macos-arm64`、`windows-x64`）。在 Intel Mac 上执行 `npm run build:mac` 会直接失败：

```
Error: 没有 macos-x64 的已审计 FFmpeg 二进制哈希
```

本说明给出 `macos-x64` 运行时的可复现构建配方，以及构建 Intel 版桌面端的完整步骤。

## 复现 FFmpeg 运行时（LGPL-only）

```bash
# 1. 官方源码（哈希固定）
curl -LO https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz
shasum -a 256 ffmpeg-8.1.2.tar.xz
# 期望 464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c

tar xf ffmpeg-8.1.2.tar.xz && cd ffmpeg-8.1.2
./configure --disable-gpl --disable-nonfree --enable-videotoolbox \
  --disable-ffplay --disable-doc --disable-debug --disable-x86asm
make -j4
```

- `--disable-x86asm`：手上没有 nasm 时的稳妥选择。如需手写汇编优化，先 `brew install nasm`
  并去掉该参数——二进制哈希会变化，需同步更新审计表条目。
- 产物 `./ffmpeg` 为 Mach-O x86_64，含 `h264_videotoolbox` 编码器。

## 构建桌面端（Intel Mac）

```bash
cd pc
export PET_MANAGER_FFMPEG_BIN=/path/to/ffmpeg
export PET_MANAGER_FFMPEG_README=/path/to/ffmpeg.README        # 需含 --disable-gpl / --disable-nonfree / h264_videotoolbox
export PET_MANAGER_FFMPEG_LICENSE=/path/to/COPYING.LGPLv2.1     # 源码包内 LGPL 文本
export PET_MANAGER_FFMPEG_SOURCE_ARCHIVE=/path/to/ffmpeg-8.1.2.tar.xz
npm install
npm run build:mac
```

产出：`src-tauri/target/release/bundle/dmg/Pet Manager_<version>_x64.dmg`

## 审计哈希说明

`macos-x64` 条目对应按上述配方构建的二进制：

- FFmpeg 8.1.2，LGPL-only，启用 VideoToolbox，`--disable-x86asm`
- 二进制 SHA-256：`d0afebc3542bb65d4bcf94e185878482066fc5ca899c4ae6a7e1aeaeae96d680`
- 构建环境：Intel Mac（x86_64-apple-darwin，Apple clang）

不同 Xcode/clang 版本可能产出不同哈希。维护者若以自己的构建为准，直接替换
`auditedFfmpegBinarySha256["macos-x64"]` 即可；README / LICENSE / SOURCE_ARCHIVE
的合规四件套要求不变。
