# petui P4 组件契约

本文件描述当前 Pet Manager 与 ESP32-P4 运行时共同接受的 `.clawpkg` 格式。生成器、校验器和发布器必须遵循同一契约。

本文中的 JSON 均为字段级说明片段，不是可发布模板。新组件必须从用户需求推导状态、实体、规则、布局和参数；禁止复制内置组件、历史生成包或其他完整 `.clawpkg` 后改名。

## 1. 目标设备

- 逻辑画布：`640x480` 横向。
- 统一组件运行时：`p4-bounded-runtime-v3`。`game` 与 `tool` 共用变量、状态、计时、Dashboard 和输入解释器。
- 可选通用场景子系统：`p4-grid-scene-v1`，用于网格、实体、移动、碰撞和边界玩法。
- `blocks`、`snake`、`flappy` 仅是历史原生预设，供旧包兼容，不是新组件可选的三种引擎。
- 默认不假设触控可用。只有能力文件明确给出 `touchInput.ready=true` 时才允许触屏事件。
- 第三方组件不能执行任意代码，也不能自行访问网络或本地文件。

可向校验器传入设备能力 JSON：

```json
{
  "widgetRuntime": "p4-bounded-runtime-v3",
  "widgetScene": "p4-grid-scene-v1",
  "widgetGamePresets": ["blocks", "snake", "flappy"],
  "touchInput": { "ready": false }
}
```

没有提供能力文件时，校验器使用上面的当前 P4 产品能力。

## 2. 包结构

组件目录必须包含：

```text
<component-id>/
├── component.json
├── negative-screen.json
├── buttons.json
├── runtime/
│   └── widget.json
├── assets/
│   └── .keep
└── share.json
```

- `component.json.id` 匹配 `^[a-z][a-z0-9_-]{0,46}$`。
- `component.json` 必须包含非空 `id`、`name`、`version`、`description`，以及 `kind: "game" | "tool"`。
- `negative-screen.json.dashboard` 是初始预览，必须与 runtime 的初始 state/page/vars 相符。
- `share.json` 必须是 JSON 对象，至少包含非空 `title`。
- 允许在 `assets/` 放运行时支持的静态资源；禁止 JS、Python、HTML、CSS、SVG、shell、PowerShell、批处理和可执行文件。
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
| `progress` | 64 | `{value,label}` 或 runtime `pct_of` |
| `visualStyle` | 16 | 新包使用 `pixel|clean`；`classic` 仅兼容旧包 |
| `visualPalette` | 16 | `candy|sunset|mint|arcade|ocean|forest|ember|mono` |
| `visualLayout` | 16 | `arcade|scoreboard|tool` |
| `visualSprite` | 16 | 见下方安全图标 |

安全图标：

```text
target trophy star bolt coffee timer droplet gauge
blocks snake flappy mole-ready mole-left mole-center mole-right
```

规则：

- `tool` 使用 `visualLayout: "tool"`，黑色 LCD 底与单一强调色，通常只突出一个大指标。
- `game` 使用 `arcade` 或 `scoreboard`，主视觉服务玩法局面，而不是数据看板。
- 不在多个槽位重复同一事实。没有真实含义就省略 `eyebrow`、`note` 或 `badge`。
- CJK 通常每字 3 字节，emoji 通常 4 字节；所有上限按 UTF-8 字节计算。
- 动态值必须绑定 runtime var/formatter，不得把看似实时的数字硬编码进初始画面。

Runtime dashboard 只允许：

- 字面量字符串。
- `switch_state`。
- `switch_page`。
- `fmt_mmss` / `fmt_hms`。
- `var`。
- progress 专用 `pct_of`、`of_max`、`label`。

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

- SW1/SW2/SW3 只允许短按，禁止任何 `long_press` 或 `hold`。
- 触屏事件仅在 `touchInput.ready=true` 时允许。
- `buttons.json` 最多 8 条；每条必须有非空 `action`、`control`、`event`、`label`。
- `action` 唯一；物理事件槽位唯一；label 不超过 30 UTF-8 字节。
- 每个普通 action 必须出现在 `runtime/widget.json.transitions[*].on`，反向也必须成立。
- 新硬件的摇杆上、下分别使用 `joystick.up`、`joystick.down`；左、右为兼容历史组件继续使用 `knob.rotate_ccw`、`knob.rotate_cw`，中键继续使用 `button.encoder.*`。
- 四个方向语义不同时使用独立 action，`control` 写作“前方摇杆”。
- SW3 短按保留给设备全局返回，生成组件不得占用；摇杆中键长按默认不绑定。
- 历史包可能包含下面的兼容返回动作，新组件不要再生成：

```json
{
  "action": "page_main",
  "control": "前方旋钮",
  "event": "button.encoder.long_press",
  "label": "返回桌宠"
}
```

兼容 `page_main` 保留旧“前方旋钮”文案、不写 runtime transition，且最多一条。组件按钮只在组件打开时生效，不覆盖设备页面导航。

## 5. Runtime 结构

基础结构：

```json
{
  "schema_version": 1,
  "engine": "p4-bounded-runtime-v3",
  "vars": {},
  "states": ["idle"],
  "initial_state": "idle",
  "transitions": [],
  "tick": [],
  "dashboard": {}
}
```

- `schema_version` 固定为 `1`。
- `engine` 固定为 `p4-bounded-runtime-v3`；工具和游戏都必须声明，不按 `kind` 切换底层引擎。
- `vars` 必须始终是对象；无变量时写 `{}`，不得省略、写成数组或 `null`。
- 最多 8 个变量；变量名为 1-31 个 ASCII 字母、数字、`_`、`-`、`.`。
- 每个变量声明必须且只能含 `type` 与可选 `init`。禁止 `min`、`max`、`default`、`label` 或其他描述字段。
- `type` 只能是 `int|string`。`int.init` 必须是 `-1000000000..1000000000` 的整数；`string.init` 最多 63 个 UTF-8 字节。
- `states` 为 1-6 个唯一字符串，`initial_state` 必须存在于其中。
- `pages` 可省略；存在时为 1-4 个对象，每个对象必须且只能含 `id`，ID 为 1-23 个 ASCII 字母、数字、`_`、`-`、`.`。
- `initial_page` 必须引用已声明 page。
- `transitions` 最多 12 条，`tick` 最多 8 条。
- 每条 transition/tick 顶层 `set+inc` 最多 4 项；每个 `tick.then` 独立最多 4 项。
- `runtime/widget.json` 紧凑 JSON 不超过 4095 字节。
- `buttons.json` 紧凑 JSON 不超过 2047 字节。
- P4 第三方组件不声明任意 `fetchers` 或 `readers`。实时数据只能使用产品已经提供的 bridge 变量。

## 6. 通用 Scene

状态、计时、页面和 Dashboard 已能表达番茄钟、喝水提醒、问答、反应赛等组件。只有需求需要坐标、自动移动、实体碰撞、墙体或越界行为时，才在同一个运行时中增加 `scene`。`scene` 由有界 `tick_ms`、状态引用、可选分数引用、`grid`、`entities` 与 `rules` 组成；所有 ID、坐标、数量、速度和规则必须从当前需求独立推导，不从本文或其他组件取得默认玩法。

### Scene 边界

- `tick_ms` 为 100-2000；只有当前 state 等于 `active_state` 时自动推进。
- `result_state` 与 `score_var` 可省略；存在时分别引用已声明 state 与 `int` var。
- `auto_start: true` 仅在 `initial_state == active_state` 时允许；通常由开始 action 执行 `restart`。
- `grid.width/height` 各为 4-16。可选 `rows` 必须与高度一致，每行与宽度一致，只含色阶字符 `0..4`。
- `grid.solid_tones` 最多声明 4 个唯一色阶；移动实体不能穿过这些底图单元。
- 最多 12 个实体。实体 `id` 为 1-15 个安全 ASCII 字符；`x/y` 为 0-15，`width/height` 为 1-8，初始边界框必须在网格内。
- 可选 `shape` 为 `rect|player-ship|enemy-ship|bullet|star|paddle|ball`，省略时为 `rect`。形状在实体边界框内原生绘制，碰撞仍按完整边界框计算。
- `shape` 不是任意 sprite、SVG 或逐帧位图入口。题材需要固定列表之外的关键轮廓时，必须报告能力缺口，不得用 `rect` 或不相干形状冒充。
- 实体色阶 `tone` 为 1-4，速度 `vx/vy` 为 -4..4；`active`、`collidable` 为布尔值。
- 边界策略 `bounds`：`clamp|wrap|bounce|hide|stop`。默认 `clamp`。
- 最多 20 条 scene rule，每条 `do` 最多 4 个操作。所有数组和对象都按声明顺序执行。
- Scene 使用固定数组、受控形状和受控操作，不接受 JS、H5、脚本、表达式、逐帧位图、任意内存或网络访问。

### Rule 触发器

| `on` | 附加字段 | 触发时机 |
|---|---|---|
| 普通 action | 无 | 与 `transitions[*].on` 和 `buttons.json.action` 同名的设备输入 |
| `tick` | 无 | 每个 scene tick，在实体按速度移动前 |
| `collision` | `entity`,`with` | 两个可碰撞实体矩形相交 |
| `edge` | `entity`,可选 `edge` | 实体越过 `left/right/top/bottom`；`edge` 省略或 `any` 表示任意边 |
| `blocked` | `entity` | 实体移动被 `solid_tones` 阻挡 |

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

一个可交付小游戏必须形成“目标出现、玩家输入、即时反馈、局面变化、回合结算、重开”的完整闭环。优先组合通用状态机和 `scene`，不得从 Flappy、贪吃蛇、方块中挑一个冒充用户要求的玩法。

文案与 runtime 必须一致：声称移动/飞行时要有实体速度或移动操作；声称碰撞时要有 collision rule；射击/战机玩法还必须有持续运动的子弹与敌人实体、子弹命中敌人的 collision rule，以及由玩家 action 驱动的水平移动。Dashboard、分享文案和组件描述不得宣传 runtime 中不存在的机制。

旧包中的 `game.type=blocks|snake|flappy` 仍由固件兼容，但新生成组件禁止声明旧版 `game` 对象。维护旧包时，其 `tick_ms`、状态、分数变量和固定 action 表仍按旧契约校验。

## 7. Tool 路由

工具与小游戏使用同一个 `p4-bounded-runtime-v3`；通常只需 vars/state/transition/tick/dashboard，不要为了“使用引擎”而添加空 scene。工具必须回答：默认展示什么、数据从哪里来、空/断连时显示什么、每个动作带来什么可见变化。

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
