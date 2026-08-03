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
3. 仅在缺失信息会改变状态机、数据来源或按键语义时追问。不要为了填满模板而追问。
4. 在创建文件前，先写一份只针对当前需求的机制清单：
   - 用户目标与一局/一次使用的完整闭环。
   - 画面中的实体或数据，以及它们如何变化。
   - 每个可用输入产生的即时、可见结果。
   - 开始、进行、成功/失败、重开或复位条件。
   - 每一项需求分别由哪个 runtime 原语实现。
5. 读取 [references/patterns.md](references/patterns.md) 检查机制是否完整。该文件只有抽象检查项，不是组件模板。
6. 从空目录创建独立工作包，不要直接写入 `~/.claw-pet/components/library`，也不要复制或改名任何既有组件。
7. 执行校验：

   ```bash
   python <skill-dir>/scripts/validate_generated_widget.py <component-dir>
   ```

8. 修复全部校验错误。只有校验退出码为 `0` 时才允许发布。
9. 原子发布到正式本地组件库：

   ```bash
   python <skill-dir>/scripts/publish_generated_widget.py <component-dir> --source-agent <agent-id>
   ```

10. 向用户报告 `jobId`、组件 ID、内容版本哈希和正式目录。Pet Manager 会自动发现正式目录中的组件。

## 路由原则

- 先实现用户真正要求的玩法或工具，不得把不支持的游戏静默替换成 Flappy Bird、贪吃蛇或其他示例。
- 如果 P4 当前契约无法表达需求，明确指出具体限制，并询问用户是简化玩法还是等待运行时能力；不要生成一个名字相似但玩法不同的包。
- 新建组件时，禁止读取或搜索 `ref/builtin-clawpkgs`、`references/examples`、`~/.claw-pet/components`、历史生成目录或其他完整 `.clawpkg`。只有用户明确要求修复某个现有组件时，才读取该组件本身。
- 不存在“选一个最接近的游戏再改名”的步骤。必须从当前需求的机制清单推导状态、实体、规则、参数和文案。
- 禁止复制后改名、替换文案、微调速度/数量或换色来冒充新组件。若状态结构、实体布局和规则组合没有来自用户需求的理由，视为未完成。
- `component.json.kind` 必须明确为 `game` 或 `tool`。
- 所有新组件都声明顶层 `engine: "p4-bounded-runtime-v3"`。`game/tool` 只是产品分类，底层运行时相同。
- 先用变量、状态、transition、tick 和 dashboard 表达需求；只有确实需要坐标、移动、碰撞或边界行为时才增加 `scene`。
- 新小游戏使用通用 `scene`，不得声明旧版 `game.type=blocks|snake|flappy`；旧版 `game` 仅用于读取和维护兼容包。
- 文案出现移动、飞行、射击、子弹、敌人或碰撞时，runtime 必须真实实现对应 scene 机制；不得只在 Dashboard 中描述不存在的玩法。
- `scene.entities[*].shape` 只允许 `rect|player-ship|enemy-ship|bullet|star|paddle|ball`。根据实体语义选择形状；省略时才使用兼容默认值 `rect`。固定形状无法表达用户要求时，明确指出能力边界，不得换成矩形冒充。
- 新组件根据题材在 `pixel|clean` 中选择 `visualStyle`，并从 `candy|sunset|mint|arcade|ocean|forest|ember|mono` 中选择有理由的配色；`classic` 只用于读取历史包。不得无条件复用同一 style、palette、layout 或实体色阶组合。
- 视觉选择必须来自当前需求：发布前说明该风格、配色和每个实体形状为何匹配题材。若两个不同游戏除标题、文案和数值外具有相同的视觉组合，视为未完成并重新设计。
- 不生成 JS、Python、HTML、CSS、SVG、shell 或可执行代码到组件包中。

## 发布边界

- `.staging` 是发布器使用的短暂事务目录，不是用户组件库，也不在 Pet Manager 中展示。
- 正式组件只存在于 `~/.claw-pet/components/library/<component-id>/<version-hash>/`。
- 不手动复制到正式目录；始终使用 `publish_generated_widget.py`，确保校验、内容寻址和原子替换一致。
- 发布失败时保留失败 job 的 staging 目录与 `~/.claw-pet/logs/component-generation/<job-id>.json`，便于修复；成功后清理 staging，只保留日志。
- 不启动 Pet Manager、Agent CLI 或额外终端。生成交互始终留在用户当前 Agent 会话中。

## 交付前检查

- `game/tool` 路由符合用户目标。
- 逐项对照生成前的机制清单；用户要求的每个核心行为都能在 runtime 中找到对应实现，且没有无来源的模板机制。
- 默认画面无需操作也有明确含义，且与 runtime 初始 state/page/vars 一致。
- 每个普通 action 同时存在于 `buttons.json` 和 runtime transition。SW3 短按由设备全局执行返回，不写入组件包；旋钮长按默认不绑定。历史包中的 `page_main` 只做兼容读取。
- 无触控设备不含触屏事件；需要移动/碰撞时只使用能力清单声明的通用 scene 原语。
- 没有虚构数据、未声明的数据源或示例残留。
- 校验和发布命令均成功，最终路径位于正式 `library`，而不是工作目录或 `.staging`。
