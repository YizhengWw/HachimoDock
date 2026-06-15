# 开源合规准备记录

## 当前建议

- 许可证：采用 `AGPL-3.0-only`。桌面端 bridge 衍生自 AGPL-3.0 的 [Clawd on Desk](https://github.com/rullerzhou-afk/clawd-on-desk)，AGPL 强 copyleft 不能降级为 GPL，故整库统一 `AGPL-3.0-only`。
- 版权署名：使用作者实名 `Copyright (C) 2026 Yizheng Wang`（GitHub [@YizhengWw](https://github.com/YizhengWw)）。实名比昵称在版权主张上更可执行。
- 交付口径：Windows 安装包、桌面端源码、设备端源码、构建说明和第三方许可证清单需要作为同一批开源材料准备。

## 依据

- GNU AGPLv3 官方文本：https://www.gnu.org/licenses/agpl-3.0.en.html
- GNU “How to use GNU licenses” 建议在源码中放版权声明和复制许可说明：https://www.gnu.org/licenses/gpl-howto.html
- SPDX 对 AGPLv3-only 的标准标识是 `AGPL-3.0-only`：https://spdx.org/licenses/AGPL-3.0-only.html
- npm `package.json` 的 `license` 字段使用 SPDX 表达式：https://docs.npmjs.com/cli/v10/configuring-npm/package-json#license
- Cargo manifest 的 `license` 字段使用 SPDX 2.1 license expression：https://doc.rust-lang.org/cargo/reference/manifest.html#the-license-and-license-file-fields

## 已落库

- `LICENSE`：GNU AGPL v3 官方全文。
- `COPYRIGHT`：仓库级版权声明。
- `README.md`：补了 License 入口。
- `ref/package.json` / `ref/package-lock.json`：补 `author` 与 `AGPL-3.0-only`。
- `ref/src-tauri/Cargo.toml`：补 `license = "AGPL-3.0-only"`。
- `ref/src-tauri/bridge/package.json` / `package-lock.json`：从 `MIT` 改为 `AGPL-3.0-only`。
- bridge 内部 package：补 `author` 与 `AGPL-3.0-only`。

## 开源委员会前需要确认

1. 版权主体已确定为作者实名 `Yizheng Wang`。如果项目后续归属公司或组织，需改为对应法人/组织名称。
2. 许可证已确定为 `AGPL-3.0-only`（因 bridge 衍生自 AGPL-3.0 的 Clawd on Desk）。如需自动适配 FSF 未来版本可改 `AGPL-3.0-or-later`。
3. Windows 安装包发布时，需要同步提供对应源码、构建说明和 AGPL 全文。
4. 需要生成第三方依赖许可证清单，并在安装包或发布页随包提供。
5. 若包含非代码素材、图标、音频、模型、宠物动画等资源，需要确认这些资源是否同样按 AGPL 发布，或补单独素材许可证。

## 依赖许可证初查

npm 锁文件中主要是 MIT、BSD、ISC、Apache-2.0、0BSD、BlueOak-1.0.0、WTFPL、Python-2.0 等宽松许可证。按 AGPLv3 口径，一般可以继续随包分发，但需要保留第三方版权和许可证文本。

建议后续补一个自动生成步骤：

```sh
cd ref
npm install --package-lock-only --ignore-scripts
npx license-checker-rseidelsohn --production --json > ../docs/third-party-licenses-npm.json

cd src-tauri
cargo metadata --locked --format-version 1 > ../../docs/third-party-licenses-cargo-metadata.json
```

如果委员会要求更正式的 Rust 报告，可以引入 `cargo-about` 或 `cargo-deny` 生成 allowlist。
