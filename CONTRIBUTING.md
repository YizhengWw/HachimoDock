# Contributing / 贡献指南

感谢你关注 Hachimiao。我们欢迎 issue、discussion 和 pull request，也欢迎围绕硬件复刻、软件适配、形象素材、负一屏组件和文档改进展开社区协作。

## 贡献方向

- 修复 Pet Manager 或设备端运行时问题。
- 适配新的屏幕、主控板、结构件或整机形态。
- 创作宠物形象、动画素材、字幕样式和提示音。
- 开发新的 `.clawpkg` 负一屏组件。
- 补充装配教程、烧录说明、采购说明和故障排查。
- 改进 Agent 状态协议和第三方 CLI Agent 接入。

## 许可证与开源义务

本项目采用软硬件分层授权：

- 软件代码：`GPL-3.0-only`
- 硬件设计：`CERN-OHL-S-2.0`
- 3D 结构件、宠物素材和第三方资源：以对应文件、模型包或资源说明为准。

这两类主许可证都属于 reciprocal / copyleft 风格：如果你基于本项目开源内容进行修改并对外分发，需要按对应许可证公开你的修改内容。这个义务不等于必须把修改提交回本仓库；你可以维护自己的 fork、派生项目或产品仓库，只要满足相应许可证的公开与保留声明要求。

## Pull Request 流程

1. 先开 issue 或 discussion 描述较大的设计、硬件改动或许可证相关改动。
2. 小修复可以直接提交 PR，但请保持改动聚焦。
3. PR 说明里写清楚：
   - 改了什么；
   - 为什么需要改；
   - 如何验证；
   - 是否涉及硬件文件、素材、第三方依赖或许可证边界。
4. 如果改动包含图片、模型、音频、视频或第三方资源，请注明来源和授权。
5. 对硬件相关改动，请尽量附上实物验证、打样信息、BOM 差异或装配影响。

## CLA 流程

正式开放外部贡献后，项目计划搭建 Contributor License Agreement（CLA）流程，用于合规接受外部开发者贡献。CLA 的目的不是替代开源许可证，也不是要求贡献者必须把所有派生修改提交回本项目，而是确认贡献者有权提交对应内容，并授权项目在当前开源许可证体系下接收、维护和分发这些贡献。

在 CLA 流程上线前，维护者可能会暂缓合并涉及核心代码、硬件工程、结构件、素材或专利/商标风险的较大外部贡献。CLA 上线后，外部 PR 可能需要通过 CLA bot 或等效流程完成签署后再合并。

## English Summary

Hachimiao welcomes issues, discussions, and pull requests.

Main outbound licenses:

- Software code: `GPL-3.0-only`
- Hardware design: `CERN-OHL-S-2.0`
- 3D files, pet assets, and third-party resources: follow the notices shipped with those files.

The software and hardware licenses are reciprocal/copyleft-style licenses. If you distribute modified versions based on this project, you need to publish the corresponding modifications under the applicable license. This does not mean you must submit those changes back to this repository; maintaining a fork or downstream project is fine as long as the license obligations are satisfied.

The project plans to introduce a CLA process before formally accepting external contributions at scale. The CLA is intended to confirm that contributors have the right to submit their contributions and that the project can receive, maintain, and distribute them under the project's license structure.
