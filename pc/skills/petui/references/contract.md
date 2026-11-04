# petui P4 组件契约

本文件描述当前 Pet Manager 与 ESP32-P4 运行时共同接受的 `.clawpkg` 格式。生成器、校验器和发布器必须遵循同一契约。

本文中的 JSON 均为字段级说明片段，不是可发布模板。新组件必须从用户需求推导状态、实体、规则、布局和参数；禁止复制内置组件、历史生成包或其他完整 `.clawpkg` 后改名。

## 1. 目标设备

通用横版跑跳、tile地图和图集能力单独见 [side-scene.md](side-scene.md)，
必须协商 `widgetSideScene=p4-side-scene-v1`；不改变旧grid-scene配额与语义。

应用级界面与原生 640×480 背景、图标、按钮和精细排版参见
[app-surface.md](app-surface.md)。只有显式声明 `p4-app-graphics-v1` 且设备支持的
应用包使用 32 节点/8191 字节配额；旧包、媒体、游戏配额不变。

本地整数/枚举表单参见 [app-form.md](app-form.md)，额外协商
`widgetAppForm=p4-app-form-v1`。最多3字段、每字段2–8选项或整数步进，
提交/草稿分离且六动作均需实体输入；不是任意文本键盘或动态列表。

- 逻辑画布：`640x480` 横向。
- 新组件统一运行时：`p4-bounded-runtime-v4`。`game` 与 `tool` 共用变量、状态、计时、Dashboard、输入解释器和受控精灵资源；v3 继续兼容历史包。
- 可选通用场景子系统：`p4-grid-scene-v2`，用于网格、语义形状、受控精灵、移动、碰撞和边界玩法；v1 继续兼容历史包。
- 可选连续单角色运动：`scene.motion.engine="p4-motion-v1"`，需设备 `widgetMotion` 与统一输入能力；固定步长重力、静态平台、按住蓄力/松开发射，见 [motion.md](motion.md)。不支持任意刚体物理。
- 连续轨道可在 motion 中声明 `track.engine="p4-platform-track-v1"`，需额外 `widgetPlatformTrack`；固定平台池复用、可配置间距/宽度/色阶与跟随镜头。不是每次落台复位主角，也不支持任意地形脚本。
- 可选受控触屏切果场景：`scene.engine="p4-touch-slice-v1"`，需要 `widgetPointer="p4-pointer-v1"` 与 `touchInput.ready=true`。配置、玩法、素材和实测边界见 [touch-slice.md](touch-slice.md)；它不使用通用网格场景的 entities/rules 字段。
- `blocks`、`snake`、`flappy` 仅是历史原生预设，供旧包兼容，不是新组件可选的三种引擎。
- `gomoku` 是唯一允许新建的原生预设：用户要求五子棋 / 五子对弈时使用 `game.type="gomoku"`
  （见第 6 节末），需设备 `widgetGamePresets` 含 `gomoku`；其他棋类或自定玩法仍走通用 `scene`。
- 默认不假设触控可用。只有能力文件明确给出 `touchInput.ready=true` 时才允许触屏事件。
- 第三方组件不能执行任意代码，也不能自行访问网络或本地文件。

可向校验器传入设备能力 JSON：

```json
{
  "widgetRuntime": "p4-bounded-runtime-v4",
  "widgetRuntimes": ["p4-bounded-runtime-v3", "p4-bounded-runtime-v4"],
  "widgetScene": "p4-grid-scene-v2",
  "widgetScenes": ["p4-grid-scene-v1", "p4-grid-scene-v2"],
  "widgetSprites": true,
  "widgetData": "p4-data-list-v1",
  "widgetMedia": "p4-media-v1",
  "widgetInstrument": "p4-instrument-v1",
  "widgetLyrics": "p4-lrc-v1",
  "widgetGamePresets": ["blocks", "snake", "flappy", "gomoku"],
  "touchInput": { "ready": false }
}
```

上方是基础能力示例，不是完整能力表。未提供能力文件时，仅使用校验器内置的
离线创作基线；这不能证明实体设备支持。实际同步始终使用 USB 报告的能力，
显式传入空能力表不会获得扩展能力。

## 2. 包结构

设备侧 JSON 文件（component.json、negative-screen.json、buttons.json、
runtime/widget.json）的字符串和键不能包含解码后的 NUL（U+0000）。
它不能由设备 C 字符串无损表示，校验失败时拒绝包，不截断、不自动修复。
普通换行与字面量反斜杠后接 u0000 不受影响；私有 PC JSON 和二进制
图片资源不是该限制的目标。设备 JSON 原文还必须为有效 UTF-8；字符串转义中的
UTF-16 代理项必须正确配对，不能用替换字符静默修复。Python/JS 对已解析值同样
拒绝孤立代理项，正常中文、合法 emoji 和普通转义文字保留。原生文本前置检查
不是完整 JSON 语法或重复键校验，后续解析和 schema 检查仍须执行。
设备入口另在递归解析前限制对象/数组容器路径最多 **32 层**，字符串中的括号和
标量叶子不增加层数。Python/JS 预览和 Rust 包校验使用相同边界；超出时拒绝，
不自动裁剪、清空或覆盖存档。私有 PC JSON 与二进制资源不套用设备容器界限。
这是输入上限，不代表完整栈调用图、实机高水位或时延已经通过。

### 原生本地音效工具

已实现的 `instrument.source=percussion.wooden-fish` 需要独立能力
`widgetInstrument=p4-instrument-v1`，详见 [instrument.md](instrument.md)。
这是受控原生工具表面：图层、音效合成和调度在产品内实现，并非扩大 scene 精灵配额。
生成包不得携带任意脚本、音频 URL、WAV 或高分辨率未引用图片。

组件目录必须包含：

```text
<component-id>/
├── component.json
├── negative-screen.json
├── buttons.json
├── runtime/
│   └── widget.json
├── assets/
│   ├── .keep
│   └── <optional-horizontal-sprite-sheet>.png
└── share.json
```

- `component.json.id` 匹配 `^[a-z][a-z0-9_-]{0,46}$`。
- `component.json` 必须包含非空 `id`、`name`、`version`、`description`，以及 `kind: "game" | "tool"`。
- 游戏 `description` 使用 1-2 个简短句子说明目标、核心规则和结束条件，不能只写题材或宣传语。组件中心会根据 `buttons.json` 另行展示实际操作方法，所以 description 不写死允许用户改绑的物理键位。
- `negative-screen.json.dashboard` 是初始预览，必须与 runtime 的初始 state/page/vars 相符。
- `share.json` 必须是 JSON 对象，至少包含非空 `title`。
- 允许在 `assets/` 放运行时支持的静态 PNG 资源；单个 sprite sheet 不超过 128 KiB。禁止 JS、Python、HTML、CSS、SVG、shell、PowerShell、批处理和可执行文件。
- `assets/` 只允许 `.keep` 与最终发布用 PNG。游戏的每个 PNG 必须被 `runtime/widget.json scene.sprites` 精确引用；媒体切图必须被受控 `surface.images` 精确引用；不得遗留未使用的候选图、单帧临时图、提示词或把 JPEG/WebP 改为 `.png`。
- 包内不得包含符号链接。

## 3. Dashboard 槽位

槽位是可选渲染原语，不是必须填满的表单。只保留对当前组件有意义的内容。

| 槽位 | 最大 UTF-8 字节 | 用途 |
|---|---:|---|
| `title` | 60 | 左上组件名 |
| `eyebrow` | 90 | 阶段、来源或分类 |
| `headline` | 156 | 当前状态或核心提示 |
| `metricLabel` | 90 | 指标名 |
| `metricValue` | 60 | 大号数字、时间或短状态 |
| `metricUnit` | 30 | 单位 |
| `badge` | 12 | 右上短计数 |
| `note` | 156 | 必要的补充事实 |
| `footer` | 156 | 当前可执行的 1-3 个动作 |
| `progress` | 64 | negative-screen 初始预览用 `{value,label}`；runtime 只能用 `{pct_of,of_max,label}`、字符串或 `{var}` |
| `visualStyle` | 16 | 新包默认 `clean`；用户明确要求复古/像素/街机时才用 `pixel`；`classic` 仅兼容旧包 |
| `visualPalette` | 16 | `candy|sunset|mint|arcade|ocean|forest|ember|mono` |
| `visualLayout` | 16 | `arcade|scoreboard|tool` |
| `visualSprite` | 16 | 见下方安全图标 |

安全图标：

```text
target trophy star bolt coffee timer droplet gauge music
blocks snake flappy mole-ready mole-left mole-center mole-right
```

规则：

- `tool` 使用 `visualLayout: "tool"`，黑色 LCD 底与单一强调色，通常只突出一个大指标。
- `game` 使用 `arcade` 或 `scoreboard`，主视觉服务玩法局面，而不是数据看板。
- `clean` 必须体现现代卡片层级、留白、圆角和语义轮廓；不能只是隐藏格子缝隙。可见像素网格、方块化主体和 8-bit 装饰只属于用户明确要求的 `pixel` 风格。
- 不在多个槽位重复同一事实。没有真实含义就省略 `eyebrow`、`note` 或 `badge`。
- CJK 通常每字 3 字节，emoji 通常 4 字节；所有上限按 UTF-8 字节计算。
- 动态值必须绑定 runtime var/formatter，不得把看似实时的数字硬编码进初始画面。

Runtime dashboard 的每个槽位只允许下面几种写法，这是固件 `parse_dashboard` 的原样文法，PC 校验器、预览和安装前校验按同一文法拒绝，其他任何形态到设备都会以 `dashboard value rule is unsupported` 失败：

- 字面量字符串。
- 值规则对象，且只含一个键：`{"var": "名"}`、`{"fmt_mmss": "int变量"}`、`{"fmt_hms": "int变量"}`。变量必须在 `vars` 中声明，formatter 需要 `int` 变量。
- `{"switch_state": {"state": 分支}}` / `{"switch_page": {"page": 分支}}`：只能出现在槽位顶层，键名必须是已声明的 state/page，最多 6 个分支；**每个分支只能是字符串或上面的单键值规则，不能再嵌套 switch、progress 或任何其他对象。** 需要同时按页和按状态变化时，用 `var_bind`/`switch_page` 搭配 transition 写入变量，不要嵌套。
- 仅 `progress` 槽位：`{"pct_of": "int变量", "of_max": "int变量", "label": "可选字符串"}`，不接受其他键。runtime 里不能写 `{value,label}`，那只属于 `negative-screen.json` 的初始预览。

不接受的例子：`{"switch_page": {"home": {"switch_state": {...}}}}`、`{"text": "..."}`、`{"var": "n", "suffix": "个"}`、`{}`、runtime 里的 `{"value": 0, "label": "..."}`。

Dashboard 没有通用 if/else、字符串拼接、任意算术或 CSS。需要局面网格时使用第 6 节受限 `scene`，不得把逐帧像素数据塞进 Dashboard。

## 4. 输入与按钮

新组件允许的事件：

```text
button.sw1.short_press
button.sw2.short_press
button.sw3.short_press
button.encoder.short_press
button.encoder.long_press
knob.rotate_cw
knob.rotate_ccw
joystick.up
joystick.down
screen.region.tap
screen.region.long_press
```

- SW1/SW2/SW3 默认只允许短按，禁止 `hold`。唯一受控例外：audio.player 媒体组件的 media.lyrics 可绑定 button.sw1.long_press，需要 widgetLyrics=p4-lrc-v1；全局实时对话与退出绑定优先于组件动作。
- 触屏事件仅在 `touchInput.ready=true` 时允许。
- `buttons.json` 最多 8 条；每条必须有非空 `action`、`control`、`event`、`label`。
- `action` 唯一；物理事件槽位唯一；label 不超过 30 UTF-8 字节。
- 每个普通 action 必须出现在 `runtime/widget.json.transitions[*].on`，反向也必须成立。
- 摇杆事件使用下面的兼容事件名；`control` 统一写作“前方摇杆”：

| 实体输入 | `event` | 建议用途 |
|---|---|---|
| 上 | `joystick.up` | 上移、旋转、增加或上一项 |
| 下 | `joystick.down` | 下移、下落、减少或下一项 |
| 左 | `knob.rotate_ccw` | 左移、向左选择或上一页 |
| 右 | `knob.rotate_cw` | 右移、向右选择或下一页 |
| 中键短按 | `button.encoder.short_press` | 确认或不适合 SW1/SW2 的辅助动作 |
| 中键长按 | `button.encoder.long_press` | 只在用户明确要求时使用 |

- 四个方向语义不同时使用独立 action。只使用玩法真正需要的方向，不为填满输入而制造无效动作。
- 设备全局退出动作不属于组件包。出厂默认是 SW3 短按，但用户可以把退出配置到多个键或完全不配置；因此生成或更新组件时禁止绑定 `button.sw3.short_press`，也禁止声明 `page_main`、`page_back`、`page_enter`、`page_toggle`、`page_app` 或 `component_center` 等系统导航 action。
- 组件画面、footer、描述不得显示固定的“SW3 退出”，应使用“全局键退出”或“退出跟随设备全局设置”。
- 游戏默认把方向动作放到摇杆，把 SW1 映射为开始/重新开始；SW2 可按玩法定义为射击、技能、暂停等次要动作，也可以不用。
- 工具默认用 SW1 执行主操作、SW2 执行次操作；摇杆方向可用于切页、选择或数值调节。
- 用户在 Pet Manager 中修改全局退出绑定后，固件会让新的全局按键优先于组件动作。组件无需、也不得复制这条绑定。
- 历史包里的 `page_main/page_back` 记录只用于读取兼容；PC 下发时会移除，固件运行时也会忽略。优化历史组件时必须删除这些记录，并把原先占用默认 SW3 的普通动作迁移到 SW1、SW2 或摇杆。

## 5. Runtime 结构

基础结构：

```json
{
  "schema_version": 1,
  "engine": "p4-bounded-runtime-v4",
  "vars": {},
  "states": ["idle"],
  "initial_state": "idle",
  "transitions": [],
  "tick": [],
  "dashboard": {}
}
```

- `schema_version` 固定为 `1`。
- 新组件 `engine` 固定为 `p4-bounded-runtime-v4`；工具和游戏都必须声明，不按 `kind` 切换底层引擎。v3 仅用于读取或修复历史包。
- `vars` 必须始终是对象；无变量时写 `{}`，不得省略、写成数组或 `null`。
- 最多 8 个变量；变量名为 1-31 个 ASCII 字母、数字、`_`、`-`、`.`。声明 `quota: "p4-quota-v2"`
  （需设备 `widgetQuota`）后变量 16、状态 8、页面 6、transition 32、tick 12、switch 分支 8、
  surface 节点 48、deck 每组 24 项、`persist.vars` 16、无 graphics 的 runtime 也放宽到 8191 字节；
  详见 [quota.md](quota.md)。未声明时全部沿用本节的 v1 数字。
- 每个变量声明必须且只能含 `type` 与可选 `init`。禁止 `min`、`max`、`default`、`label` 或其他描述字段。
- `type` 只能是 `int|string`。`int.init` 必须是 `-1000000000..1000000000` 的整数；`string.init` 最多 63 个 UTF-8 字节。
- `states` 为 1-6 个唯一字符串，`initial_state` 必须存在于其中。
- `pages` 可省略；存在时为 1-4 个对象，每个对象必须且只能含 `id`，ID 为 1-23 个 ASCII 字母、数字、`_`、`-`、`.`。
- `initial_page` 必须引用已声明 page。
- `transitions` 最多 12 条，`tick` 最多 8 条。
- 启用 `p4-input-v1` 的包允许最多 24 条 transition。能力中的 `maxTransitions`
  保留旧包上限 12；新工具应读取 `maxInputTransitions`，不能把条件上限用于旧包。
- tick 的 `while_state` 省略时在所有状态运行；填写时必须引用已声明状态，
  不接受字面量 `"*"`。状态改变会重置 tick 时间基准，不补演之前的非活动时间。
  一次推进最多补演 10 步。PC 预览与技能模型使用同一语义用例集。
- 每条 transition/tick 顶层 `set+inc` 最多 4 项；每个 `tick.then` 独立最多 4 项。
- 声明 `logic: "p4-logic-v1"`（需设备 `widgetLogic`）后，transition 可带 `when` 条件与 `else` 分支，
  tick 的 `when` 可用 `op/value/all`，`inc` 可写 `{var}`，scene 规则可带 `when` 与 `var` 操作；
  主分支与 `else` 的 `set+inc` 合计仍不超过 4 项。详见 [logic.md](logic.md)。未声明时保持原契约。
- 声明 `sfx: {"engine": "p4-sfx-v1", "volume": 0-100, "cues": [...]}`（需设备 `widgetSfx`）后，transition、
  `tick[].then` 可带 `sfx: "<cue id>"`，scene 规则可用 `{"op": "sfx", "cue": ...}`。1-8 个 cue，每个只能选
  固定预设 `tap|beep|chirp|low|water|page`，不能带采样、文件或自定义波形；不与 `instrument`/`media`/`local`/
  `program` 混用。详见 [sfx.md](sfx.md)。
- `runtime/widget.json` 紧凑 JSON 不超过 4095 字节。
- `buttons.json` 紧凑 JSON 不超过 2047 字节。
- P4 第三方组件不声明任意 `fetchers` 或 `readers`。实时数据只能使用产品已经提供的 bridge 变量。

### 5.1 可选 PC 实时数据列表

仅支持 `widgetData: "p4-data-list-v1"` 的新固件可以在 v4 工具 runtime 中声明
`"data": {"source": "stocks.watchlist", "page_var": "list_page"}`。
`page_var` 必须引用已声明的 int 变量，初值 0 表示第一页；增减变量切页，固件按实际页数循环。
`source` 为 1–47 位小写 ASCII 字母、数字、点、短横线或下划线；data 仅允许这两个字段，
不能与 scene/game 混用。旧固件必须拒绝安装，不能假设所有 v4 设备都支持实时数据。

首个产品数据源为 `stocks.watchlist`：PC 组件中心管理最多 20 只自选股及顺序，
PC 保持运行并通过 USB 下发腾讯行情；组件不存放股票名单、Key、任意 URL 或请求脚本。
设备每页显示 5 行。通用行包含 label/value/detail/meta/tone；股票适配器分别映射
名称、价格、涨跌幅、代码币种及数据状态、正负色彩。当前行情顶部统一显示日期，行内不显示时刻；
只有 value/detail（股价/涨跌幅）按 tone 着色，label/meta 保持正常色。PC 详情保留原始行情时间。
其他来源必须先有真实产品适配器，
不能仅改 source 名就宣称支持天气等数据。

Dashboard 使用 clean/tool，提供 title/footer；初始预览使用“等待数据”，不填示例价格。
摇杆左右通过普通 transition 增减 page_var；可用 SW1 将其置 0 回到首页。
自动取价由 PC 后台服务负责，不用 runtime tick 模拟价格，不声明设备端手动刷新。
数据只存于设备 RAM，不反复写 flash；断连/PC 退出后最多 30 秒标记过期，保留最后结果。
上游失败与单只行情缺失同样明确标注，不把旧价格当实时价格。
行情时间来自数据源，不以请求时间冒充行情时间。
PC 数据源、刷新与验收边界见 [live-data.md](live-data.md)。实时列表也必须通过现有自测脚本的分页测试；
模拟通过不等于完成网络、断连恢复或真机显示验收。

### 5.2 可选 PC 音频播放器

新直播组件使用 `media={"source":"pc.<component-id>","mode":"live"}`、
包内 surface 与独立的 `pc/extension.json`，需要 `widgetMediaSurface=p4-media-surface-v1`。
完整布局、权限、搜索映射和持久化契约见 [pc-extensions.md](pc-extensions.md)。
PC 文件不下发设备，收藏和凭据不能随包分发；不接受歌词与进度跳转。

仅支持 `widgetMedia=p4-media-v1` 的设备接受 v4 tool 的
`"media":{"source":"audio.player"}`。media 只含 source，不能混用 data/scene/game。
旧固件必须拒绝安装，不能仅凭 v4 判断支持。
使用产品原生播放器界面及真实音频状态，详见 [media.md](media.md)。

## 6. 通用 Scene

状态、计时、页面和 Dashboard 已能表达番茄钟、喝水提醒、问答、反应赛等组件。只有需求需要坐标、自动移动、实体碰撞、墙体或越界行为时，才在同一个运行时中增加 `scene`。`scene` 由有界 `tick_ms`、状态引用、可选分数引用、`grid`、`entities` 与 `rules` 组成；所有 ID、坐标、数量、速度和规则必须从当前需求独立推导，不从本文或其他组件取得默认玩法。

### Scene 边界

- `tick_ms` 为 100-2000；只有当前 state 等于 `active_state` 时自动推进。
- `result_state` 与 `score_var` 可省略；存在时分别引用已声明 state 与 `int` var。
- `auto_start: true` 仅在 `initial_state == active_state` 时允许；通常由开始 action 执行 `restart`。
- `grid.width/height` 各为 4-16。可选 `rows` 必须与高度一致，每行与宽度一致，只含色阶字符 `0..4`。
- `grid.solid_tones` 最多声明 4 个唯一色阶；移动实体不能穿过这些底图单元。
- 最多 12 个实体。实体 `id` 为 1-15 个安全 ASCII 字符；`x/y` 为 0-15，`width/height` 为 1-8，初始边界框必须在网格内。
- 可选 `shape` 为 `rect|player-ship|enemy-ship|bullet|star|paddle|ball|circle|capsule|triangle|diamond|heart|cloud|coin|character`，省略时为 `rect`。形状在实体边界框内原生绘制，碰撞仍按完整边界框计算。
- 固定形状无法准确表达核心对象时，可在实体上用 `sprite` 引用本 scene 声明的受控 PNG sprite sheet。`sprite` 只影响显示，碰撞仍按实体完整边界框计算；它不是 SVG、脚本或任意代码入口。
- 实体色阶 `tone` 为 1-4，速度 `vx/vy` 为 -4..4；`active`、`collidable` 为布尔值。
- 边界策略 `bounds`：`clamp|wrap|bounce|hide|stop`。默认 `clamp`。
- 最多 20 条 scene rule，每条 `do` 最多 4 个操作。所有数组和对象都按声明顺序执行。
- Scene 使用固定数组、受控形状、受限 PNG 精灵和受控操作，不接受 JS、H5、脚本、表达式、任意内存或网络访问。

### 受控 Sprite sheet（新游戏核心元素 PNG 优先）

新游戏先为玩家/主角设计受控 PNG；配额允许时，主要敌人、目标或题材识别物也优先使用 PNG。内置 shape 主要用于平台、弹丸、金币、碰撞标记、装饰和本身就是极简几何的对象。不能因为生成方便而用矩形或方块代替题材主体。PNG 必须是透明背景横向帧带，画布宽度严格等于 `frame_width * frames`，高度严格等于 `frame_height`：

```json
{
  "sprites": [
    {
      "id": "hero",
      "asset": "assets/hero.png",
      "frame_width": 24,
      "frame_height": 24,
      "frames": 4,
      "fps": 8
    }
  ],
  "entities": [
    { "id": "player", "x": 4, "y": 6, "width": 2, "height": 2, "sprite": "hero" }
  ]
}
```

- 最多 4 个精灵；每个 1-8 帧；单帧宽高各 8-64；动画 1-20 fps。
- 所有精灵的全部解码帧合计最多 4096 像素；单个源 PNG 不超过 128 KiB。
- `id` 为 1-15 个安全 ASCII 字符；`asset` 只能是 `assets/<safe-name>.png`。
- 路径按大小写精确匹配。生成流程必须先把真实 PNG 写入 `assets/`，再声明 `scene.sprites`；禁止先写一个尚不存在的素材引用后直接发布。
- 横向帧带的每一帧都必须包含清楚可辨的主体。首帧表达默认状态；连续帧不得出现整帧透明、裁切、重影、主体尺寸骤变或重心大幅跳位，否则设备动画会表现为闪烁或模糊色团。
- PC 预览直接读取源 PNG；下发时会编译为 RGB565+alpha 并作为同一组件事务安装，固件重启后仍会从组件双代存储恢复。
- 不要为装饰性细节滥用精灵。简单球、胶囊、心形、云、金币、子弹和平台可优先使用内置 shape，以节省包体和运行内存；这条节省规则不能覆盖“玩家/主角和主要题材对象 PNG 优先”的要求。

### Rule 触发器

| `on` | 附加字段 | 触发时机 |
|---|---|---|
| 普通 action | 无 | 与 `transitions[*].on` 和 `buttons.json.action` 同名的设备输入 |
| `tick` | 无 | 每个 scene tick，在实体按速度移动前 |
| `collision` | `entity`,`with` | 两个可碰撞实体矩形相交 |
| `edge` | `entity`,可选 `edge` | 实体越过 `left/right/top/bottom`；`edge` 省略或 `any` 表示任意边 |
| `blocked` | `entity` | 实体移动被 `solid_tones` 阻挡 |

规则可选 `when`（仅 `logic=p4-logic-v1`，一条 int 条件）：条件为假时整条规则跳过。

### 受控操作

| `op` | 字段 | 作用 |
|---|---|---|
| `move` | `entity,dx,dy` | 立即移动，`dx/dy` 为 -4..4 且不能同时为 0 |
| `velocity` | `entity,vx,vy` | 设置每 tick 速度 |
| `accelerate` | `entity,vx,vy` | 累加速度，运行时仍钳制到 -4..4 |
| `place` | `entity` 加 `x/y` 或 `source` | 放置并显示实体；坐标可为整数或 `[min,max]` 随机范围，`source` 可配 `dx/dy` |
| `show` / `hide` | `entity` | 显示或隐藏实体 |
| `score` | `add` 或 `set` | 更新 scene 分数并同步 `score_var` |
| `run` / `stop` / `restart` | 无 | 开始、结束或重置并开始 scene |
| `bounce` | `entity,axis` | 按 `x|y|both` 反转速度 |
| `tone` | `entity,tone` | 切换实体 1-4 色阶 |
| `var` | `name` 加 `set` 或 `inc` | 仅 `logic=p4-logic-v1`：写一个 int 变量，-10000..10000 |
| `sfx` | `cue` | 仅 `sfx=p4-sfx-v1`：排队一个已声明的 cue，不改变画面，不算可见反馈 |

一个可交付小游戏必须形成“目标出现、玩家输入、即时反馈、局面变化、回合结算、重开”的完整闭环。优先组合通用状态机和 `scene`，不得从 Flappy、贪吃蛇、方块中挑一个冒充用户要求的玩法。

### 玩法自测发布门槛

新游戏在发布前必须运行：

```bash
python <skill-dir>/scripts/smoke_test_widget_game.py <component-dir>
```

测试器从 `buttons.json` 的真实 action 出发探索可达局面，并要求：开始后进入 active state；每个玩法输入在可达局面中产生即时可见反馈；tick/自动实体真实推进；声明的得分或进展可达；成功或失败结算可达；结算后可以重新开始。只改变 `vx/vy`、隐藏变量或文案而没有当刻画面反馈的动作不通过。发布器会在 staging 中再次运行同一测试，失败包不能进入正式组件库。

带 `scene` 的互动工具也运行同一测试命令，但不强制游戏结算：测试器要求每个声明输入在可达状态中产生即时可见反馈，并要求声明的 tick 或自动运动在互动后的可达状态中真实推进。这样桌宠、互动摆件等被归类为 `tool` 的组件也不能绕过行为自测。

文案与 runtime 必须一致：声称移动/飞行时要有实体速度或移动操作；声称碰撞时要有 collision rule；射击/战机玩法还必须有持续运动的子弹与敌人实体、子弹命中敌人的 collision rule，以及由玩家 action 驱动的水平移动。Dashboard、分享文案和组件描述不得宣传 runtime 中不存在的机制。

旧包中的 `game.type=blocks|snake|flappy` 仍由固件兼容，但新生成组件禁止声明旧版 `game` 对象。维护旧包时，其 `tick_ms`、状态、分数变量和固定 action 表仍按旧契约校验。

### 原生五子棋预设 `game.type="gomoku"`

用户要求五子棋（五子对弈、连珠）时，不用 `scene` 拼凑，而是声明原生预设；校验器要求目标能力 `widgetGamePresets` 含 `gomoku`，旧固件在传输前被拒绝。

- `game`：`{type:"gomoku", tick_ms:100-2000, playing_state, result_state, score_var, actions}`；`actions` 必须且只能包含 `place|left|right|up|down|undo` 六项，每项都要出现在 `transitions` 与 `buttons.json`。推荐映射：`place`→SW1，`undo`→SW2，`left/right`→旋钮逆/顺时针，`up/down`→摇杆上/下；SW3 为全局退出，不得绑定。
- 玩法：15×15 棋盘，玩家执黑先行，设备执白在下一个 tick 回应（确定性有界 AI，不接受自定义棋力参数）。`place` 在 `ready/result` 开始或重开，进行中落子；光标边界截停；`undo` 只撤回最近一回合。任一方 ≥5 连即结束，棋盘满为和棋。
- `score_var` 契约：进行中为盘面手数（黑白总数，奇数=白方待回应，偶数=轮到黑方）；结束后为 `结果×1000+手数`（1 黑胜 / 2 白胜 / 3 和棋）。可与 `logic:"p4-logic-v1"`、`sfx:"p4-sfx-v1"` 并用：transition 的 `sfx` 做落子/悔棋音，`tick[].then.sfx` 做节奏循环、回应音（`score gte {var}` 的偶数步）与结算音；固件 tick 与 sfx 在原生预设进行中照常运行。HUD 用 `state_mask` 区分 ready/playing/result，用 `show:{var:score,min,max}` 在 result 状态显示对应结果文案；不要宣传不存在的计时、段位或联网对弈。
- 画面：必须声明 `surface.game_view`（见 app-surface.md「原生五子棋画面」），棋盘线画在 640×480 JPEG 背景里并与视口格子对齐；游戏层只画棋子、光标、最近落子点与五连高亮。
- 自测：`smoke_test_widget_game.py` 通过同源 WASM 走开始 → 光标移动 → 落子 → 设备回应 → 悔棋 → 结算 → 重开。

## 7. Tool 路由

工具与小游戏使用同一个 `p4-bounded-runtime-v4`；通常只需 vars/state/transition/tick/dashboard，不要为了“使用引擎”而添加空 scene。工具必须回答：默认展示什么、数据从哪里来、空/断连时显示什么、每个动作带来什么可见变化。

- 计时器：用 `fmt_mmss/fmt_hms`；用户未给目标时可做正计时，不擅自编倒计时。
- 提醒器：区分计时、到点、暂停；确认完成后更新真实计数并重置间隔。
- Tracker：只记录用户实际输入，不声称不存在的历史、推断或跨设备同步。
- 多页工具：每页回答一个问题，使用 `switch_page` 改变主信息，不在页面间复制相同内容。
- Live telemetry：必须有已确认的数据源、刷新频率、空态和断连态；没有产品 bridge 时不可生成伪实时数据。

## 8. 反虚构与失败处理

- 只写用户提供的事实、通用状态和明确占位值。
- 数据未知时使用 `0`、`00:00`、`—` 或“等待数据”。
- 不复制示例中的时长、次数、Token 数、业务名称或趋势值。
- 需求不完整但不会改变行为时自行做保守设计；会改变核心行为时只追问最关键的一项。
- 校验失败的包不得发布。发布失败的目录不得出现在正式 library 中。

## 9. 正式发布目录

```text
~/.claw-pet/
├── components/
│   ├── .staging/
│   └── library/
│       └── <component-id>/
│           └── <version-hash>/
└── logs/
    └── component-generation/
        └── <job-id>.json
```

- `.staging` 只用于事务，不是“草稿库”。
- `<version-hash>` 是包内容的 SHA-256 短哈希，同样内容重复发布会复用同一正式版本。
- `component.json.id` 是组件的升级身份。优化或修复既有组件必须保留原 ID；显示名称相同但 ID 不同仍是两个组件。相同 ID 的新内容进入同一版本链，并由 Pet Manager 选取最新版本展示。
- 发布器先复制到 `.staging/<job-id>/package`、重新校验，再以同盘 rename 原子进入 `library`。
- Pet Manager 只扫描 `library`；“正式本地组件”和“设备已安装组件”是两个独立状态。
- 发布新版本只更新正式本地组件库，不会隐式写设备。用户需要在组件中心刷新后，打开同一组件卡片并执行“保存并同步”，才能覆盖设备上的旧版本。
