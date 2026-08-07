---
name: petui
description: Generate, validate, and publish petui desktop-pet components for the ESP32-P4 640x480 negative screen. Use when a user asks for a desk-pet widget, petAgent component, OpenClaw component, negative-screen tool or game, .clawpkg package, or wants an existing component repaired and added to Pet Manager's formal local component library.
---

# petui

把自然语言需求实现为可在 ESP32-P4 上运行的 `.clawpkg` 组件，并在校验通过后发布到 Pet Manager 的正式本地组件库。

## 工作流

1. 完整读取 [references/contract.md](references/contract.md)。契约是实现与校验的唯一依据。
2. 判断需求属于 `game` 还是 `tool`：
   - 有目标、玩家输入、即时反馈、局面变化和回合结算时，使用 `game`。
   - 以计时、提醒、追踪、展示、查询或控制为主要价值时，使用 `tool`。
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
6. 读取 [references/patterns.md](references/patterns.md) 检查机制是否完整。该文件只有抽象检查项，不是组件模板。
7. 在正式组件库之外准备独立工作包：
   - 新建组件必须从空目录开始，不要复制或改名任何既有组件。
   - 优化/修复现有组件时，只把用户指定的那一个组件复制到独立工作目录中修改，并保持原组件 ID；不得直接写入 `~/.claw-pet/components/library`。
8. 执行校验：

   ```bash
   python <skill-dir>/scripts/validate_generated_widget.py <component-dir>
   ```

9. 修复全部校验错误。只有校验退出码为 `0` 时才允许发布。
10. 原子发布到正式本地组件库：

   ```bash
   python <skill-dir>/scripts/publish_generated_widget.py <component-dir> --source-agent <agent-id>
   ```

11. 向用户报告 `jobId`、组件 ID、内容版本哈希和正式目录。Pet Manager 会自动发现正式目录中的组件。
12. 如果本次是优化/修复现有组件，必须明确告诉用户：在 Pet Manager 组件中心刷新组件库，打开同一个组件卡片并点击“保存并同步”，用同一组件 ID 的新包覆盖设备上的旧包；只发布到本地组件库不会自动替换设备内容。

## 路由原则

- 先实现用户真正要求的玩法或工具，不得把不支持的游戏静默替换成 Flappy Bird、贪吃蛇或其他示例。
- 如果 P4 当前契约无法表达需求，明确指出具体限制，并询问用户是简化玩法还是等待运行时能力；不要生成一个名字相似但玩法不同的包。
- 新建组件时，禁止读取或搜索 `ref/builtin-clawpkgs`、`references/examples`、`~/.claw-pet/components`、历史生成目录或其他完整 `.clawpkg`。只有用户明确要求修复某个现有组件时，才读取该组件本身。
- `component.json.id` 是版本升级和设备覆盖的唯一组件身份。优化/修复现有组件时必须复用原 ID，不能通过加后缀、时间戳或重新起包名创建另一条组件记录；显示名称相同但 ID 不同仍会被视为两个组件。
- 不存在“选一个最接近的游戏再改名”的步骤。必须从当前需求的机制清单推导状态、实体、规则、参数和文案。
- 禁止复制后改名、替换文案、微调速度/数量或换色来冒充新组件。若状态结构、实体布局和规则组合没有来自用户需求的理由，视为未完成。
- `component.json.kind` 必须明确为 `game` 或 `tool`。
- 游戏的 `component.json.description` 不是宣传口号：必须简要说明怎么玩、怎样得分或完成、何时结束。组件中心会在它下方根据 `buttons.json` 自动补充当前操作方法，因此描述不得写死可被用户重新映射的物理键位。
- 所有新组件都声明顶层 `engine: "p4-bounded-runtime-v4"`。`game/tool` 只是产品分类，底层运行时相同；v3 只用于读取和维护历史包。
- 先用变量、状态、transition、tick 和 dashboard 表达需求；只有确实需要坐标、移动、碰撞或边界行为时才增加 `scene`。
- 新小游戏使用通用 `scene`，不得声明旧版 `game.type=blocks|snake|flappy`；旧版 `game` 仅用于读取和维护兼容包。
- 文案出现移动、飞行、射击、子弹、敌人或碰撞时，runtime 必须真实实现对应 scene 机制；不得只在 Dashboard 中描述不存在的玩法。
- `scene.entities[*].shape` 可使用 `rect|player-ship|enemy-ship|bullet|star|paddle|ball|circle|capsule|triangle|diamond|heart|cloud|coin|character`。根据实体语义选择轮廓；省略时才使用兼容默认值 `rect`，不得用无关方块冒充主体。
- 固定轮廓不足以表达题材核心角色时，可使用受控横向 PNG sprite sheet：最多 4 个精灵、每个 1-8 帧、单帧 8-64 像素、1-20 fps、全部帧合计最多 4096 像素、单个源 PNG 不超过 128 KiB。PNG 只作为静态素材，不能包含脚本；PC 会预编译后随组件事务下发。
- 新组件默认 `visualStyle: "clean"`；只有用户明确要求复古、像素、8-bit 或街机风时才选 `pixel`。从 `candy|sunset|mint|arcade|ocean|forest|ember|mono` 中选择有理由的配色；`classic` 只用于读取历史包。不得无条件复用同一 style、palette、layout 或实体色阶组合。
- 视觉选择必须来自当前需求：发布前说明该风格、配色和每个实体形状为何匹配题材。若两个不同游戏除标题、文案和数值外具有相同的视觉组合，视为未完成并重新设计。
- 不生成 JS、Python、HTML、CSS、SVG、shell 或可执行代码到组件包中。

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
- 每个 action 同时存在于 `buttons.json` 和 runtime transition。新硬件可使用摇杆上/下/左/右与中按；左右和中按沿用历史事件名以兼容旧包。组件不得声明 `page_main/page_back` 等系统导航 action；退出由设备全局绑定统一处理，摇杆中键长按默认不绑定。
- 出厂默认 SW3 短按是设备全局退出键，用户可以改绑或不绑定；生成或更新组件时仍不得占用 SW3，也不得自行声明退出动作。游戏优先将方向动作放到四向摇杆，将 SW1 用作开始/重新开始，将 SW2 留给射击、技能、暂停等题材专属动作。工具以 SW1 为主操作、SW2 为次操作，并可按语义使用摇杆方向切页或调节。
- 组件画面、footer 和说明不得写死“SW3 退出”；需要提示时统一写“全局键退出”或“退出跟随设备全局设置”。
- 无触控设备不含触屏事件；需要移动/碰撞时只使用能力清单声明的通用 scene 原语。
- 没有虚构数据、未声明的数据源或示例残留。
- 校验和发布命令均成功，最终路径位于正式 `library`，而不是工作目录或 `.staging`。
- 优化/修复任务保留了原 `component.json.id`，交付说明包含组件中心刷新与“保存并同步”的设备覆盖步骤。
