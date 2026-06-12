/**
 * [Input] User component description, followed coding-agent channel state from localStorage.
 * [Output] Prompt template for generating .clawpkg negative-screen component packages via Codex / Claude Code, allowing install-time button-function bindings across screen touch, top red button, and front encoder while requiring clarification when button behavior is underspecified.
 * [Pos] lib node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import { CLAWPKG_FILES, COMPONENT_DASHBOARD_V1_SLOTS } from "./clawpkg-contract.js";
import {
  AGENT_APPEARANCE_MAP_STORAGE_KEY,
  ENABLED_AGENTS_STORAGE_KEY,
} from "./agent-appearance-config.js";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const COMPONENT_GENERATION_AGENT_IDS = ["codex", "claude-code", "openclaw"];

const AGENT_LABEL_MAP = {
  codex: "Codex",
  "claude-code": "Claude Code",
  openclaw: "OpenClaw",
};

const AGENT_COMMAND_MAP = {
  codex: "codex",
  "claude-code": "claude",
  openclaw: "openclaw",
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

function firstCodingAgentId(values = []) {
  return values.find((id) => COMPONENT_GENERATION_AGENT_IDS.includes(id)) || "";
}

function parseJsonObject(value) {
  if (typeof value !== "string" || !value.trim()) return null;
  try {
    const parsed = JSON.parse(value);
    return parsed && typeof parsed === "object" && !Array.isArray(parsed) ? parsed : null;
  } catch {
    return null;
  }
}

function parseJsonArray(value) {
  if (typeof value !== "string" || !value.trim()) return [];
  try {
    const parsed = JSON.parse(value);
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];
  }
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

/**
 * Build the full component-generation prompt for a given description and agent.
 *
 * The prompt body includes:
 *   (a) .clawpkg six-file description
 *   (b) COMPONENT_DASHBOARD_V1 ten-slot table (9 mandatory + progress optional) with maxBytes limits
 *   (c) Token-usage worked example
 *   (d) Device control facts + screen/red-button/knob bindings
 *   (e) Output requirements (6 files)
 *   (f) Clarification directive for missing slot information
 */
const DEFAULT_DESCRIPTION =
  "做一个会议计时组件,切到负一屏即进入会议计时场景并显示本轮倒计时和下一场会议；点击屏幕开始/暂停,长按屏幕重置。";

export function buildComponentGenerationPrompt({ description = "", agentId = "codex" } = {}) {
  const resolvedDescription =
    description === null ||
    description === undefined ||
    (typeof description === "string" && description.trim() === "")
      ? DEFAULT_DESCRIPTION
      : description;

  const agentLabel = labelForComponentGenerationAgent(agentId);

  // (a) .clawpkg file descriptions
  const filesList = CLAWPKG_FILES.map((f) => `  - ${f.name}：${f.role}`).join("\n");

  // (b) COMPONENT_DASHBOARD_V1 slot table
  const slotsTable = COMPONENT_DASHBOARD_V1_SLOTS.map(
    (s) => `  | ${s.id.padEnd(14)} | ${String(s.maxBytes).padStart(4)} 字节 | ${s.role}`,
  ).join("\n");

  const headlineMaxBytes =
    COMPONENT_DASHBOARD_V1_SLOTS.find((s) => s.id === "headline")?.maxBytes ?? 156;

  return [
    `你是 OpenClaw 通用功能组件实现 agent（当前渠道：${agentLabel}）。`,
    "请根据下面的用户需求，生成一个可安装、可分享的负一屏组件包（.clawpkg 格式）。",
    "",
    "═══════════════════════════════════════════════",
    "一、.clawpkg 包结构（六件套）",
    "═══════════════════════════════════════════════",
    filesList,
    "",
    "═══════════════════════════════════════════════",
    "二、COMPONENT_DASHBOARD_V1 槽位规格（十槽位：9 必填 + progress 可选）",
    "═══════════════════════════════════════════════",
    "  | 槽位 id        | 上限  | 用途",
    "  |----------------|-------|-------------------------",
    slotsTable,
    "",
    `重要：所有槽位内容须在 UTF-8 字节上限内（如 headline 上限为 ${headlineMaxBytes} 字节）。`,
    "",
    "═══════════════════════════════════════════════",
    "三、完整 Worked Example —— Token 消耗卡片",
    "═══════════════════════════════════════════════",
    "以下是一个填写完整的 COMPONENT_DASHBOARD_V1 示例，供参考格式与 Token 消耗展示：",
    "",
    "  title        = Codex",
    "  eyebrow      = 等价于购买了",
    "  headline     = 约 3.7 顿工作午餐",
    "  metricLabel  = 今日累计 Token",
    "  metricValue  = 1.30M",
    "  metricUnit   = TOKEN",
    "  badge        = 1",
    "  note         = 输入 900.0K · 输出 400.0K",
    "  footer       = 点击 查看拆分 · 长按 刷新",
    "",
    "说明：headline 槽位最多 156 字节，上例中「约 3.7 顿工作午餐」占用约 36 字节，",
    "远低于上限；metricValue / metricUnit 合并展示当日累计 Token 消耗总量。",
    "",
    "═══════════════════════════════════════════════",
    "四、设备控件事实与默认绑定模型",
    "═══════════════════════════════════════════════",
    "负一屏场景模型（必须遵守）：",
    "  - 切到负一屏就是进入这个组件场景；组件默认状态必须自己成立,不能依赖红钮或旋钮触发才有价值",
    "  - 每个组件必须说明进入负一屏后的默认行为: 自动计时 / 自动刷新展示 / 显示当前状态 / 进入待开始状态",
    "  - 适合的交互模型是: 无操作也持续展示或运行；screen.region.tap 做主操作；screen.region.long_press 做次操作",
    "",
    "设备端固定事实（必须遵守）：",
    "  - 组件中心安装前可把每个 action 绑定到屏幕点击或屏幕长按",
    "  - 屏幕滑动仍用于系统级切屏，不写进 buttons.json",
    "  - 旋钮旋转固定用于系统音量，不写成 widget action",
    "  - 未明确要求硬件按钮时，默认只写 screen.region.tap 和 screen.region.long_press",
    "",
    "buttons.json 写按钮功能绑定：",
    "  - screen.region.tap        → 执行主操作（如刷新、开始、查看详情）",
    "  - screen.region.long_press → 执行次操作（如重置、展开更多操作）",
    "",
    "不同按钮配置必须描述清楚：",
    "  - 进入负一屏后的默认场景和默认状态是什么；如果缺少这个信息,先追问",
    "  - 点击 screen.region.tap 和长按 screen.region.long_press 分别执行什么 action",
    "  - 如果用户想用红钮或旋钮，说明这些硬件不用于组件动作；旋钮固定为音量",
    "  - 每个 action 的中文 label、状态切换、变量变化必须能和 runtime/widget.json transitions 对上",
    "  - 如果用户只说\"加个按钮\"、\"支持点击\"但没说清点击 / 长按分别做什么，先追问再生成",
    "",
    "═══════════════════════════════════════════════",
    "五、产出要求",
    "═══════════════════════════════════════════════",
    "请将以下 6 个文件写满并输出到当前目录：",
    "  1. component.json       — 组件元数据（id、name、version、author、capabilities、入口）",
    "  2. negative-screen.json — COMPONENT_DASHBOARD_V1 槽位映射与空状态",
    "  3. buttons.json         — 屏幕按钮功能绑定表",
    "  4. runtime/             — 声明式运行逻辑（首版只引用受控能力）",
    "  5. assets/              — 图标/声音等静态素材",
    "  6. share.json           — 社区分享卡片元数据",
    "",
    "═══════════════════════════════════════════════",
    "六、信息不足时的处理指令",
    "═══════════════════════════════════════════════",
    "如果用户描述信息不足，缺少渲染某槽位或按钮配置所需的关键信息（例如：",
    "  - 没说明切到负一屏后的默认场景 / 默认状态",
    "  - 计时类组件缺少时长范围",
    "  - 数值类组件缺少单位或取值范围",
    "  - 展示类组件缺少数据来源",
    "  - 交互类组件没有说清点击 / 长按分别触发什么动作",
    "），请先向用户澄清（追问），再生成组件文件。",
    "不要假设缺省值后直接生成——信息不足时必须先追问再继续。",
    "",
    "═══════════════════════════════════════════════",
    "七、用户需求",
    "═══════════════════════════════════════════════",
    `用户需求：${resolvedDescription}`,
  ].join("\n");
}

/**
 * Build the command object for launching a component-generation agent session.
 */
export function createComponentGenerationCommand({ description = "", agentId = "codex" } = {}) {
  const label = labelForComponentGenerationAgent(agentId);
  const command = AGENT_COMMAND_MAP[agentId] ?? "codex";

  return {
    agentId,
    label,
    command,
    mode: "component-generation",
    template: "negative-screen-component",
    packageFiles: CLAWPKG_FILES.map((f) => f.name),
    prompt: buildComponentGenerationPrompt({ description, agentId }),
  };
}

/**
 * Return a prompt appropriate for the given description.
 * Empty / whitespace-only / null / undefined → clarification prompt.
 * Non-empty → full generation prompt via buildComponentGenerationPrompt.
 */
export function createAgentPrompt(description) {
  const isEmpty =
    description === null ||
    description === undefined ||
    (typeof description === "string" && description.trim() === "");

  if (isEmpty) {
    return '请先描述你想要的组件功能,例如"做一个会议计时,切到负一屏即进入计时场景,点击屏幕开始/暂停,长按屏幕重置"。';
  }

  const agentId = loadFollowedComponentGenerationAgentId();
  return buildComponentGenerationPrompt({ description, agentId });
}

/**
 * Short prompt that defers to the installed petAgent-ui-generator skill — the skill
 * description matches "做个桌搭子组件 / 负一屏组件 / clawpkg / 文生组件 / petAgent 组件"
 * and auto-loads its full SKILL.md + references when triggered.
 * Use this AFTER the user has installed the skill (Step 1) — much shorter than
 * the full inlined template, lets the skill take over with proper context.
 */
export function createSkillTriggerPrompt(description) {
  const isEmpty =
    description === null ||
    description === undefined ||
    (typeof description === "string" && description.trim() === "");
  if (isEmpty) {
    return '请先描述你想要的组件功能,例如"做一个会议计时,切到负一屏即进入计时场景,点击屏幕开始/暂停,长按屏幕重置"。';
  }
  /* prefix triggers Claude Code skill auto-discovery AND gives Codex
     an explicit "use this skill" instruction */
  return `请用 petAgent-ui-generator skill 帮我生成一个 .clawpkg 桌搭子负一屏组件,需求如下:

${description.trim()}

按钮配置要求:
- 必须说明切到负一屏后的默认场景和自运行/默认状态；组件不能依赖红钮或旋钮触发才有价值。
- 默认至少说明 screen.region.tap 和 screen.region.long_press 分别做什么；红钮不可用,不要写顶部红钮事件；旋钮固定用于系统音量,不要写成 widget action。
- 请把按钮功能写进 buttons.json,并让 action 与 runtime/widget.json transitions 对齐。
- 如果按钮配置不清楚,例如只说"加个按钮"或"支持点击"但没说点击 / 长按分别触发什么,请先追问,不要直接猜。

要求把组件目录生成在当前工作目录下,目录名用 kebab-case id。生成后告诉我目录路径,我会拖回到 HachimoDock 的组件中心安装到设备。`;
}

/**
 * Read the currently-followed component-generation agent id from localStorage.
 * Strategy mirrors the legacy resolveFollowedComponentGenerationAgentId in component-generator.js.
 */
export function loadFollowedComponentGenerationAgentId(storage = globalThis.localStorage) {
  if (!storage || typeof storage.getItem !== "function") return "codex";
  const agentAppearanceMap =
    parseJsonObject(storage.getItem(AGENT_APPEARANCE_MAP_STORAGE_KEY)) || {};
  const enabledAgents = parseJsonArray(storage.getItem(ENABLED_AGENTS_STORAGE_KEY));
  return resolveFollowedComponentGenerationAgentId({ agentAppearanceMap, enabledAgents });
}

/**
 * Pure resolver — exported for use by A5 rewiring and tests.
 */
export function resolveFollowedComponentGenerationAgentId({
  agentAppearanceMap = {},
  enabledAgents = [],
} = {}) {
  const followedAgentId = firstCodingAgentId(
    Object.entries(agentAppearanceMap || {})
      .filter(([, appearanceId]) => Boolean(appearanceId))
      .map(([agentId]) => agentId),
  );
  if (followedAgentId) return followedAgentId;

  const enabled = Array.isArray(enabledAgents) ? enabledAgents : [...(enabledAgents || [])];
  return firstCodingAgentId(enabled) || "codex";
}

/**
 * Map an agent id to its display label.
 * Unknown ids fall back to "Codex".
 */
export function labelForComponentGenerationAgent(id) {
  return AGENT_LABEL_MAP[id] ?? "Codex";
}
