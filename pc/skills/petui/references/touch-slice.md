# 触屏切果：p4-touch-slice-v1

这是固件内置的受控玩法核心，不是组件可执行代码或任意物理引擎。仅适用于切水果、避炸弹、连击与限时结算这类明确需求；其他触屏游戏须先扩展宿主能力。

## 能力与安全边界

- 实际设备 capabilities.widgetScenes 包含 `p4-touch-slice-v1`、widgetPointer 为 `p4-pointer-v1`，touchInput.ready 为 true 才能安装。离线夹具只用于测试，不能当作设备已验收的证明。
- 顶层 engine 为 `p4-bounded-runtime-v4`，component.json.kind 为 `game`。
- 包内只有契约 JSON 和 PNG；不得附带 WASM、脚本或外部音频。宿主预览和自测自带的可信 WASM 与固件共用 C 核心，不从组件加载代码。
- 640×480 单指坐标，DOWN/MOVE/UP/CANCEL。只在前台组件持有本次触摸；多指、失联、过期样本和退出取消当前划线，不把离散触摸连成跨屏刀痕。系统全局退出和实时对话优先，离开或进入实时对话会暂停游戏。

## 精确配置

scene 仅含 `engine`、`duration_sec`（整数 30–120）、`lives`（整数 1–5）、`sound_volume`（整数 0–100）、`sprites` 和可选 `revision`（整数 1–2，省略为 1）。声明 `revision:2` 需要实际设备 `widgetSliceRevision>=2`，旧固件不能安装多模式版本。

sprites 恰好四项，顺序对应四种水果；每项遵守通用 PNG 精灵契约，frames 必须为 1。仍限制合计最多 4096 像素，推荐四张 32×32 RGBA。水果主体用原创透明 PNG；炸弹、刀痕和果汁由受控渲染器绘制，不使用未经授权的商标或游戏素材。

runtime 的 vars 为 `{}`，states 为 `["idle"]`，initial_state 为 `idle`，tick 为 `[]`。不得组合 pages、data、media、instrument 或额外场景实体。游戏内部四阶段由宿主管理，JSON 不重复实现物理或计分。

buttons 恰好两个动作：`slice.restart`（默认 SW1 短按，开始或重开）与 `slice.pause`（默认 SW2 短按，局内暂停/继续；revision 2 首页/结算时切换模式）。transitions 恰好为 `{ "on": "slice.restart", "from": "*" }` 和 `{ "on": "slice.pause", "from": "*" }`，不添加 effects、to 等字段。不能占用全局退出键。

## 玩法与验收

水果成波抛出、受重力落下；滑动轨迹与水果的连续相对运动相交才切中，每个水果只计分一次。单个水果 10 分，同一次划线切中至少三个额外累计连击分；炸弹优先判定且结束本局，漏接达到 lives 或时间结束均结算。切开有分离碎片、果汁、刀痕与本地合成音效；暂停/离开停止相关音效。最高分仅在当前组件运行期保留，不宣称永久记录。

revision 2 扩展：经典模式保留上述规则；限时冲分模式漏果不结束、炸弹扣最多 30 分并清空能量而非结束；轻松练习没有炸弹、漏果不结束。三种模式均在配置时长结束。首页/结算可点模式卡，点击开始开局，暂停页可点继续。触摸拖动或取消不能误触菜单。每种模式分别保留当前运行期最佳成绩。

水果种类决定基础半径（20/23/26/34），每个果再随机 ±4 像素，渲染和扫掠碰撞使用同一半径。每 12 秒提升一阶段，最多五阶，波次包含成组、交叉和散落轨迹；实体池仍上限 8。每五波有黄金果（50 分）；普通果补 8 能量、连击额外补 8、黄金果补 35；满 100 后启动 5 秒双倍得分，触发该效果的水果本身不追溯翻倍。连击要求同一划线中相邻命中间隔不超过 320 ms。暂停冻结倒计时和双倍剩余时间，不跨暂停续连击。

结构校验、自测、发布分别运行对应脚本，全部传 `--capabilities <实际设备能力.json>`。自测使用 skill 自带 `fruit-core.wasm` 和 Node.js，检查真实输入后的移动、碰撞得分、暂停、结算、重开，而非只检查字段。自测通过不等于屏幕方向、手感、帧率和扬声器已经实测；烧录后必须另做触摸方向、快速连切、全局退出、重新进入和实机声音验收。
# Round safety and records (firmware 0.7.91)

The physical start action starts an idle/finished round, resumes a paused round,
and does not reset an active round. Three mode records and the selected mode are
stored in the component's own bounded save. Restoring never resumes flying
objects or an old pointer stroke; the game returns to the ready screen.
Corrupted/out-of-range saved records are rejected as a unit.

