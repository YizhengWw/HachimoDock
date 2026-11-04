---
name: petui
description: Generate, validate, and publish ESP32-P4 desktop-pet games, tools, PC-fed live-data widgets such as stock watchlists, and bounded PNG sprite assets as .clawpkg packages. Use for new components or repairs to existing Pet Manager components; live data requires an implemented PC data source.
---

# petui

把自然语言需求实现为可在 ESP32-P4 上运行的 `.clawpkg` 组件，并在校验通过后发布到 Pet Manager 的正式本地组件库。

## 扩展能力与设备协商

创作扩展能力包时，先读取设备当前能力 JSON，再选择声明。`p4-quota-v3`、
`p4-graphics-v2`、格子/文本表、程序 v2、音序/采样/视频、应用壳和文本输入
分别协商；不能由版本号、包内声明、离线默认能力表或另一个能力推断支持。
编译实现、离线测试、设备验收是三个不同状态，只有真实设备报告的能力才可用于同步。
新增能力仍在统一验收期间，不得将候选目标的编译结果写成已在普通固件开放。

- 格子状态与包内文本表：读取 [cells-texts.md](references/cells-texts.md)，分别核验 `widgetCells`、`widgetTexts`。
- 新图形预算：读取 [graphics-v2.md](references/graphics-v2.md)，不能全局提高旧包预算。
- 粒子与关键帧背景：读取 [particles.md](references/particles.md)、[keyframe-background.md](references/keyframe-background.md)，分别核验能力；静态背景不要求关键帧能力。
- 多刚体与旋转精灵：读取 [bodies-preview.md](references/bodies-preview.md)，核验 `widgetBodies`；不把单角色 motion 的能力当成多刚体能力。
- 多页应用、文本表单、集合、长文、通知和 PC HTTP 数据：读取 [d-host-and-surfaces.md](references/d-host-and-surfaces.md)，逐项核验能力与宿主配置；旧表单限制不用于描述显式 v3 表单。
- 多服务组合、本地文件导入、动态封面和播放进度：读取 [service-bridge.md](references/service-bridge.md)，核验服务桥的精确协商与包配对；没有服务桥能力时不得伪造能力下发。
- 合成旋律、短采样、组件视频、大图分片、跨组件事件与持久闹钟：读取 [component-media-events.md](references/component-media-events.md)，逐项核验独立能力；固定 sfx 的六个提示音不是这组扩展的上限，也不代表允许任意媒体/程序混用。
- 各能力的精确字段与拒绝边界同时见 `contracts/` 中对应向量。向量不是可发布模板。
- 账号与支付不属于本轮功能。联网请求由 PC 宿主按用户授予的域名权限执行。

## 工作流

1. 完整读取 [references/contract.md](references/contract.md)。契约是实现与校验的唯一依据。
2. 判断需求属于 `game` 还是 `tool`：
   - 需要设备端通用算法/事件程序时，完整读取 [references/component-program.md](references/component-program.md) 和 [references/program-preview.md](references/program-preview.md)。依据实际 `widgetProgram`/`widgetPrograms` 选择 v1/v2，并同时遵守设备报告的 cells/code/fuel/quantum 上限；缺失能力时拒绝同步。公开候选仍只有一个 PROGRAM 租约、总燃料 4096，不因 v2 私有上限更高而扩大。发布与设备执行验收分别报告，同源预览与自测不等于 P4 执行/声学验收。
   - 需要持久的公开 GET 与本地步骤串联、失败断点恢复或取消时，完整读取 [references/workflows.md](references/workflows.md)。使用显式组合 v3，共享有界工作队列；失败只允许明确恢复，不自动重提。尚不支持任意鉴权写请求或付费任务的未知结果恢复。
   - 需要任意命名变量驱动文字/进度/显隐，或组合计数、计时、本地时钟与公开 JSON 数据时，完整读取 [references/composition.md](references/composition.md)。分类汇总、筛选、排序、记录集合和多步本地事务可使用显式组合 v2，须核验宿主动作上下文能力；不能冒称已支持异步付费服务恢复。使用通用绑定与受控服务组合，不按组件 ID 添加代码；超出配额或需要鉴权/音频等服务时明确能力边界。
   - 需要"达到阈值才触发"、"有/没有资源时走不同分支"、变量相加或场景规则读写变量时，完整读取 [references/logic.md](references/logic.md)，声明 `logic: "p4-logic-v1"` 并核验 `widgetLogic`；它只提供有界比较、2 条 `all`、`else` 与 `{var}` 增量，不是表达式或脚本。
   - 变量、状态、页面、transition、节点或内容池项数超出 v1 上限时，完整读取 [references/quota.md](references/quota.md)，从实际 `widgetQuota`/`widgetQuotas` 选择显式 v2 或 v3；图片、精灵、表单及持久化仍按各自独立契约限制，不能把某一配额档位用于全局放宽。
   - 组件需要按键、结算或场景事件的短提示音时，完整读取 [references/sfx.md](references/sfx.md)，声明 `sfx: {"engine": "p4-sfx-v1", ...}` 并核验 `widgetSfx`；只有 6 个固定预设、最多 8 个 cue，没有采样、音乐或语音，不与木鱼/播放器混用。
   - 离线随机内容池、排除抽签、思考卡或设备端日期倒计时，完整读取 [references/local-tools.md](references/local-tools.md)。使用通用 local 引擎并校验 widgetLocalTools，不按组件 ID 写固件分支；断电后日期工具必须重新校时。
   - 近期资讯/新闻简报读取 [references/news.md](references/news.md)，采用有来源、时间窗和数量约束的 PC 资讯宿主，不把模型记忆当作联网结果。
   - 需要应用级布局、图标按钮、精细字号或原生 640×480 背景时，完整读取 [references/app-surface.md](references/app-surface.md)，核对设备图形能力；不得为了迁就旧图片配额静默降低分辨率或删掉图标。
   - 需要 PC 数据源驱动的可见集合行时，另完整读取 [references/host-collection.md](references/host-collection.md) 和 composition 契约；仅 UI 候选明确协商后使用，普通设备不支持时不得伪造能力或声称本地独立动态应用已完成。
   - 需要手机式滚动卡片、固定标题/底栏、打卡进度或记录历史时，另完整读取 [references/app-viewport.md](references/app-viewport.md) 与 [references/composition.md](references/composition.md)。使用通用裁切视口和 ledger，不按组件名字添加宿主逻辑；滚动须协商 widgetAppViewport，不冒充完整手机操作系统。
   - 本地应用需要多级详情、返回和页面级动作时，完整读取 [references/app-navigation.md](references/app-navigation.md)，协商 widgetAppNavigation；host 表单与动态列表不能冒用该本地导航能力。
   - 本地整数/枚举选择器与确认/取消编辑须完整读取 [references/app-form.md](references/app-form.md)，协商 widgetAppForm；该 v1 表单最多3字段。多字段文本、数字/Latin/拼音输入另读 [references/d-host-and-surfaces.md](references/d-host-and-surfaces.md) 并核验 widgetAppFormV3；提交/草稿隔离及实体可达要求保持，不能由 v1 能力推断 v3。
   - 组件需要点击、选择或直接操作画面时，读取 [references/touch.md](references/touch.md)。按需求灵活添加触屏区域，复用实体按键动作；应用点击与切果连续划线不是同一种能力，不能把可触屏等同于支持任意手势。
   - 需要滑动翻页、摇杆连发、按下即响应、确认弹窗或按状态显示按钮时，另读 [references/input-policy.md](references/input-policy.md)。这些能力必须由 `widgetInput=p4-input-v1` 明确协商，不能仅通过提高版本号假定设备支持。
   - 需要按住蓄力、松开发射和连续平台运动时，另读 [references/motion.md](references/motion.md)，使用 `widgetMotion=p4-motion-v1` 的通用单角色场景能力，不冒充任意物理引擎。
   - 需要完整横版跑跳、tile地图、巡逻敌人、移动平台和检查点时，完整读取 [references/side-scene.md](references/side-scene.md)，核验 `widgetSideScene=p4-side-scene-v1`；独立图集遵循该契约，不能退化为单角色蓄力跳跃示例。
   - 需要连续向前的随机平台和跟随镜头时，使用同一 motion 中的 `track` 扩展，另核验 `widgetPlatformTrack=p4-platform-track-v1`；不能用每次落台重置角色位置冒充连续前进。
   - 有目标、玩家输入、即时反馈、局面变化和回合结算时，使用 `game`。
   - 以计时、提醒、追踪、展示、查询或控制为主要价值时，使用 `tool`。
   - 展示行情等实时数据时，另读 [references/live-data.md](references/live-data.md)，确认 PC 数据源与设备能力；组件只订阅数据，不自行联网。需要登录态的数据（如 `github.me`）走 PC 鉴权提供器，令牌只在 PC；接口未确认的源（如 `codex.usage`）不得假装可用。
   - 把设备当快捷键面板向电脑发组合键时，另读 [references/hotkeys.md](references/hotkeys.md)，使用宿主服务 `hotkeys` 的白名单组合键与场景声明；发送默认关闭，由用户在 PC 开启。
   - 维护城市天气应用时，另读 [references/location-forecast.md](references/location-forecast.md)，使用包内声明式接口、字段映射、城市与文案；宿主只解释受控能力，不运行包内代码。
   - 分类知识库或长文阅读工具另读 [references/knowledge-library.md](references/knowledge-library.md)，知识和来源在 PC 资源包中，设备按页读取；校验数量、长度、随机不重复与授权，不用重复段落凑字数。
   - 农历/黄历工具另读 [references/calendar.md](references/calendar.md)，历法与民俗内容保留在 PC 数据包，宿主负责日期切换和时区，不把宜忌作为现实决策建议。
   - 新联网媒体组件使用双端声明式扩展，完整读取 [references/pc-extensions.md](references/pc-extensions.md)。PC 包声明面板与受控 GET 搜索映射，设备包声明 surface；宿主提供公共网络、解码与存储。先确认宿主和设备能力，不用模拟假装播放。维护旧 audio.player 时另读 [references/media.md](references/media.md)。
   - 本地木鱼音效工具另读 [references/instrument.md](references/instrument.md)，需要 widgetInstrument=p4-instrument-v1；不是任意音频或高分辨率素材的通用执行入口。
   - 触屏切果游戏另读 [references/touch-slice.md](references/touch-slice.md)。需要设备声明 p4-touch-slice-v1、widgetPointer=p4-pointer-v1 且 touchInput.ready=true；只支持该受控玩法，不代表任意触屏脚本均可运行。校验、自测和发布都传入实际设备能力文件；自测需要 Node.js。
3. 先判断这是新建组件还是优化/修复现有组件：
   - 新建组件才创建新的 `component.json.id`。
   - 优化、修复或继续迭代现有组件时，先读取用户指定的那个组件，并原样保留它的 `component.json.id`；除非用户明确要求另存为新组件，否则禁止生成新 ID。显示名称相同不能代替 ID 相同。
   - 优化现有组件时默认同时保留原 `component.json.name`，避免组件中心出现两个看似相同的条目；只有用户明确要求改名时才改显示名称，但 ID 仍保持不变。
4. 仅在缺失信息会改变状态机、数据来源、按键语义或无法确认要覆盖的现有组件时追问。不要为了填满模板而追问。
5. 在创建文件前，先写一份只针对当前需求的机制清单：
   - 用户目标与一局/一次使用的完整闭环。
   - 画面中的实体或数据，以及它们如何变化。
   - 每个可用输入产生的即时、可见结果。
   - 开始、进行、成功/失败、重开或复位条件。
   - `component.json.description` 用 1-2 句说明游戏目标、核心规则和结束条件，让用户在组件库里不进入游戏也知道要做什么；具体物理按键仍由 `buttons.json` 提供，避免用户改键后描述失真。
   - 每一项需求分别由哪个 runtime 原语实现。
   - 哪些动作适合摇杆上/下/左/右，游戏的 SW1 开始/重开动作，以及 SW2 的题材专属动作；退出由设备全局设置负责（出厂默认 SW3，但用户可以改绑或不绑定），组件不得定义退出动作或占用出厂默认退出键。
   - 先确定一套与题材匹配的现代视觉语言：主要轮廓、层级、留白、配色和动效。除非用户明确要求复古、像素或街机风，新组件默认使用 `clean`，不得把网格方块当作默认美术风格。
   - 游戏先设计核心 PNG 素材：玩家角色、主要敌人和最重要的动态物件优先使用透明背景 PNG / 横向 sprite sheet。矩形、方块和内置 shape 只用于平台、子弹、金币、碰撞区等次要几何元素，或确实更符合极简题材时使用；不能因为省事让主体退化成色块。
6. 读取 [references/patterns.md](references/patterns.md) 检查机制是否完整。该文件只有抽象检查项，不是组件模板。
7. 在正式组件库之外准备独立工作包：
   - 新建组件必须从空目录开始，不要复制或改名任何既有组件。
   - 优化/修复现有组件时，只把用户指定的那一个组件复制到独立工作目录中修改，并保持原组件 ID；不得直接写入 `~/.claw-pet/components/library`。
8. 需要 PNG 精灵时，先完成素材再写 runtime 引用：
   - 使用当前 Agent 可用的位图生成/编辑能力，或在组件目录之外使用本地图像工具制作透明背景横向 sprite sheet；组件包内只保留最终 `.png`，不得保留生成脚本、提示词、临时帧或其他源文件。
   - 每帧使用完全一致的单元尺寸并从左到右紧密排列；最终宽度必须等于 `frame_width * frames`，高度必须等于 `frame_height`。文件名使用安全 ASCII，并保证 `scene.sprites[*].asset` 的大小写与磁盘路径完全一致。
   - 先确认 PNG 文件已真实写入 `assets/`、不是把 JPEG/WebP 改扩展名、不是空白占位图，再把它加入 `scene.sprites` 并让实体通过 `sprite` 引用。没有可用位图能力时优先改用语义 shape；不得引用一个尚不存在的素材路径。
   - 发布前逐帧检查横向帧带：首帧必须在默认画面中清楚可辨；每帧都要有非空主体，主体的尺寸、重心和朝向变化要连续，不能出现整帧透明、裁切、重影、突然缩放或大幅跳位。动画只改变当前动作所需的部分，避免设备上呈现“闪烁的一团”。
9. 执行结构与素材校验：

   ```bash
   python <skill-dir>/scripts/validate_generated_widget.py <component-dir>
   ```

10. 游戏、带 `scene` 的互动工具、声明 `data` 的实时列表、声明 `instrument` 的木鱼及声明 `media` 的播放器均执行自测：

   ```bash
   python <skill-dir>/scripts/smoke_test_widget_game.py <component-dir>
   ```

   游戏自测必须从真实输入映射出发，跑通开始、每个玩法动作的即时可见反馈、自动推进、得分或进展、成功/失败结算和重开。互动工具必须让每个声明输入在可达状态中产生即时可见变化，并验证声明的 tick/自动运动确实推进画面。只验证 action 名存在、速度字段变化或单帧动画不算通过。
   普通应用自测也探索同一状态/页面内的变量变化，例如先计数再清零。探索受 512 个快照和 32 层输入深度约束，并报告是否截断；这不是无限状态空间或全部路径证明。
   实时列表自测覆盖 0/1/6/11/16/20 行的分页可达性与真实按键反馈；它不验证网络取价、时效或设备画面，这些必须另外验收，不能把结构/模拟通过写成实时链路已通过。
11. 修复全部结构、素材和玩法错误。使用 PNG 时还要逐条核对没有素材缺失、格式、尺寸、帧数、总像素、文件大小或帧间闪烁风险。只有结构校验与适用的场景/玩法自测退出码都为 `0` 时才允许发布；发布器还会重复执行自测，不能跳过。
12. 原子发布到正式本地组件库：

   ```bash
   python <skill-dir>/scripts/publish_generated_widget.py <component-dir> --source-agent <agent-id>
   ```

13. 向用户报告 `jobId`、组件 ID、内容版本哈希、玩法自测结果和正式目录。Pet Manager 会自动发现正式目录中的组件。
14. 如果本次是优化/修复现有组件，必须明确告诉用户：在 Pet Manager 组件中心刷新组件库，打开同一个组件卡片并点击“保存并同步”，用同一组件 ID 的新包覆盖设备上的旧包；只发布到本地组件库不会自动替换设备内容。

## 路由原则

- 扩展现有组件时先复用通用能力：视觉/文案/动作映射/场景规则和受支持的物理参数放资源包；PC 网络、日期、阅读和音频等复用声明式宿主服务。不得用组件 ID、书名或玩法名称在固件内新增专用分支。
- 发现缺失原语时，一次同步设备解释器、PC 预览、校验器、能力协商、技能参考和回归测试；记录支持边界。不能把仅某个包的补丁称为通用支持，也不能承诺未实现能力免固件升级。
- 输入扩展必须覆盖实体解码→事件队列→动作派发，再覆盖场景核心和触屏；浏览器模拟通过不能代替实体按住/松开验收。新资源包若只使用设备已具备的原语，不因更换主题、布局或参数而提高固件要求。

- 先实现用户真正要求的玩法或工具，不得把不支持的游戏静默替换成 Flappy Bird、贪吃蛇或其他示例。
- 如果 P4 当前契约无法表达需求，明确指出具体限制，并询问用户是简化玩法还是等待运行时能力；不要生成一个名字相似但玩法不同的包。
- 新建组件时，禁止读取或搜索 `pc/builtin-clawpkgs`、`references/examples`、`~/.claw-pet/components`、历史生成目录或其他完整 `.clawpkg`。只有用户明确要求修复某个现有组件时，才读取该组件本身。
- `component.json.id` 是版本升级和设备覆盖的唯一组件身份。优化/修复现有组件时必须复用原 ID，不能通过加后缀、时间戳或重新起包名创建另一条组件记录；显示名称相同但 ID 不同仍会被视为两个组件。
- 不存在“选一个最接近的游戏再改名”的步骤。必须从当前需求的机制清单推导状态、实体、规则、参数和文案。
- 禁止复制后改名、替换文案、微调速度/数量或换色来冒充新组件。若状态结构、实体布局和规则组合没有来自用户需求的理由，视为未完成。
- `component.json.kind` 必须明确为 `game` 或 `tool`。
- 游戏的 `component.json.description` 不是宣传口号：必须简要说明怎么玩、怎样得分或完成、何时结束。组件中心会在它下方根据 `buttons.json` 自动补充当前操作方法，因此描述不得写死可被用户重新映射的物理键位。
- 所有新组件都声明顶层 `engine: "p4-bounded-runtime-v4"`。`game/tool` 只是产品分类，底层运行时相同；v3 只用于读取和维护历史包。
- 先用变量、状态、transition、tick 和 dashboard 表达需求；只有确实需要坐标、移动、碰撞或边界行为时才增加 `scene`。
- 实时数据工具遵循契约 5.1 与 `references/live-data.md`；当前已实现 `stocks.watchlist`，其他 source 先确认或补齐 PC 提供器。股票默认项、刷新周期、鉴权都属于 PC，不写入组件包，不假装任意 URL 可以直接工作。
- 新小游戏使用通用 `scene`，不得声明旧版 `game.type=blocks|snake|flappy`；旧版 `game` 仅用于读取和维护兼容包。
- 例外：用户要求五子棋 / 五子对弈 / 连珠时，使用原生预设 `game.type="gomoku"`（契约 §6「原生五子棋预设」、app-surface.md「原生五子棋画面」），固定 action 表 `place|left|right|up|down|undo`，需设备 `widgetGamePresets` 含 `gomoku`；description 规则不变，仍需说明目标、规则与结束条件。其他棋类不得借用该预设。
- 文案出现移动、飞行、射击、子弹、敌人或碰撞时，runtime 必须真实实现对应 scene 机制；不得只在 Dashboard 中描述不存在的玩法。
- 新游戏的核心视觉元素默认使用受控横向 PNG sprite sheet：玩家/主角必须优先使用 PNG，主要敌人或目标在配额允许时也使用 PNG；最多 4 个精灵、每个 1-8 帧、单帧 8-64 像素、1-20 fps、全部帧合计最多 4096 像素、单个源 PNG 不超过 128 KiB。PNG 只作为静态素材，不能包含脚本；PC 会预编译后随组件事务下发。
- `scene.entities[*].shape` 可使用 `rect|player-ship|enemy-ship|bullet|star|paddle|ball|circle|capsule|triangle|diamond|heart|cloud|coin|character`。优先用于平台、弹丸、金币、装饰和极简几何题材；省略时才使用兼容默认值 `rect`。若当前 Agent 没有生成或绘制 PNG 的能力，必须先说明限制并选择语义形状，禁止静默用矩形/方块冒充主体。
- `assets/` 只保留 `.keep` 与最终 PNG；每个 PNG 都必须被 `scene.sprites` 精确引用，每条 sprite 引用也必须在发布前对应到真实文件。不要把 JPEG/WebP 改扩展名，也不要留下未使用的候选图或逐帧临时文件。
- 新组件默认 `visualStyle: "clean"`；只有用户明确要求复古、像素、8-bit 或街机风时才选 `pixel`。从 `candy|sunset|mint|arcade|ocean|forest|ember|mono` 中选择有理由的配色；`classic` 只用于读取历史包。不得无条件复用同一 style、palette、layout 或实体色阶组合。
- 视觉选择必须来自当前需求：发布前说明该风格、配色和每个实体形状为何匹配题材。若两个不同游戏除标题、文案和数值外具有相同的视觉组合，视为未完成并重新设计。
- 不生成 JS、Python、HTML、CSS、SVG、shell 或可执行代码到组件包中。
- 已安装且校验通过的组件默认获准使用其声明的受控宿主服务，首次使用和更新均不增加授权确认。可展示非阻塞的数据使用说明；保留手动停用、系统权限、服务配置、实际操作及破坏性操作确认，不把默认授权实现为自动录音、播放或派发任务。

## 发布边界

- `.staging` 是发布器使用的短暂事务目录，不是用户组件库，也不在 Pet Manager 中展示。
- 正式组件只存在于 `~/.claw-pet/components/library/<component-id>/<version-hash>/`。
- 同一 `<component-id>` 下的新内容哈希是该组件的新版本；Pet Manager 展示最新版本。旧内容可以保留用于内容寻址，但设备仍需从组件中心对同一卡片执行“保存并同步”才会被新版本覆盖。
- 不手动复制到正式目录；始终使用 `publish_generated_widget.py`，确保校验、内容寻址和原子替换一致。
- 发布失败时保留失败 job 的 staging 目录与 `~/.claw-pet/logs/component-generation/<job-id>.json`，便于修复；成功后清理 staging，只保留日志。
- 不启动 Pet Manager、Agent CLI 或额外终端。生成交互始终留在用户当前 Agent 会话中。

## 交付前检查

- `game/tool` 路由符合用户目标。
- 逐项对照生成前的机制清单；用户要求的每个核心行为都能在 runtime 中找到对应实现，且没有无来源的模板机制。
- 默认画面无需操作也有明确含义，且与 runtime 初始 state/page/vars 一致。
- 游戏在组件库中的 description 已经说明目标、核心规则与结束条件；按钮说明可由组件中心根据真实映射生成，不在 description 中写死 SW1/SW2/SW3。
- 默认现代视觉不是“把格子间距去掉”：主体应优先使用语义形状或小型精灵，卡片/HUD 要有层级和留白；只有明确的复古需求才允许可见像素网格和方块化主体。
- 使用 PNG 时，素材是透明背景横向帧带，文件名与 `scene.sprites.asset` 完全一致；所有 PNG 都有引用、所有引用都有文件，首帧能表达默认状态，连续帧不存在空白、错位或尺寸漂移。
- 每个 action 都必须有 runtime transition。传统包还必须绑定实体输入；声明 `p4-input-v1` 的资源包可以额外提供仅触屏按钮或滑动动作，但须真实可达，不能只是未绑定的空动作。旧版 game 预设动作仍须有实体键兜底。新硬件可使用摇杆上/下/左/右与中按；左右和中按沿用历史事件名以兼容旧包。组件不得声明 `page_main/page_back` 等系统导航 action；退出由设备全局绑定统一处理，摇杆中键长按默认不绑定。
- 游戏核心角色和主要动态物件已优先使用 PNG；若退回内置 shape，能够说明题材或能力限制上的具体理由，而不是为了省事。PNG 的透明背景、帧尺寸、帧数和引用路径均通过校验。
- 出厂默认 SW3 短按是设备全局退出键，用户可以改绑或不绑定；生成或更新组件时仍不得占用 SW3，也不得自行声明退出动作。游戏优先将方向动作放到四向摇杆，将 SW1 用作开始/重新开始，将 SW2 留给射击、技能、暂停等题材专属动作。工具以 SW1 为主操作、SW2 为次操作，并可按语义使用摇杆方向切页或调节。
- 组件画面、footer 和说明不得写死“SW3 退出”；需要提示时统一写“全局键退出”或“退出跟随设备全局设置”。
- 无触控设备不含触屏事件；需要移动/碰撞时只使用能力清单声明的通用 scene 原语。
- 没有虚构数据、未声明的数据源或示例残留。
- 结构校验、完整玩法自测和发布命令均成功；玩法报告证明开始、输入反馈、推进、进展/得分、结算与重开都可达。最终路径位于正式 `library`，而不是工作目录或 `.staging`。
- 带 scene 的互动工具自测报告证明每个真实输入均有可见反馈，声明的 tick/自动运动在互动后的可达状态中会生效；PNG 横向帧带已逐帧检查，没有空帧、裁切、重影、尺寸或重心突变。
- 优化/修复任务保留了原 `component.json.id`，交付说明包含组件中心刷新与“保存并同步”的设备覆盖步骤。

媒体切图扩展：已支持 widgetSurfaceImages=p4-surface-images-v1 时，遵循 references/pc-extensions.md 的 surface.images 契约；这是媒体独立配额，不扩大游戏 scene 精灵配额。

### D 能力开发路由

涉及 pc-fetch、应用壳标签页、form v3、data rows、长文/figure 或通知时，读取 [D 宿主与展示参考](references/d-host-and-surfaces.md)。该参考区分已接入入口与仍待公共 C 钩子及统一构建/HIL 的能力；不得把独立测试通过宣传为完整设备支持。

### 通用服务桥候选路由

需要原生服务投影、稳定键列表、类型化参数动作、显示时间轴、动态图片或显式本地媒体导入时，完整读取 [service-bridge.md](references/service-bridge.md)。纯标量桥可复用 legacy host 及原有能力；动态 image/timeline/event 参数或 local-media 必须显式声明 `surface.services="p4-service-bridge-v1"`、核验真实设备候选能力及各依赖，并用 `host_contract` 配对 PC 声明与设备 runtime（所有数值零统一 `i0;`，其他有限浮点保留）；不得按组件 ID 写特例或把 PC 预览当成设备支持。候选默认关闭，未完成统一固件/HIL验收；本地媒体不开放在线歌曲、任意 URL、账号或支付，不同步用户全局 skill。
