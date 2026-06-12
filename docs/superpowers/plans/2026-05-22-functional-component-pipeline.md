# 功能组件生成与安装管线 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让用户在组件中心点一下,就能用自然语言交给 Codex/Claude Code 生成一个真实可安装的 `.clawpkg` 功能组件;并把 token 消耗 / 番茄时钟两个内置案例做成可经 USB OTA 装到设备的样例。

**Architecture:** 三阶段。Phase A(`claw-pet-manager` 客户端)落地 `.clawpkg` / `COMPONENT_DASHBOARD_V1` 契约、生成 prompt 模板、`launch_agent_with_prompt` Tauri 命令、组件中心接线、两个内置案例定稿。Phase B(`board-runtime` 设备端)给 `fb-speech-overlay` 增加 `COMPONENT_DASHBOARD_V1` 渲染分支。Phase C(跨端)`.clawpkg` 校验 + USB OTA 安装到设备。A 与 B 可并行,C 依赖 A+B。

**Tech Stack:** React 18 + Vite,Tauri 2(Rust),`node:test`(grep 式 Static Node 测试),C11(设备端 armhf 交叉编译)。

**仓库:** `claw-pet-manager`(Phase A、C 客户端侧)、`board-runtime`(Phase B、C 设备侧)。现在位于同一个 git 仓库内,桌面端在 `ref/`,设备端在 `board-runtime/`。

---

## 关键设备契约:`COMPONENT_DASHBOARD_V1`(借鉴自 board-runtime 负一屏)

设备端负一屏(stats 页)今天只渲染一种固定版式 `STATS_DASHBOARD_V1`,由 `src/fb_speech_overlay.c` 的 `br_overlay_render_stats_dashboard` 硬编码绘制。**没有通用布局引擎。** 因此通用组件不能画任意像素,只能填一套**固定槽位**。`COMPONENT_DASHBOARD_V1` 是 `STATS_DASHBOARD_V1` 的泛化,槽位与现有版式一一对应,设备端只需新增一个近似克隆的渲染分支。

固定事实(来自 `fb_speech_overlay.c` / `CLAUDE.md` / `stats-page.md`):

| 项 | 值 |
|---|---|
| 屏幕 | 800×480 LCD,`/dev/fb0`,32bpp BGRA |
| 渲染模式 | `compact`(`fb->height <= 540` 为真,真机恒为 compact) |
| 字体 | GNU Unifont 位图,16px 字格;ASCII 半角 8px 宽,CJK 全角 16px 宽;按槽位 scale 放大 |
| payload 载体 | 文本文件 `.stats-display`,读入 2048 字节缓冲,**payload 必须 < 2048 字节** |
| payload 格式 | 首行是版本标识,其后每行 `key=value` |

`COMPONENT_DASHBOARD_V1` 槽位(maxBytes 为 UTF-8 字节上限,CJK 1 字 = 3 字节;沿用 `br_stats_dashboard_model` 的结构上限):

| 槽位 | maxBytes | 设备版式角色 | 对应 STATS_DASHBOARD_V1 字段 |
|---|---|---|---|
| `title` | 60 | 左上角徽章 | agent |
| `eyebrow` | 90 | 徽章下小号暗色说明 | eyebrow |
| `headline` | 156 | 大号橙色高亮句 | headline |
| `metricLabel` | 90 | 指标面板标题 | metricTitle |
| `metricValue` | 60 | 指标面板大号数值 | metricValue |
| `metricUnit` | 30 | 数值后小号单位 | metricUnit |
| `badge` | 12 | 右上角绿色圆内数字 | completed |
| `note` | 156 | 指标面板下方小号说明行 | breakdown |
| `footer` | 156 | 底部小号提示行(放硬件操作提示) | sources |

> ⚠️ 在真机 compact 模式下,现有代码 `breakdown` / `sources` 不渲染(`if (!compact && ...)`)。Phase B 的新渲染分支需让 `note` / `footer` 在 compact 下也显示(否则两个槽位在真机上看不见)。

**生成器据此生成"刚好能放进去"的内容:** prompt 模板把上表(槽位名 + maxBytes + 角色)交给 agent,要求每个槽位文本不超过 maxBytes,只用上述 9 个槽位,不得自定义布局。

---

## Phase A:客户端生成链路(claw-pet-manager)

**产出可验证软件:** 组件中心"生成组件"按钮 → 用设备契约构建真实 prompt → 打开终端跑 `claude`/`codex` → 在 `~/.openclaw/component-drafts/<时间戳>/` 产出 `.clawpkg` 草稿目录。token 消耗 / 番茄时钟在 `mock-data.js` 里以新契约定稿。

**测试方式:** 项目用 grep 式 Static Node 测试(`import test from "node:test"`,断言源码字符串)。运行:在 `ref/` 目录下 `node --test src/<file>.test.js`。Rust 侧用 `cargo test`(在 `ref/src-tauri/`)。

### Task A1:`.clawpkg` / `COMPONENT_DASHBOARD_V1` 契约模块

**Files:**
- Create: `ref/src/lib/clawpkg-contract.js`
- Test: `ref/src/lib/clawpkg-contract.test.js`

- [ ] **Step 1:写失败测试** — `clawpkg-contract.test.js`:

```js
import test from "node:test";
import assert from "node:assert/strict";
import {
  CLAWPKG_FILES,
  COMPONENT_DASHBOARD_V1_SLOTS,
  validateClawpkgManifest,
} from "./clawpkg-contract.js";

test("clawpkg 六件套结构齐全", () => {
  assert.deepEqual(
    CLAWPKG_FILES.map((f) => f.name),
    ["component.json", "negative-screen.json", "buttons.json", "runtime/", "assets/", "share.json"],
  );
});

test("COMPONENT_DASHBOARD_V1 暴露 9 个固定槽位且带 maxBytes", () => {
  const ids = COMPONENT_DASHBOARD_V1_SLOTS.map((s) => s.id);
  assert.deepEqual(ids, ["title", "eyebrow", "headline", "metricLabel", "metricValue", "metricUnit", "badge", "note", "footer"]);
  assert.ok(COMPONENT_DASHBOARD_V1_SLOTS.every((s) => Number.isInteger(s.maxBytes) && s.maxBytes > 0));
});

test("validateClawpkgManifest 缺文件报错、超字节报错、合法通过", () => {
  const ok = validateClawpkgManifest({
    "component.json": { id: "x", name: "X", version: "1.0.0" },
    "negative-screen.json": { dashboard: { title: "X", headline: "你好" } },
    "buttons.json": [], "runtime/": {}, "assets/": {}, "share.json": { title: "X" },
  });
  assert.equal(ok.valid, true);

  const missing = validateClawpkgManifest({ "component.json": { id: "x", name: "X", version: "1.0.0" } });
  assert.equal(missing.valid, false);
  assert.match(missing.errors.join(" "), /negative-screen\.json/);

  const tooLong = validateClawpkgManifest({
    "component.json": { id: "x", name: "X", version: "1.0.0" },
    "negative-screen.json": { dashboard: { badge: "1234567890123" } },
    "buttons.json": [], "runtime/": {}, "assets/": {}, "share.json": { title: "X" },
  });
  assert.equal(tooLong.valid, false);
  assert.match(tooLong.errors.join(" "), /badge/);
});
```

- [ ] **Step 2:运行测试确认失败** — `cd ref && node --test src/lib/clawpkg-contract.test.js` → 预期 FAIL(`Cannot find module './clawpkg-contract.js'`)。

- [ ] **Step 3:实现 `clawpkg-contract.js`:**

```js
/**
 * [Input] 设备端 fb_speech_overlay.c 的 STATS_DASHBOARD_V1 版式与 br_stats_dashboard_model 字节上限。
 * [Output] .clawpkg 包结构常量、COMPONENT_DASHBOARD_V1 槽位 schema、清单校验函数。
 * [Pos] lib node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

export const CLAWPKG_FILES = [
  { name: "component.json", role: "组件元数据:id、name、version、author、capabilities、入口" },
  { name: "negative-screen.json", role: "负一屏:COMPONENT_DASHBOARD_V1 槽位映射" },
  { name: "buttons.json", role: "默认硬件绑定表" },
  { name: "runtime/", role: "声明式运行逻辑(首版只引用受控能力)" },
  { name: "assets/", role: "图标/声音等静态素材" },
  { name: "share.json", role: "社区分享卡片元数据" },
];

// 槽位上限来自设备端 br_stats_dashboard_model 结构(UTF-8 字节;留 4 字节安全余量)。
export const COMPONENT_DASHBOARD_V1_SLOTS = [
  { id: "title", maxBytes: 60, role: "左上角徽章" },
  { id: "eyebrow", maxBytes: 90, role: "徽章下小号说明" },
  { id: "headline", maxBytes: 156, role: "大号高亮句" },
  { id: "metricLabel", maxBytes: 90, role: "指标面板标题" },
  { id: "metricValue", maxBytes: 60, role: "指标大号数值" },
  { id: "metricUnit", maxBytes: 30, role: "数值单位" },
  { id: "badge", maxBytes: 12, role: "右上角绿色圆内数字" },
  { id: "note", maxBytes: 156, role: "指标下方小号说明行" },
  { id: "footer", maxBytes: 156, role: "底部硬件操作提示行" },
];

const SLOT_BY_ID = new Map(COMPONENT_DASHBOARD_V1_SLOTS.map((s) => [s.id, s]));
const utf8Bytes = (text) => new TextEncoder().encode(String(text ?? "")).length;

export function validateClawpkgManifest(manifest) {
  const errors = [];
  for (const file of CLAWPKG_FILES) {
    if (!(file.name in (manifest || {}))) errors.push(`缺少 ${file.name}`);
  }
  const meta = manifest?.["component.json"];
  if (meta && (!meta.id || !meta.name || !meta.version)) {
    errors.push("component.json 必须含 id、name、version");
  }
  const dashboard = manifest?.["negative-screen.json"]?.dashboard;
  if (dashboard) {
    for (const [slot, value] of Object.entries(dashboard)) {
      const def = SLOT_BY_ID.get(slot);
      if (!def) {
        errors.push(`negative-screen.json 含未知槽位 ${slot}`);
      } else if (utf8Bytes(value) > def.maxBytes) {
        errors.push(`槽位 ${slot} 超出 ${def.maxBytes} 字节上限`);
      }
    }
  }
  return { valid: errors.length === 0, errors };
}
```

- [ ] **Step 4:运行测试确认通过** — `cd ref && node --test src/lib/clawpkg-contract.test.js` → 预期 PASS(3 tests)。

- [ ] **Step 5:提交** — `git add ref/src/lib/clawpkg-contract.js ref/src/lib/clawpkg-contract.test.js && git commit -m "feat: 新增 .clawpkg 与 COMPONENT_DASHBOARD_V1 契约模块"`

### Task A2:组件生成 prompt 模板模块

**Files:**
- Create: `ref/src/lib/component-generation-template.js`
- Test: `ref/src/lib/component-generation-template.test.js`

模块导出 4 个函数(名字与 `ComponentCenter.test.js` 既有断言对齐):`buildComponentGenerationPrompt({description, agentId})`、`createComponentGenerationCommand({description, agentId})`、`createAgentPrompt(description)`、`loadFollowedComponentGenerationAgentId()`、`labelForComponentGenerationAgent(id)`。

- [ ] **Step 1:写失败测试** — `component-generation-template.test.js`:

```js
import test from "node:test";
import assert from "node:assert/strict";
import {
  buildComponentGenerationPrompt,
  createAgentPrompt,
  labelForComponentGenerationAgent,
} from "./component-generation-template.js";

test("prompt 内嵌 .clawpkg 结构、COMPONENT_DASHBOARD_V1 槽位与字节上限", () => {
  const prompt = buildComponentGenerationPrompt({ description: "做一个会议计时", agentId: "codex" });
  assert.match(prompt, /COMPONENT_DASHBOARD_V1/);
  assert.match(prompt, /component\.json/);
  assert.match(prompt, /negative-screen\.json/);
  assert.match(prompt, /headline/);
  assert.match(prompt, /156/);          // 槽位字节上限被写进 prompt
  assert.match(prompt, /做一个会议计时/); // 用户描述被嵌入
});

test("prompt 含 worked example(token 消耗)与缺信息追问要求", () => {
  const prompt = buildComponentGenerationPrompt({ description: "x", agentId: "codex" });
  assert.match(prompt, /token|Token/);
  assert.match(prompt, /信息不足|追问|向用户澄清/);
});

test("createAgentPrompt 对空描述返回澄清式 prompt", () => {
  assert.match(createAgentPrompt(""), /请先描述/);
});

test("labelForComponentGenerationAgent 映射 agent 显示名", () => {
  assert.equal(labelForComponentGenerationAgent("codex"), "Codex");
  assert.equal(labelForComponentGenerationAgent("claude-code"), "Claude Code");
});
```

- [ ] **Step 2:运行测试确认失败** — `cd ref && node --test src/lib/component-generation-template.test.js` → FAIL。

- [ ] **Step 3:实现 `component-generation-template.js`** — 导出上述函数。`buildComponentGenerationPrompt` 用 `CLAWPKG_FILES`、`COMPONENT_DASHBOARD_V1_SLOTS`(import 自 `./clawpkg-contract.js`)拼出模板正文,正文须包含:(a) `.clawpkg` 六件套说明;(b) `COMPONENT_DASHBOARD_V1` 九槽位表(id + maxBytes + role);(c) token 消耗作为完整 worked example(title=Codex / metricLabel=今日累计 Token / metricValue=1.30M / metricUnit=TOKEN …);(d) 三个硬件控件与绑定模型;(e) 产出要求:写满六个文件到当前目录;(f) **明确指令:若用户描述缺少渲染某槽位所需信息(如计时类缺时长),先向用户澄清再生成**。`createAgentPrompt` 空描述时返回 `请先描述你想要的组件功能…`。`loadFollowedComponentGenerationAgentId` 从 `localStorage` 读当前跟随渠道(键复用 `lib/agent-appearance-config.js` 的现有键),缺省 `codex`。`labelForComponentGenerationAgent` 用 `{ codex: "Codex", "claude-code": "Claude Code" }` 映射。

- [ ] **Step 4:运行测试确认通过** — `cd ref && node --test src/lib/component-generation-template.test.js` → PASS(4 tests)。

- [ ] **Step 5:提交** — `git commit -m "feat: 新增组件生成 prompt 模板模块"`

### Task A3:`mock-data.js` 三个内置案例按新契约定稿

> **变更说明（2026-05-23 决策）：** 内置案例从 5/22 plan 原定的两件（`token-usage` + `focus-flow`）改为产品级三件套（`slack-off-countdown` 摸鱼倒计时 + `tomato-clock` 番茄钟 + `drink-reminder` 喝水提醒）。三件均按 `COMPONENT_DASHBOARD_V1` 槽位定义，作为对外开源版本默认装机案例 + 文生组件样板。

**Files:**
- Modify: `ref/src/mock-data.js`(`MOCK_COMPONENT_CENTER.components`)
- Test: `ref/src/ComponentCenter.test.js`(由 Task A3.5 同步)

- [ ] **Step 1:跑既有测试看当前差距** — `cd ref && node --test src/ComponentCenter.test.js`。当前 `MOCK_COMPONENT_CENTER.components` 是 `token-usage` + `focus-flow` 两件、`promptBuilder.title` 已是 `没找到？直接描述组件需求`。我们要替换为三件套并保留同等深度（capabilities / defaultBindings / screens / packageIncludes / hardwareControls / dashboard）。

- [ ] **Step 2:改 `mock-data.js`** —(a)替换 `components` 数组为下面三件（保留外层结构和 `subhead`、`replacementPreview` 等字段；`subhead` 改为"产品上线版三件套：摸鱼倒计时、番茄钟、喝水提醒。其他功能让用户用自然语言按模板生成。"`replacementPreview.currentComponent` 改为 `番茄钟`,`incomingComponent` 改为 `喝水提醒`);(b)每件加一个 `dashboard` 字段（`COMPONENT_DASHBOARD_V1` 九槽位）:

  **slack-off-countdown（摸鱼倒计时）:**
  - `dashboard`: `{ title:"摸鱼倒计时", eyebrow:"距离今天下班", headline:"还有 2 小时 13 分", metricLabel:"下班时间", metricValue:"18:00", metricUnit:"", badge:"5", note:"本周已坚持 5 天", footer:"红钮 切显示 · 旋钮 调下班时间 · 长按 重设" }`
  - `capabilities`: `["clock.local", "schedule.offwork", "calendar.weekend", "display.metrics"]`
  - 主键 `clock.switch_view`、长按 `clock.reset_offhour`、旋钮 `clock.adjust_offhour`、屏幕区域 `clock.show_weekend_easter_egg`。

  **tomato-clock（番茄钟）:**
  - `dashboard`: `{ title:"番茄钟", eyebrow:"当前阶段", headline:"专注中 · 第 2 轮", metricLabel:"剩余时间", metricValue:"24:59", metricUnit:"", badge:"2", note:"本轮目标:写完登录页", footer:"红钮 开始/暂停 · 旋钮 调时长 · 长按 重置" }`
  - `capabilities`: `["timer.focus", "timer.local_state", "achievement.share"]`
  - 主键 `timer.start_pause`、长按 `timer.reset`、旋钮 `timer.adjust_duration`、屏幕区域 `achievement.share`。

  **drink-reminder（喝水提醒）:**
  - `dashboard`: `{ title:"喝水提醒", eyebrow:"距离下次喝水", headline:"还有 18 分钟", metricLabel:"间隔", metricValue:"45", metricUnit:"分钟", badge:"4", note:"今天已喝 4 次（约 1 升）", footer:"红钮 我喝了 · 旋钮 调间隔 · 长按 暂停" }`
  - `capabilities`: `["timer.interval", "reminder.local", "persist.daily_count"]`
  - 主键 `reminder.acknowledge`、长按 `reminder.pause_resume`、旋钮 `reminder.adjust_interval`、屏幕区域 `reminder.show_history`。

  注意:为保证 ComponentCenter.test.js（Task A3.5 同步后）通过,新 components 必须仍包含: `packageIncludes`(["组件说明","负一屏页面","按钮配置","运行文件","资源","分享信息"])、`hardwareControls`、`defaultBindings`(4 行 `red button short / long / knob / screen region`)、`screens` 数组(至少一屏,含 regions)。

- [ ] **Step 3:运行测试** — `cd ref && node --test src/ComponentCenter.test.js` → mock-data 相关断言（在 Task A3.5 同步后）PASS。

- [ ] **Step 4:提交** — `git commit -m "feat: 三件套内置案例按 COMPONENT_DASHBOARD_V1 定稿"`

### Task A3.5:同步 `ComponentCenter.test.js` 到三件套契约

**Files:**
- Modify: `ref/src/ComponentCenter.test.js`

22 号写的 spec test 把 `Token 消耗` / `番茄时钟` / `tokenUsage` / `runtime_stats.c` / `PET_CLAW_STATS_TOKENS_PER_LUNCH` / `focus-flow mock` / `bridge.tokenUsage` / `timer.focus` 硬编码在 assert 里。三件套替换后这些 assert 需要换。

- [ ] **Step 1:把以下 assert 从 ComponentCenter.test.js 中移除**（它们是旧两件套的硬编码,搬到三件套后不再适用）:
  - `assert.match(data, /Token 消耗/);`
  - `assert.match(data, /番茄时钟/);`（保留 `番茄钟` — Task A3.6 替换为新名字）
  - `assert.match(data, /tokenUsage/);`
  - `assert.match(data, /runtime_stats\.c/);`
  - `assert.match(data, /PET_CLAW_STATS_TOKENS_PER_LUNCH/);`
  - `assert.match(data, /focus-flow mock/);`
  - `assert.match(data, /bridge\.tokenUsage/);`
  - `assert.match(data, /timer\.focus/);`

- [ ] **Step 2:加入三件套等价 assert**:
  - `assert.match(data, /slack-off-countdown/);`
  - `assert.match(data, /摸鱼倒计时/);`
  - `assert.match(data, /tomato-clock/);`
  - `assert.match(data, /番茄钟/);`
  - `assert.match(data, /drink-reminder/);`
  - `assert.match(data, /喝水提醒/);`
  - `assert.match(data, /clock\.switch_view/);`
  - `assert.match(data, /timer\.start_pause/);`
  - `assert.match(data, /reminder\.acknowledge/);`
  - `assert.match(data, /dashboard/);` —— 因为每件都有 `dashboard` 字段
  - 保留 `button.primary.long_press` / `knob.rotate_cw` / `screen.region.tap` / `packageIncludes` / `hardwareControls` / 各 `component-*` className 的 assert。

- [ ] **Step 3:更新 jsx 断言部分的"内置案例和 AI 生成"等 UI 文案 assert**（仅当 ComponentCenter.jsx 的实际拷贝随之改动；Task A5 会一起处理）。

- [ ] **Step 4:运行 `cd ref && node --test src/ComponentCenter.test.js` 看 mock-data 相关断言**:在 Task A3 mock-data 完成后,本任务结束 mock-data 部分应 PASS;jsx 部分仍 FAIL(由 A5 处理)。

- [ ] **Step 5:提交** — `git commit -m "test: ComponentCenter.test.js 同步三件套契约"`

### Task A4:`launch_agent_with_prompt` Tauri 命令

**Files:**
- Modify: `ref/src-tauri/src/lib.rs`(新增命令 + 注册到 `invoke_handler`,line 2627)
- Test: `ref/src-tauri/src/lib.rs`(`#[cfg(test)] mod` 内新增单测)

- [ ] **Step 1:写失败测试** — 在 `lib.rs` 测试模块加:

```rust
#[test]
fn agent_binary_maps_known_agents() {
    assert_eq!(agent_cli_binary("codex"), Some("codex"));
    assert_eq!(agent_cli_binary("claude-code"), Some("claude"));
    assert_eq!(agent_cli_binary("unknown"), None);
}
```

- [ ] **Step 2:运行确认失败** — `cd ref/src-tauri && cargo test agent_binary_maps_known_agents` → FAIL(`agent_cli_binary` 未定义)。

- [ ] **Step 3:实现** — 新增:

```rust
fn agent_cli_binary(agent_id: &str) -> Option<&'static str> {
    match normalize_agent_id(agent_id)?.as_str() {
        "codex" => Some("codex"),
        "claude-code" => Some("claude"),
        _ => None,
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct LaunchAgentPromptInput { agent_id: String, prompt: String }

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct LaunchAgentPromptResult { ok: bool, work_dir: String, prompt_file: String }

#[tauri::command]
async fn launch_agent_with_prompt(input: LaunchAgentPromptInput) -> Result<LaunchAgentPromptResult, String> {
    let bin = agent_cli_binary(&input.agent_id)
        .ok_or_else(|| format!("暂不支持的 agent: {}", input.agent_id))?;
    tauri::async_runtime::spawn_blocking(move || -> Result<LaunchAgentPromptResult, String> {
        let ts = current_timestamp_ms();
        let work_dir = get_home_dir()?
            .join(".openclaw").join("component-drafts").join(ts.to_string());
        fs::create_dir_all(&work_dir).map_err(|e| e.to_string())?;
        let prompt_file = work_dir.join("PROMPT.md");
        fs::write(&prompt_file, &input.prompt).map_err(|e| e.to_string())?;
        // macOS: 写一个 .command 脚本,open 后在新 Terminal 窗口里 cd + 跑 agent
        #[cfg(target_os = "macos")]
        {
            let runner = work_dir.join("run.command");
            let script = format!(
                "#!/bin/sh\ncd \"{}\"\nexec {} \"$(cat PROMPT.md)\"\n",
                work_dir.display(), bin);
            fs::write(&runner, script).map_err(|e| e.to_string())?;
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&runner, fs::Permissions::from_mode(0o755))
                .map_err(|e| e.to_string())?;
            command_for_host("open").arg(&runner).status()
                .map_err(|e| e.to_string())?;
        }
        #[cfg(not(target_os = "macos"))]
        {
            return Err("当前仅实现 macOS 终端启动".to_string());
        }
        Ok(LaunchAgentPromptResult {
            ok: true,
            work_dir: work_dir.display().to_string(),
            prompt_file: prompt_file.display().to_string(),
        })
    }).await.map_err(|e| e.to_string())?
}
```

并在 `invoke_handler`(line 2661 `usb_sync_appearance` 之后)加 `, launch_agent_with_prompt`。

- [ ] **Step 4:运行测试确认通过** — `cd ref/src-tauri && cargo test agent_binary_maps_known_agents` → PASS;`cargo build` 编译通过。

- [ ] **Step 5:提交** — `git commit -m "feat: 新增 launch_agent_with_prompt Tauri 命令"`

### Task A5:`ComponentCenter.jsx` 接线生成按钮

**Files:**
- Modify: `ref/src/ComponentCenter.jsx`
- Test: `ref/src/ComponentCenter.test.js`(已存在,即本任务的 spec)

- [ ] **Step 1:跑既有 spec 测试** — `cd ref && node --test src/ComponentCenter.test.js` → FAIL(jsx 缺 `createComponentGenerationCommand`、`createAgentPrompt`、`generatedCommand` 等)。

- [ ] **Step 2:实现 ComponentCenter.jsx** — 满足 `ComponentCenter.test.js` 所有 jsx 断言:import `createComponentGenerationCommand` / `createAgentPrompt` / `loadFollowedComponentGenerationAgentId` / `labelForComponentGenerationAgent`(自 `./lib/component-generation-template`);加 `const [generatedCommand, setGeneratedCommand] = useState("")`;读 `MOCK_COMPONENT_CENTER.componentGenerator.magicMirror` 存进 `magicMirrorHint`;按钮文案 `生成组件`;点击 → `createAgentPrompt(promptDraft)` 构 prompt → `invoke("launch_agent_with_prompt", { input: { agentId, prompt } })` → 把 prompt 摘要写进 `generatedCommand`,渲染在 `.component-generated-prompt` 区块(含文案 `生成组件 prompt`);保留既有卡片选择 / 安装预览 / 替换确认 / 按钮设置。

- [ ] **Step 3:运行测试确认通过** — `cd ref && node --test src/ComponentCenter.test.js` → PASS。

- [ ] **Step 4:浏览器手测** — `cd ref && npm run dev:web`,打开组件中心,确认"生成组件"按钮点击后展示 prompt 区块(终端启动需 Tauri 环境,web 模式下用 `invoke` 的 mock 兜底或仅验证 prompt 展示)。

- [ ] **Step 5:提交** — `git commit -m "feat: 组件中心生成按钮接通生成链路"`

### Task A6:文档同步

**Files:** Modify `ref/src/.folder.md`、`ref/.folder.md`(按 CLAUDE.md update contract:新增 `lib/clawpkg-contract.js`、`lib/component-generation-template.js`,新 Tauri 命令)。

- [ ] **Step 1:** 更新两个 `.folder.md` 的 Files 表与 Architecture 段。
- [ ] **Step 2:提交** — `git commit -m "docs: 同步组件生成链路 folder 文档"`

---

## Phase B:设备端 `COMPONENT_DASHBOARD_V1` 渲染分支(board-runtime)

**产出可验证软件:** 设备负一屏能渲染任意 `COMPONENT_DASHBOARD_V1` payload。**执行前需补充设备侧 recon,再展开为 bite-sized 任务。**

任务概要(每条执行前补全测试代码与实现代码):

- **B1** `fb_speech_overlay.c`:新增 `br_component_dashboard_model`(9 槽位)+ `br_component_dashboard_parse`(识别首行 `COMPONENT_DASHBOARD_V1`),与既有 `STATS_DASHBOARD_V1` 并列。
- **B2** 新增 `br_overlay_render_component_dashboard`:克隆 `br_overlay_render_stats_dashboard` 版式,字段换成 9 个通用槽位;**修正 compact 下 `note`/`footer` 不渲染的问题**,让两行在 480 屏可见。
- **B3** `br_overlay_build_frame`:在 `br_stats_dashboard_parse` 分支旁加 `COMPONENT_DASHBOARD_V1` 分支。
- **B4** 测试:扩展 `tests/` 里 overlay layout 测试(参考既有 `fb-speech-overlay-layout-tests`),覆盖 parse + 槽位越界截断。
- **B5** `stats-page.md` / `device-runtime-design.md` 补 `COMPONENT_DASHBOARD_V1` 文件契约;`scripts/build-armhf.sh` 交叉编译验证。

依赖:`COMPONENT_DASHBOARD_V1` 槽位定义须与 Phase A 的 `clawpkg-contract.js` 完全一致(同一份契约,两端各实现)。

---

## Phase C:`.clawpkg` 校验 + USB OTA 安装(跨端)

**产出可验证软件:** 一个 `.clawpkg`(内置案例或生成草稿)经 USB 装到设备并在负一屏显示。**依赖 A+B;执行前需补充 USB 协议 recon。**

任务概要:

- **C1** Tauri 新增 `validate_clawpkg(path)`:解压 `.clawpkg.zip` → 调与 `clawpkg-contract.js` 等价的 Rust 校验 → 返回清单与错误。
- **C2** Tauri 新增 `install_clawpkg_over_usb(path)`:复用 `usb_sync_appearance` 的分片传输模式(`lib.rs` 既有),把 `negative-screen.json` 的 dashboard 转成 `COMPONENT_DASHBOARD_V1` payload 推到设备 `.stats-display`(或新文件契约)+ 切 `.screen-page`。
- **C3** 设备侧:`board_serial_bridge.c` / USB 接收端支持写入组件 payload 文件;`board_server.c` 增加组件 payload 下发入口。
- **C4** 把 token / 番茄时钟两个内置案例打成真实 `.clawpkg.zip`,验证 USB OTA 全链路:Pet Manager 选案例 → 校验 → USB 传 → 设备负一屏显示。
- **C5** `ComponentCenter.jsx`:草稿目录"导入并安装"入口接 C1+C2。

依赖:C 必须在 A、B 都完成后执行。

---

## 自检结论

- 覆盖 spec:模板配置(A1/A2 借鉴设备契约)、内置案例定稿(A3)、USB OTA(C4)、`launch_agent_with_prompt`(A4)、组件中心接线(A5)、`.clawpkg` 校验+安装(C1/C2)、设备端 `COMPONENT_DASHBOARD_V1` 渲染分支(B)。全部有对应任务。
- Phase A 任务为完整 bite-sized TDD,可直接执行;Phase B/C 为概要,因跨仓库且需设备侧 recon,各自阶段开始前展开为 bite-sized 任务(此为有意分阶段,非占位符)。
- 类型一致性:`COMPONENT_DASHBOARD_V1` 九槽位 id 在 A1、A2、A3、B1 全程一致。
