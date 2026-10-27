# 每日新知内容构建

组件 ID 保持 `hachimi-learn`，只是显示名称从「每日一知」改为「每日新知」。
`editorial-sample.json` 是十类各一条的问答式编辑样稿，尚未替换正式
`catalog.json` 的一千条内容。不得把十条试写描述为全量改写完成。
样稿沿用相同 ID，发布时应继承原条目的来源、修订号和 CC BY-SA 许可，
并在包内说明改写，不能标成未修改的百科原文。

编辑方向：具体好奇问题、自然生活场景、准确解释、贴切但有边界的类比，
澄清误会而不硬套统一开头或结尾。每条不少于300个汉字；不编造实验、
对话、数据或医疗/投资建议。人工编辑修正模型草稿，不能以结构检查
代替事实核对。

`topics.json` 与 `supplements.json` 是选题清单，不是随设备发送的内容。
`catalog.json` 为完整构建产物：十个分类各100条，合计1000条来源唯一的百科节选，
每条至少300个汉字（标点、拉丁字母不计入），保留正文、署名、来源版本与许可证。

```sh
python pc/scripts/build-knowledge-corpus.py --cache /absolute/build-cache --output pc/content/knowledge/catalog.json
```

运行时和资源包不会联网抓取百科；PC 宿主仅解释资源包中的受控 JSON。
完整正文只保留在 PC，设备按页收取，阅读状态保存在本机私有存储。
导入器需要 `mwparserfromhell==0.7.2`、`opencc-python-reimplemented==0.1.7`。
原始响应、`report.json`、不足数量时的 `candidate.json` 留在指定构建缓存，
不随包发布。内容未达标时不会覆盖正式 catalog。

## 授权及编辑说明

知识正文来自中文维基百科贡献者，适用 CC BY-SA 4.0。
每条 `url` 指向来源修订版本，可从页面历史查看作者。
许可：https://creativecommons.org/licenses/by-sa/4.0/
正文按完整句子节选、规范段落空白；无可用纯文本时跳过含复杂公式/模板的段落，
原始修订文本经简体转换。没有生成式补写或重复段落填充。
词条是百科资料，不等于经过本项目专家审核；有争议或时效性内容请核对来源。
不作为医疗、心理诊断或投资建议。

配置、脚本和美术许可不改变这些第三方正文的 ShareAlike 要求。
发布包须保留 `THIRD_PARTY_NOTICES.txt` 以及每条来源和许可信息。
