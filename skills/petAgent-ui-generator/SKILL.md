---
name: petAgent-ui-generator
description: Generate petAgent 桌搭子 widget folders (.clawpkg format) from natural-language descriptions. Use when the user asks to "做个桌搭子组件 / petAgent 组件 / openclaw 组件 / 负一屏组件 / clawpkg / 文生组件 / 生成一个 widget" or pastes the widget contract and asks you to fill in a new widget.
---

# Generate a petAgent .clawpkg widget

## When to use

- 用户说"做一个 X 组件给我的 openclaw 桌搭子"
- 用户说"做一个负一屏组件"
- 用户提供 .clawpkg 规范并要求生成新组件
- 用户在当前目录是 `~/.openclaw/component-drafts/<ts>/` 或类似临时目录,且有 PROMPT.md

## How to use

1. **Read [references/slot-contract.md](references/slot-contract.md)** — 设备端 9 槽位字节预算 + 文件格式
2. **Read [references/prompt-template.md](references/prompt-template.md)** — 完整 prompt 模板和约束
3. **Read 1-2 of the [examples](references/examples/)** — 真实可装的样板:
   - `slack-off-countdown/` — 摸鱼倒计时
   - `tomato-clock/` — 番茄钟
   - `drink-reminder/` — 喝水提醒
4. **询问用户(如果还没说清):**
   - 组件想干啥?(一句话)
   - 负一屏要显示哪些数字、状态、数据来源?
   - 切到负一屏后的默认场景 / 默认状态是什么? 是否无操作也有价值?
   - 点击 `screen.region.tap` 和长按 `screen.region.long_press` 分别触发什么动作?
   - 9 个槽位或按钮配置有缺信息的(尤其 metricValue 这种动态值)就追问
5. **生成 6 个文件**到当前目录,组件 id 用 kebab-case:
   - `<id>/component.json`
   - `<id>/negative-screen.json` (初始 dashboard 10 槽位)
   - `<id>/buttons.json` (action ↔ 屏幕点击/长按功能绑定)
   - `<id>/runtime/widget.json` ⭐ (新:状态机 + tick + 可选 fetcher/reader,见下方专门小节)
   - `<id>/assets/.keep`
   - `<id>/share.json`
6. **告诉用户:** "已生成 `<id>/` 目录,拖到 HachimoDock（哈基米机）的 ComponentCenter 即可安装到设备。"

## 强制约束

- **9 槽位字节预算**(UTF-8 字节;CJK 1 字 = 3 字节;emoji 通常 4 字节):title 60 / eyebrow 90 / headline 156 / metricLabel 90 / metricValue 60 / metricUnit 30 / badge 12 / note 156 / footer 156
- **id** 用 kebab-case,3-40 字符,小写字母 + 数字 + 连字符,首字符必须是字母
- **每个槽位**只能填用户实际提供的信息 + 通用占位符 + 通用状态词。**用户没说的具体业务上下文一律不准编**(见下方"反虚构原则")
- **不要**生成 JS/Python 代码逻辑(.clawpkg 是声明式的,runtime/widget.json 只写状态机数据)
- **不要**在 negative-screen.json 中放未声明的槽位(parser 会报 "未知槽位")

## 按钮配置追问规则

HachimoDock（哈基米机）负一屏的产品模型是:切到负一屏就是进入这个组件场景。组件必须有一个无操作也成立的默认状态(自动计时、自动刷新展示、显示当前状态、或明确的待开始状态),不能依赖顶部红钮或旋钮触发后才有价值。

HachimoDock（哈基米机）组件中心只允许用户在安装前把每个组件动作绑定到屏幕点击或屏幕长按。顶部红钮不可用；前方旋钮旋转固定用于系统音量，不要写成 widget action。生成新组件时优先给出清晰默认绑定。

`buttons.json` 可写这些事件:
- `screen.region.tap` → 屏幕主动作,如开始/暂停、刷新、查看详情
- `screen.region.long_press` → 屏幕次动作,如重置、展开、清空、暂停提醒

生成前必须确认四件事:
- 切到负一屏后的默认场景 / 默认状态是什么；是否无操作也能持续展示或运行
- 点击和长按分别做什么；如果用户想用红钮或旋钮,说明这些硬件不用于组件动作,旋钮固定为音量
- 每个动作的中文 label 是什么
- 每个 action 是否在 `runtime/widget.json` 的 transitions 里有对应处理

如果用户没说清"进入负一屏后默认做什么"、"点击"和"长按"分别触发什么,或只说"加个按钮 / 支持点击 / 可以操作",先追问一句再生成。不要猜一个默认动作塞进 buttons.json。用户只说"红钮也要能用"时,说明红钮不可用,并请他改用屏幕点击/长按。

## ⚠️ 反虚构原则(anti-fabrication)

**.clawpkg 是静态包,设备端目前不会重新计算字段** —— 用户装到设备上看到的就是你写进 negative-screen.json 的那串字符串。所以**写进去的每一个字都会被用户当成是他自己定义的内容**。

### 反例

用户说:"做一个会议计时"
模型输出 `headline: "📅 设计评审 · 还有 30 分钟"` ← ❌ "设计评审" 和 "30 分钟" 都是凭空编的

用户说:"做一个 Token 消耗展示"
模型输出 `metricValue: "12,847"`、`note: "比昨日 +18%"` ← ❌ token 数和涨跌都是编的

用户说:"做个会议提醒"
模型输出 `eyebrow: "下一场:产品周会 @ 14:00"` ← ❌ 议题和时间都是编的

### 正例

用户说:"做一个会议计时" → 正确生成:
```json
{
  "title": "📅 会议计时",
  "eyebrow": "本场会议",
  "headline": "计时中",
  "metricLabel": "已用时长",
  "metricValue": "00:00",
  "metricUnit": "",
  "footer": "点击 开始/暂停 · 长按 重置"
}
```
每个字段都只描述"组件类型"或"通用状态"或"占位数字",没有引入用户没提的具体业务上下文。

### 判定准则

| 内容类型 | 准则 | 例子 |
|---|---|---|
| 组件类型词 | ✅ 可以(用户用了你才用) | "会议"、"番茄"、"喝水"、"Token" |
| 通用状态词 | ✅ 可以(描述组件 CATEGORY 的当前状态) | "计时中"、"专注中"、"待开始"、"已完成" |
| 占位数值 | ✅ 可以(作为初始显示) | `00:00`、`0`、`—`、`--`、空串 |
| 操作提示 | ✅ 可以(描述按钮绑定,不是业务事实) | "点击 开始/暂停 · 长按 重置" |
| 业务专有名词 | ❌ 用户没提就不许用 | "设计评审"、"项目 X"、"Q3"、"李总" |
| 具体时长 / 距离 / 数量 | ❌ 用户没给就不许写 | "30 分钟"、"还剩 2 小时 13 分"、"12,847" |
| 同比 / 环比 / 排名 | ❌ 用户没给数据来源就不许编 | "+18%"、"top 3"、"本周第 2" |
| 主观叙事 | ❌ 这不是设备能算的 | "最近很累"、"今天表现不错" |

### 关于本文档 worked example 的特殊说明

[references/examples/](references/examples/) 和 [references/prompt-template.md](references/prompt-template.md) 里的内置组件示例(`slack-off-countdown` "还有 2 小时 13 分"、`tomato-clock` "24:59"、`drink-reminder` 等)有大量具体数字 —— **这些是 demo,不是生成模板**。它们的作用是展示 schema 形状,不是给你抄业务内容。给新用户写组件时,所有具体数字都要按"反虚构原则"换成占位符。

### 不同槽位的"如果用户没给"规则

| 槽位 | 用户没给具体内容时写什么 |
|---|---|
| title | 写组件类型词,如 "📅 会议计时" / "💧 喝水提醒" |
| metricLabel | 写指标的类别,如 "已用时长" / "今日次数" |
| footer | 写按钮操作映射(基于 buttons.json),非业务内容 |
| metricValue | 写占位数字 `00:00` / `0` / `—` |
| headline | 写通用状态 "计时中" / "进行中" / 留空 `""` |
| note | 用户没给上下文就留空 `""` |
| eyebrow | 没明确分类标签就留空 `""` |
| badge | 没明确编号/计数就留空 `""` |
| metricUnit | 跟着 metricValue 走,纯计时类一般留空 |
| progress | 没明确进度概念就**不要加这个字段** |

**追问优先于编造**:如果模型判断某个字段缺关键事实(用户想盯哪种数据 / 想用什么时长),应该追问一句"X 想显示啥?"而不是猜一个塞进去。

### 让组件好玩起来(强烈推荐)

设备屏渲染支持 emoji 和进度条,生成时主动加上:

- **title**:开头放主题 emoji,如 `"🌅 摸鱼倒计时"` / `"🍅 番茄钟"` / `"💧 喝水提醒"` / `"🎯 目标追踪"`
- **headline**:开头放状态 emoji,如 `"⏰ 还有 2 小时 13 分"` / `"🔴 专注中 · 第 2 轮"` / `"✅ 已完成 8 项"`
- **note**:开头放情绪/类别 emoji,如 `"🔥 本周已坚持 5 天"` / `"🥤 今天已喝 4 次"` / `"📊 同比 +12%"`
- **progress(可选)**:除 9 槽位外还可加 `progress: { value: 0-100, label: "...短标签" }` 字段,设备端会渲染一条进度条 + 百分比

emoji 选择参考表:
| 主题 | 推荐 emoji |
|---|---|
| 时间 / 计时 | ⏰ ⏱️ ⌛ ⏳ |
| 工作 / 专注 | 🍅 🎯 💼 🧑‍💻 |
| 健康 / 饮水 | 💧 🥤 🍵 |
| 财务 / 数字 | 💰 💵 📈 📊 |
| 学习 / 进步 | 📚 ✏️ 🎓 |
| 庆祝 / 完成 | ✅ 🎉 🏆 ⭐ |
| AI / 工具 | 🤖 ⚡ 🔧 |
| 心情 / 节奏 | 🔥 🌅 🌙 ☕ |

不要堆砌 emoji,每个槽位 1 个就够。

## 文件 schema 速查

`component.json`:
```json
{
  "id": "<id>",
  "name": "<中文显示名>",
  "version": "1.0.0",
  "author": "<用户名 或 anonymous>",
  "description": "<一句话说明>"
}
```

`negative-screen.json`:
```json
{
  "dashboard": {
    "title": "🍅 番茄钟",
    "eyebrow": "当前阶段",
    "headline": "🔴 专注中 · 第 2 轮",
    "metricLabel": "剩余时间",
    "metricValue": "24:59",
    "metricUnit": "",
    "badge": "2",
    "note": "🎯 本轮目标:写完登录页",
    "footer": "点击 开始/暂停 · 长按 重置",
    "progress": { "value": 4, "label": "本轮进度" }
  }
}
```

`progress` 字段为可选,`value` 范围 0-100,`label` 短文本(≤8 字符);客户端预览和设备屏都会渲染一条进度条。

`buttons.json`(action ↔ 屏幕点击/长按功能绑定,每个数组元素):
```json
{
  "action": "<动作 id, e.g. timer.start_pause>",   // 唯一抽象 id, widget.json 用这个名字
  "control": "屏幕区域",
  "event": "screen.region.tap" | "screen.region.long_press",
  "label": "<UI 中文说明>"
}
```

⚠️ **buttons.json 是"按钮功能绑定层",widget.json 的 transitions.on 用 `action` 名(不是 `event` 名)。** 这样用户在客户端改按钮功能时,只改 buttons.json 的 control/event/label,widget.json 的状态机 action 不动。

`share.json`:
```json
{ "title": "<同 component.json.name>", "summary": "<分享卡片摘要>" }
```

## ⭐ runtime/widget.json — 让 widget 真的跑起来

**重点新增。** v1 .clawpkg 只有静态 dashboard,设备装上就是张图。**v2 加 runtime/widget.json,描述状态机 + 周期 tick + 输入响应**,装到设备上真的能跑(按钮启停 / 计数器跳 / 多页切换)。

设备端有通用 Python 解释器 `board-widget-runtime`(已部署),按 widget.json 跑你定义的状态机。**你只生成声明数据,不生成代码**,grammar Turing-incomplete,所有合法操作都是受限的固定形状。

### widget.json 顶层 key

```json
{
  "schema_version": 1,
  "vars":          { "name": { "type": "int|string", "init": <default> } },
  "states":        ["state_a", "state_b", ...],
  "initial_state": "state_a",
  "pages":         [{ "id": "page_a", "label": "..." }, ...],   // 可选,多页 widget
  "initial_page":  "page_a",                                     // 有 pages 则必填
  "transitions":   [{ "from": "state|*", "on": "action", "to"?, "set"?, "inc"?, "page"? }],
  "tick":          [{ "every_ms": >=100, "while_state"?, "set"?, "inc"? }],
  "fetchers":      { "id": { url, every_s>=30, parse, json_path, into } },  // 可选,HTTP 拉取
  "readers":       { "id": { path, every_s>=1, field_pattern?, into } },     // 可选,读本地白名单文件
  "dashboard":     { "<slot>": <rule> }                          // 怎么从 state/var 渲染 10 槽位
}
```

### dashboard 渲染规则(只 5 种 shape)

| 形状 | 含义 |
|---|---|
| `"字面量字符串"` | 直接显示 |
| `{ "switch_state": { "state_id": "...", ... } }` | 按当前 state 选 |
| `{ "switch_page":  { "page_id": "...", ... } }` | 按当前 page 选 |
| `{ "fmt_mmss": "var_name" }` / `{ "fmt_hms": "var_name" }` | 把整数秒数格式化 MM:SS / H:MM:SS |
| `{ "var": "var_name" }` | 把变量按字符串渲染 |
| `{ "pct_of": "var_a", "of_max": "var_b", "label": "..." }` | **(仅用于 progress 槽位)** 算 var_a/var_b 百分比,设备端渲染进度条 + 标签 + N% |

`pct_of` 例子(喝水提醒用 since_last_min/interval_min 显示距下次提醒的进度条):
```json
"progress": { "pct_of": "since_last_min", "of_max": "interval_min", "label": "本次间隔" }
```
设备端 widget runtime 自动算 `min(100, var_a * 100 / var_b)` 并 emit `"<pct>:<label>"` 字符串,fb-stats-renderer 画一条橙色填充条 + 右侧 N%。

### ⭐ "10 槽位密度" 风格规范(强烈推荐)

设备端 LCD 320×240 视觉空间紧但留白也吓人。**金标参考 token-usage**:10 个槽位全部填满,emoji 点缀,metric panel 内部三行(label/value/note),底部带 progress bar。这样 widget 视觉密度刚好。

**强制最低密度**(skill 生成时必须达到):
- `title` ✓ 必须(带 emoji,主题图标)
- `metricLabel` + `metricValue` ✓ 必须(panel 主信息)
- `footer` ✓ 必须(按钮提示)
- `headline` ✓ 强烈推荐(switch_state 带状态 emoji)
- `eyebrow` ⭕ 推荐(小号上下文标签)
- `note` ⭕ 推荐(panel 第三行,补充说明 / 操作提示)
- `badge` ⭕ 推荐(右上角计数,如累计次数 — 值为 `"0"` 时 renderer 自动不画)
- `progress` ⭕⭕ **强烈推荐用 pct_of**(只要有"% 推进感"的概念,比如计时类的 elapsed/target,饮水类的 since_last/interval,token 类的 used/budget)

**风格规则:**
- title 用绿色 pill 渲染(自动),所以 title 文字别太长,12 个字符以内最好,前缀 emoji 1 个
- headline 是大字橙色,有 text-shadow,**用 switch_state 区分**(idle/working/done 等),emoji 强调状态(⏰ 🔴 ⏸ ✅)
- metricValue 是 panel 主角,**绑 var 让它真的会跳**(写字面量 "00:00" 是死的)
- note 是 panel 第三行小灰字,放固定操作提示(如 "📊 长按暂停")或单个 var 显示
- footer 居中 + 虚线分隔,**用 switch_state 区分**让每个状态有不同操作提示 — 用"点击 / 长按"开头最清楚

**例:drink-reminder 的 dashboard(参考实现)**:
```json
"dashboard": {
  "title":       "💧 喝水提醒",
  "eyebrow":     "距离上次喝水",
  "headline":    { "switch_state": { "reminding": "🥤 别忘了补水", "paused": "⏸ 提醒已暂停" } },
  "metricLabel": "已隔",
  "metricValue": { "var": "since_last_min" },
  "metricUnit":  "分钟",
  "badge":       { "var": "drink_count_today" },
  "note":        "📊 长按暂停提醒",
  "footer":      { "switch_state": {
                     "reminding": "点击 我喝了 · 长按 暂停",
                     "paused":    "长按 恢复提醒" }},
  "progress":    { "pct_of": "since_last_min", "of_max": "interval_min", "label": "本次间隔" }
}
```

### 完整例子:会议计时

```json
{
  "schema_version": 1,
  "vars": {
    "elapsed_s": { "type": "int", "init": 0 },
    "target_s":  { "type": "int", "init": 1500 }
  },
  "states": ["idle", "running", "paused"],
  "initial_state": "idle",
  "transitions": [
    { "from": "idle",    "on": "timer.start_pause", "to": "running" },
    { "from": "running", "on": "timer.start_pause", "to": "paused"  },
    { "from": "paused",  "on": "timer.start_pause", "to": "running" },
    { "from": "*",       "on": "timer.reset",       "to": "idle", "set": { "elapsed_s": 0 } }
  ],
  "tick": [
    { "every_ms": 1000, "while_state": "running", "inc": { "elapsed_s": 1 } }
  ],
  "dashboard": {
    "title":       "📅 会议计时",
    "eyebrow":     "本场会议",
    "headline":    { "switch_state": { "idle": "未开始", "running": "计时中", "paused": "已暂停" } },
    "metricLabel": "已用时长",
    "metricValue": { "fmt_mmss": "elapsed_s" },
    "footer":      { "switch_state": {
                       "idle":    "点击 开始 · 长按 重置",
                       "running": "点击 暂停 · 长按 重置",
                       "paused":  "点击 继续 · 长按 重置" }}
  }
}
```

配套 `buttons.json`(action 跟 widget.json 对得上):
```json
[
  { "action": "timer.start_pause", "control": "屏幕区域", "event": "screen.region.tap",        "label": "开始/暂停" },
  { "action": "timer.reset",       "control": "屏幕区域", "event": "screen.region.long_press", "label": "重置" }
]
```

### widget.json 生成准则

- **每个 button binding 都要有对应 transition**;反过来 transitions[*].on 提到的 action 也必须出现在 buttons.json 里
- **states 名字用动词式英文小写**(`idle` / `running` / `paused`),避免中文/空格 — 它是 id 不是 UI 文本
- **vars 只能 int 或 string**,int 用于计数/秒数,string 用于显示文本
- **tick.every_ms ≥ 100**;真做"每秒跳"用 `1000`
- **dashboard.metricValue 写 `{ "fmt_mmss": "elapsed_s" }`,不要写 `"00:00"` 字面量** — 字面量永远不变,绑 var 才会随 tick 跳
- **没有 if/else / 算术 / 字符串拼接** — 用 states + transitions 表达分支
- **没有 fetcher 别加 `"fetchers": {}`** — 整个 key 省掉
- **多页才加 `pages`**,单页 widget 不要为了凑数加

### 反虚构原则仍生效

跟 dashboard 一样,**widget.json 里不准编用户没提的具体业务事实**:
- 用户说"做个会议计时" → vars `elapsed_s: 0`、`target_s: 1500` (25分钟通用值) OK
- ❌ 不准编 `vars.meeting_topic.init: "设计评审"` 或 `target_s: 1800` 然后写 "本场设计评审 30 分钟"

### 可选 fetchers / readers(进阶)

**fetchers** 让 widget 周期性拉 HTTP API,把结果写进 var。**注意**:URL 必须在 Pi 的 `widget-runtime.conf` 白名单里才会真发请求,否则装上去也是 no-op。skill 可以生成 URL,但要告诉用户:"装好后还要去 `/opt/board-runtime/widget-runtime.conf` 加 `api.github.com` 这一行,fetcher 才会启用"。

```json
"fetchers": {
  "github_prs": {
    "url": "https://api.github.com/repos/anthropics/claude-code/pulls?state=open",
    "every_s": 60,
    "parse": "json",
    "json_path": "$.length",
    "into": "pr_count"
  }
}
```

**readers** 读 Pi 本地白名单文件(`.stats-display` / `.token-stats` / `.current-speech` 等):
```json
"readers": {
  "token_today": {
    "path": ".token-stats",
    "every_s": 5,
    "field_pattern": "metric_value=(\\d+)",
    "into": "token_count"
  }
}
```

声明了 fetcher/reader 之后,对应 var 要在 vars 里声明 (`"pr_count": {"type":"int","init":0}`),并且 dashboard 用 `{"var": "pr_count"}` 显示出来才有意义。

## 检查清单(write 前自查)

- [ ] `id` 匹配 `^[a-z][a-z0-9-]{2,39}$`
- [ ] 10 槽位每个值的 UTF-8 字节数都 ≤ maxBytes
- [ ] buttons.json 每条 action 都能在 widget.json 的 transitions 找到引用(反之亦然)
- [ ] runtime/widget.json 存在且 schema_version=1
- [ ] widget.json 的 states / initial_state / transitions.from / transitions.to 互相对得上
- [ ] widget.json 的 vars 引用都对得上(transitions.set/inc 的 key、tick.set/inc 的 key、dashboard 的 fmt_*/var 都引用真存在的 var)
- [ ] assets/.keep 存在(空文件即可),`runtime/` 目录下除了 widget.json 还要有 `.keep` 或就用 widget.json 自身
- [ ] dashboard 里没有未声明的 key (只能用那 10 个槽位名)
- [ ] 反虚构:dashboard 和 widget.json vars 都不准出现用户没提的具体业务名/数字
