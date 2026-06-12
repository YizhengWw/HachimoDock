# Pet Manager 整体布局重排设计

## Context

`claw-pet-manager/ref` 是 Tauri 2 + React 桌面端，包含三个一级页面：

- **设备页**（`DeviceDashboard.jsx`，1964 行）：已绑定设备的主控台，展示连接状态、当前展示形象、语音助手、板端按钮配置
- **形象画廊**（`AppearanceGallery.jsx`，1323 行）：浏览内置和自定义形象，提供 Codex/社区导入
- **组件中心**（`ComponentCenter.jsx`，1092 行）：负一屏组件商店，组件库 + AI 生成草稿 + clawpkg 拖入安装

侧栏导航在三页之间切换。三页各自迭代，没有统一的视觉骨架或共享的状态层。

## Problems

### 跨页面共性问题

1. **视觉骨架三套写法**

| 页面 | 头部 | section 卡片 |
|------|------|------|
| 设备 | `page-hero` + 右上 help | `panel-card` |
| 画廊 | `page-toolbar` + `page-hero` + `gallery-actions` 三层堆叠 | 无统一卡 |
| 组件中心 | `component-store-hero` | `component-store-section` / `component-tool-card` / `component-device-panel` 三种 |

2. **跨页上下文丢失**：在画廊里看不到「哪个形象正在用」；在组件中心看不到「设备是否在线」；操作完一件事要靠记忆切回设备页确认。

3. **三页重复轮询**：USB 状态、设备在线、formosa map 在每个页面的 `useEffect` 里各轮询一次，是另一种"信息重复"。

4. **散乱细节**：画廊 `sync-notice` 用 inline style 写 fixed 浮层（应该是 App 级 toast）；各页 hero 字号、按钮位置、间距靠各自 class 控制，没有共享 token。

### 设备页内部问题

1. **信息重复 3 次**：连接状态出现在页头摘要、运行状态卡、语音卡的"板子离线"提示；"通信方式 / 连接模式 / Bridge 状态"三个字段说的是同一件事。
2. **危险/低频操作占 C 位**：解绑 + 发送测试消息排在页面第二屏。
3. **「换形象」要 3 次点击**：点渠道卡 → 展开 → 点"设置形象" → 弹 picker → 再点"设为桌宠"。
4. **板端按键配置被塞进语音卡**：这两个功能领域不同，强行嵌套导致语音卡变得超重。
5. **按钮配置不显眼**：用户最关心的"每个按键当前是什么功能"被埋在卡片深处，需要展开多层才能看到。

### 画廊页内部问题

1. **3 个创建按钮独占第一屏一行**（新建 / Codex / 社区），把网格推到第二屏。
2. **每张卡塞「配置渠道」按钮**：但渠道分配本是设备页的事，画廊既要做"浏览"又要做"管理"，定位混乱。
3. **当前正在用的形象没有标识**：画廊里看不出来哪张是设备正在展示的。

### 组件中心内部问题

1. **「3 步教程」和「组件库」并列**：但教程是"如何造"、库是"挑现成"，两件事强行并列。
2. **右侧「安装预览」未选组件时空白**：缺占位提示，且预览只在选中后才有意义，没分清主次。

## Design Principles

- **less is more**：减少卡片数量、减少点击层级，每个 section 只表达一件事
- **职责分明**：设备页管"渠道分配 + 板端配置"，画廊管"浏览 + 一键应用到当前渠道"，组件中心管"挑组件 + 装到设备"
- **跨页上下文常驻**：通过侧栏底部 ContextRail 和 DeviceContextProvider，让"设备/形象/组件"状态在任何页面都可见
- **持久化即可见**：用户已经持久化的配置（按钮映射）必须 first-class 展示，不是高级折叠

## Architecture: Shared Shell (Plan 1)

### 1. `<PageShell>` — 每页 top-level wrapper

```jsx
<PageShell
  title="桌搭控制台"
  subtitle="..."                       // 可选
  actions={[<Button>...</Button>]}    // 可选，右上区
  help={() => setGuideOpen(true)}     // 可选，提供 ? 图标，仅传了才渲染
>
  <Card>...</Card>
</PageShell>
```

内部统一渲染 `<PageHeader/>`，吃掉 `page-hero` / `page-toolbar` / `component-store-hero` 三套。

### 2. `<Card>` — 统一 section 卡片

```jsx
<Card title="按钮配置" subtitle="..." actions={[<Button>...</Button>]}>
  ...body...
</Card>

// 折叠态（设备页"语音助手"用）
<Card.Collapsible
  title="语音助手"
  summary="已开启 · top_button.hold"   // 折叠时显示的一行摘要
  defaultOpen={false}
>
  ...body...
</Card.Collapsible>
```

废掉 `panel-card` / `component-store-section` / `component-tool-card` / `component-device-panel` 四套。

**Card 本身不带 `tone`**：状态用 banner / chip 表达，Card 保持中性。

### 3. `<ContextRail>` — 侧栏底部常驻条

固定在 sidebar 底部，3 行紧凑信息，各自可点：

```
┌─ 侧栏底 ─────────────┐
│ 🖥️ 板子在线 USB     │  → 点跳设备页
│ 🐱 codex pet 缩略   │  → 点跳形象详情
│ 🧩 时钟组件          │  → 点跳组件中心
└──────────────────────┘
```

- 未绑定时整条收成 `[+ 绑定设备]` 一行
- 不可拖动、不可隐藏（导航辅助信息，藏起来无意义）
- 数据全部从 `useDeviceContext()` 读取

### 4. `<ToastStack>` + `useToast()` — 全局通知

- 现有 App.jsx 的 ToastStack 加 React Context，暴露 `useToast().push({tone, title, message?})`
- 迁移：画廊的 inline-style `sync-notice` 全部走 toast；设备页同步成功/失败提示也走 toast
- **保留**：表单/卡片就近的 `message-banner`（如"形象加载失败"）—— 这类错误必须出现在被影响的内容旁边

### 5. `<DeviceContextProvider>` — 共享设备状态

挂在 App.jsx 最外层，向下提供 `useDeviceContext()`：

```ts
{
  binding: Binding | null;           // 当前绑定
  usb: { connected: boolean; portName: string; boardDeviceId: string };
  deviceOnline: boolean;
  onlineBoardDeviceId: string;
  agentAppearanceMap: { [agentId]: appearanceId };
  enabledAgents: Set<agentId>;        // 实际单选
  currentDisplay: {
    agentId: string;
    appearance: AppearanceRecord | null;
    channelLabel: string;
  };
  currentComponent: { id: string; name: string } | null;
  applyDesktopPetAssignment: (agentId, appearance) => Promise<void>;
  refresh: () => Promise<void>;
}
```

`applyDesktopPetAssignment` 是一个薄包装，内部调用现有
`lib/desktop-pet-assignment.js#applyDesktopPetAssignment`，自动注入 `invoke`、
`listen`、`agentAppearanceMap`、`agentOptions`、`boardDeviceId`、
`currentAppearanceId`、`deviceOnline`，让 UI 层只传 `(agentId, appearance)`。
3 类原子操作的语义和错误行为完全沿用既有实现。

Provider 内部承接现有 `usb_get_status` / `check_device_availability` / `load_bridge_profile` 等轮询，三页都从 context 读，删掉各自的 `useEffect` 轮询。

## Page IA

### Plan 2: Device Dashboard

```
<PageShell title="桌搭控制台" help={openGuide}
           actions={[
             <Menu>
               发送测试消息
               复制桌面设备 ID
               解绑设备 (danger)
             </Menu>
           ]}>

  ─ 区 1：设备状态条 ──────────────────────────
  <Card>
    [🖥️] 板子ID · WiFi: xxx       [USB直连 / 在线 / 离线 chip]
  </Card>

  ─ 区 2：当前展示 ────────────────────────────
  <Card title="当前展示">
    ┌─大形象预览─┐  渠道：[codex ▾]    ← 独立下拉
    │  160×120   │
    │ [类型徽章] │  形象：xxx [更换 ▾] ← 独立按钮
    └────────────┘
    (banner: 同步进度 / USB提示 / 错误)
  </Card>
  // 「更换 ▾」打开 formosa picker：视觉上沿用现有
  // `AgentAppearancePickerModal` 的卡片网格模式（已实现于
  // DeviceDashboard.jsx），重做以匹配新 Card / button 设计 token。
  // picker 不再让用户选渠道，只选 formosa，对当前渠道生效。

  ─ 区 3：按钮配置 (always visible, 不折叠) ──
  <Card title="按钮配置" subtitle="按键当前的作用，可直接编辑">
    [SVG 板端示意图，每个按钮 callout 处直接写当前 action label]
    ─────
    [5 行按钮 → 功能映射，下拉直接改]
       某行若是 voice_ptt：右侧加 chip「语音助手已开启」/「未开启」
    ─────
    [USB OTA 下发] (待生效徽章 if dirty)
  </Card>

  ─ 区 4：语音助手 (折叠) ────────────────────
  <Card.Collapsible title="语音助手"
                    summary="已开启 · 续接最近会话">
    [启用语音开关 + 续接会话下拉 + 启动/停止板端收听]
  </Card.Collapsible>
</PageShell>
```

#### 渠道与形象的独立切换（重要）

数据模型（不变）：

```
agentAppearanceMap: { codex: 'A', claude-code: 'B', ... }  // 每渠道独立记忆
enabledAgents: 单元素 Set                                   // 当前跟随渠道
当前展示 = (enabledAgents.first, map[enabledAgents.first])
```

两个独立入口对应代码层 3 类原子操作（出自 `applyDesktopPetAssignment`）：

| UI 入口 | agentId 变化 | appearance 变化 | USB 要求 | 是否弹 confirm |
|---------|---|---|---|---|
| 渠道下拉切到 X，且 `map[X]` 与当前 formosa 相同 | 变 | 不变 | 在线/USB 任一 | 弹（沿用现有 ChannelSwitchConfirmModal）|
| 渠道下拉切到 X，且 `map[X]` 与当前 formosa 不同 | 变 | 变 | **必须 USB** | 弹 + 提示要 USB |
| 渠道下拉切到 X，且 `map[X]` 不存在 | 变 | 沿用当前并写进 map | 在线/USB 任一 | 不弹 |
| 「更换 ▾」选新形象 | 不变 | 变 | **必须 USB** | 不弹 |

`ChannelSwitchConfirmModal` 复用现有逻辑。

#### 按钮配置 SVG 的升级

现有 `BoardButtonMap` SVG 只高亮 voice_ptt 按钮。重做后：

- 每个按钮 callout 旁边显示**当前 action 的 label**（不只是高亮）
- voice_ptt 按钮额外用颜色 + 图标区分
- 鼠标 hover SVG 上的按钮时，下方对应的编辑行高亮
- SVG 始终展示设备真实状态（来自 `voiceConfig.buttonActions`，已持久化）

#### 已删除的元素

- 「运行状态」卡整张删除（Bridge/USB 合并到状态条 chip，桌面 ID 进菜单，测试/解绑进菜单）
- 「设备展示配置」卡的"渠道列表 + 展开形象 + picker"三层结构替换为「当前展示」卡的两个独立入口
- 子组件 `DesktopPetAssignmentPanel` 重写为更轻的 `CurrentDisplayCard`
- 子组件 `VoiceAssistantPanel` 拆出 `BoardButtonPanel`（区 3 用）和 `VoiceAssistantPanel`（区 4 用）

### Plan 3: Appearance Gallery

```
<PageShell title="形象画廊" subtitle="..."
           actions={[
             <Button>刷新</Button>,
             <SplitButton label="添加形象 ▾">
               新建自定义形象 / 从 Codex 导入 / 从社区导入
             </SplitButton>
           ]}>

  ─ 任务进行中（条件渲染）────────────────────
  <Card>{<RunningTaskCard/>}</Card>

  ─ 形象网格 ─────────────────────────────────
  <div className="appearance-grid">
    {items.map → <AppearanceCard
      isActive={当前正在用?}            // "使用中"徽章 + 高亮边框
      onClick={openDetail}
      onSetAsDesktopPet={openChannelModal}   // hover/选中时显示"设为桌宠"按钮
    />}
  </div>

  ─ AppearanceChannelModal (保留) ────────────
  // 用户在画廊里也能选渠道并应用，不强制切回设备页
  // Modal 视觉重做以匹配新设计系统，逻辑层不变
</PageShell>
```

#### 关键决定

- **3 个创建按钮收成 PageShell actions 里的 `添加形象 ▾` split-button**：腾出第一屏纵向空间。
- **画廊保留完整渠道选择能力**：用户明确要求"在画廊里换渠道更方便"，所以 `AppearanceChannelModal` **不简化、不废除**，只做视觉重做。每张卡 hover/选中时浮出「设为桌宠」按钮，点击打开 modal 让用户选目标渠道并应用。
- **"使用中"徽章**：通过 `useDeviceContext().currentDisplay.appearance?.id === row.id` 判定。
- **画廊的 inline-style `sync-notice` 删除**：成功/失败通过 `useToast().push()` 走 App 级 ToastStack。

### Plan 4: Component Center

```
<PageShell title="组件中心" subtitle="选一个负一屏组件，推到桌搭子"
           actions={[
             <Button>刷新草稿</Button>,
             <Button icon={Sparkles}>创建组件 ▾</Button>
                  // 点击打开抽屉/弹层，里面是原 3 步教程
                  // (装 skill / 描述生成 / 拖入 clawpkg)
           ]}>

  ─ 两栏布局 ─────────────────────────────────
  ┌─ 主区：组件库网格 ────────────┬─ 侧区：选中预览 ───┐
  │ [内置1] [内置2] [草稿1] ...  │  ┌─设备屏预览─┐   │
  │   ↑ 草稿和内置混排              │  │  组件 demo │   │
  │   靠卡片角徽章区分              │  └────────────┘   │
  │                                │  组件名 · 描述     │
  │                                │  [安装到设备]     │
  │                                │                    │
  │                                │  (未选时显示软提示  │
  │                                │   "选一个组件预览  │
  │                                │   和安装")         │
  └────────────────────────────────┴────────────────────┘
</PageShell>
```

#### 关键决定

- **3 步教程从主流变为 actions 抽屉**：腾出主区让"挑组件"成为唯一焦点。点击 `创建组件 ▾` 从屏幕右侧滑出抽屉，抽屉内容直接复用现有 3 个 `component-tool-card`，只是从 layout 流程里抽出来。
- **草稿和内置同网格陈列**：保持现有顺序（内置在前、草稿在后），都用同一种 card 组件，靠卡片角徽章（"内置" / "自定义"）区分来源。用户视角下都是"可装的组件"。
- **侧区未选中时显示软提示**：不留空白，引导用户挑组件。
- **安装预览的设备屏 demo 复用现有 `component-device-screen`** 逻辑。

## Implementation Order & Dependencies

```
       ┌─ Plan 1: shared-shell ─┐    [依赖: 无]
       │  PageShell, Card,      │
       │  ContextRail,          │
       │  ToastStack+useToast,  │
       │  DeviceContextProvider,│
       │  接入 App.jsx 最外层    │
       └───────────┬────────────┘
                   │ (lock shell API)
        ┌──────────┼──────────┐
        ▼          ▼          ▼
   Plan 2     Plan 3      Plan 4         ← 并行执行
   device-    appearance-  component-
   dashboard  gallery      center
```

**Plan 1 必须最先且单独完成**：它定义 shell 组件 API 和 DeviceContextProvider 数据契约，下游 3 个 Plan 全部依赖。Plan 1 合并后，shell API 视为 frozen。

**Plan 2/3/4 并行执行**，工作互不耦合：
- 各自只改自己页面文件（`DeviceDashboard.jsx` / `AppearanceGallery.jsx` / `ComponentCenter.jsx`）和对应 test
- 各自的 CSS 仅作用于自己的 page-level class（`.page-dashboard` / `.page-gallery` / `.page-component-center`）
- 公共 token、Card 样式、ToastStack 在 Plan 1 已落地，Plan 2-4 只消费不修改

**并行执行的风险与对策**：

| 风险 | 对策 |
|------|------|
| shell API 在 Plan 2-4 落地时被发现不够用 | Plan 1 完成后由各 Plan 第一步做"API 试用"（构造最小可用页面），暴露的缺口集中提一个 shell amendment PR，再各 Plan 继续；不允许 Plan 2-4 自行修改 shell 文件 |
| DeviceContextProvider 字段 3 页都要加 | 同上：Plan 1 完成后冻结字段，新增字段走 shell amendment |
| CSS token / 变量冲突 | Plan 1 定下所有共享 token（颜色、间距、圆角、字号），写在 `styles.css` 顶部命名空间；Plan 2-4 不得添加新 token，只能 compose |
| 三页同时改 `App.jsx` 路由 | 不会发生：App.jsx 在 Plan 1 已重排（挂 DeviceContextProvider、ContextRail），Plan 2-4 不动 App.jsx |

每个 Plan 完成后跑该页面的 jest 测试（`*.test.js`），并人工 smoke test。三页都合并后做一次跨页面端到端验证（切设备→画廊→组件中心，状态条更新、toast 串通）。

## Testing

- **Plan 1**：为 4 个 shell 组件 + DeviceContextProvider 写单元测试（render + props 行为）
- **Plan 2-4**：保留现有 `DeviceDashboard.test.js` / `AppearanceGallery.test.js` 等的"用户行为"断言，重排后改对应的 query selector / role；不删测试用例
- 重点回归：
  - 设备页：渠道独立切换 / 形象独立切换 / 按钮配置编辑 + OTA 下发 / 语音开关
  - 画廊：3 个导入入口、"设为桌宠" modal、"使用中"徽章随设备状态更新
  - 组件中心：内置 + 草稿混排、安装到设备、创建抽屉 3 步教程

## Out of Scope

- **不重构后端 Tauri 命令**：所有 `invoke("xxx")` 调用、所有 Rust 端逻辑、所有 MQTT/USB 通信协议保持不变
- **不重写 `applyDesktopPetAssignment`**：3 类原子操作的语义不变，只改 UI 入口的拆分方式
- **不动持久化 schema**：`agentAppearanceMap` / `enabledAgents` / `VOICE_CONFIG_STORAGE_KEY` / `pet-bridge.json` 全部沿用
- **不改 DeviceSetup（绑定流程）**：本次只动 dashboard / gallery / component-center 三页 + App 层 shell
- **不引入新依赖**：在现有 React + Tauri + lucide-react 体系内完成
- **CSS 旧 class 的最终清理**作为 Plan 1-4 之外的"垃圾回收"延后处理；本次重排允许新旧 class 短期共存，但每个 Plan 必须保证自己页面的视觉不破

## Open Questions Resolved (本次 brainstorm 中已确定)

| 议题 | 决定 |
|------|------|
| 设备页是否做综合 picker（渠道+形象同时选） | 否。拆成两个独立入口（用户操作意图通常只动一个轴）|
| 画廊是否保留 `AppearanceChannelModal` 选渠道能力 | 保留。用户明确要求画廊里换渠道"更方便"|
| Card 是否带 `tone` | 否。状态用 banner / chip 表达，Card 中性 |
| PageShell 的 `help` 是否所有页都强制 | 否。谁需要谁传 |
| 板端按钮配置是否折叠 | 否。"持续可见"是本次需求核心 |
| 组件中心草稿和内置是否分两个分组 | 否。混排 + 徽章区分 |
| 画廊/组件中心刷新是否独立按钮 | 是。刷新是高频快速操作 |
| 是否抽 DeviceContextProvider lift 共享状态 | 是。Plan 1 范围内 |
| 落地顺序 | Plan 1 单独先行 → Plan 2/3/4 并行；shell API 合并后冻结 |
