/**
 * [Input] Product skeleton requirements defined by `ref/.folder.md`.
 * [Output] Three things the React app reads as seed/scenario data, NOT mocks
 *          (the prior MOCK_ prefix misled readers into thinking these would
 *          be replaced by a real API — they will not; they're the real source
 *          of truth for builtin widgets + setup scenarios):
 *            • `AGENT_DISCOVERY_FIXTURES` — scenarios for the agent-detection
 *              flow, consumed by `agent-discovery-contract.js`.
 *            • `DEVICE_SETUP_FIXTURES` — happy/error scenario data for the
 *              setup wizard, consumed by `device-setup-contract.js`.
 *            • `BUILTIN_COMPONENT_CENTER` — the actual builtin negative-screen
 *              widget catalog (slack-off-countdown / tomato-clock / drink-reminder
 *              / token-usage) plus the component-generator prompt + replacement
 *              preview metadata, consumed by `ComponentCenter.jsx`.
 * [Pos] shared-data node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 *        Renamed from `mock-data.js` on 2026-06-01; the 10 unused PRD-era
 *        exports (MOCK_USER, MOCK_PACKS, MOCK_THEMES, MOCK_PET_STATS,
 *        MOCK_POSTCARDS, MOCK_SOUVENIRS, MOCK_TRAVEL_LOGS, INITIAL_PET_DRAFT,
 *        VIEW_IDS, LOGIN_SCENARIOS) were dropped in the same pass.
 */

export const AGENT_DISCOVERY_FIXTURES = {
  ready_available: {
    scannedAt: 1770000000000,
    agents: [
      {
        id: "claude-code",
        label: "Claude Code",
        detected: true,
        ready: true,
        status: "ready",
        detail: "已检测到本地配置与 SessionStart hook，可以直接作为当前主驱动。",
        commandPath: "/usr/local/bin/claude",
        configPath: "~/.claude/settings.json",
        activityPath: "~/.claude/projects",
        canSyncHook: false,
      },
      {
        id: "codex",
        label: "Codex",
        detected: true,
        ready: true,
        status: "ready",
        detail: "已检测到会话日志目录，JSONL 轮询链路可直接接入。",
        commandPath: "/usr/local/bin/codex",
        configPath: "",
        activityPath: "~/.codex/sessions",
        canSyncHook: false,
      },
      {
        id: "cursor-agent",
        label: "Cursor",
        detected: false,
        ready: false,
        status: "not_found",
        detail: "当前没有发现 Cursor hooks.json 或可识别的本地配置。",
        commandPath: "",
        configPath: "",
        activityPath: "",
        canSyncHook: false,
      },
    ],
  },
  no_ready: {
    scannedAt: 1770000005000,
    agents: [
      {
        id: "claude-code",
        label: "Claude Code",
        detected: true,
        ready: false,
        status: "needs_hook",
        detail: "已检测到配置，但还没有把 hook 注册到本机环境。",
        commandPath: "/usr/local/bin/claude",
        configPath: "~/.claude/settings.json",
        activityPath: "~/.claude/projects",
        canSyncHook: true,
      },
      {
        id: "codex",
        label: "Codex",
        detected: false,
        ready: false,
        status: "not_found",
        detail: "没有发现 Codex 命令或会话日志。",
        commandPath: "",
        configPath: "",
        activityPath: "",
        canSyncHook: false,
      },
      {
        id: "cursor-agent",
        label: "Cursor",
        detected: false,
        ready: false,
        status: "not_found",
        detail: "尚未发现 Cursor 本地配置。",
        commandPath: "",
        configPath: "",
        activityPath: "",
        canSyncHook: false,
      },
    ],
  },
};

export const DEVICE_SETUP_FIXTURES = {
  happy_path: {
    device_label: "Companion Side Display",
    desktop_device_id: "desktop-workbench-01",
    board_device_id: "board-side-display-01",
    wifi_ssid: "",
    pet_behavior_state: "pet_focus_loop",
    metrics: {
      token_total: 18640,
      tool_call_count: 42,
      code_line_delta: 628,
      task_completed_count: 8,
    },
  },
  host_link_error: {
    device_label: "Companion Side Display",
    desktop_device_id: "desktop-workbench-02",
    board_device_id: "board-link-lab-02",
    wifi_ssid: "",
    pet_behavior_state: "pet_observe_idle",
    metrics: {
      token_total: 9640,
      tool_call_count: 18,
      code_line_delta: 214,
      task_completed_count: 3,
    },
  },
  network_error: {
    device_label: "Companion Side Display",
    desktop_device_id: "desktop-workbench-03",
    board_device_id: "board-network-lab-03",
    wifi_ssid: "",
    pet_behavior_state: "pet_watchful_wait",
    metrics: {
      token_total: 14280,
      tool_call_count: 27,
      code_line_delta: 351,
      task_completed_count: 5,
    },
  },
  channel_error: {
    device_label: "Companion Side Display",
    desktop_device_id: "desktop-workbench-04",
    board_device_id: "board-channel-lab-04",
    wifi_ssid: "",
    pet_behavior_state: "pet_sync_monitor",
    metrics: {
      token_total: 21930,
      tool_call_count: 51,
      code_line_delta: 782,
      task_completed_count: 11,
    },
  },
};

export const BUILTIN_COMPONENT_CENTER = {
  headline: "内置案例 + AI 生成组件",
  subhead: "产品上线版三件套：摸鱼倒计时、番茄钟、喝水提醒。其他功能让用户用自然语言按模板生成。",
  installPromise: "功能组件 = 进入负一屏即进入场景 + 自运行状态 + 屏幕点击/长按两个动作；旋钮固定用于系统音量",
  promptBuilder: {
    title: "没找到？直接描述组件需求",
    placeholder: "例如：做一个会议计时组件，切到负一屏即显示并运行会议场景；或粘贴 MagicMirror / MMM-* 模块链接。",
    defaultPrompt: "做一个会议计时组件，切到负一屏即进入会议计时场景并显示本轮倒计时和下一场会议；点击屏幕开始/暂停，长按屏幕重置。",
    resultHints: ["按 OpenClaw 组件模板生成", "输出页面、按钮绑定和 runtime 文件", "安装前先预览负一屏和权限"],
  },
  componentGenerator: {
    template: "negative-screen-component",
    packageFiles: ["component.json", "negative-screen.json", "buttons.json", "runtime/", "assets/", "share.json"],
    magicMirror: {
      label: "MagicMirror 模块可转换",
      detail: "MagicMirror 模块会先转换成 OpenClaw 组件，再安装到负一屏；适合 MMM-Weather、MMM-Calendar、MMM-HomeAssistant 这类信息型模块。",
      accepted: ["MagicMirror 模块 GitHub 链接", "MMM-* 模块目录", "config.js 中的一段模块配置"],
    },
  },
  replacementPreview: {
    slot: "负一屏",
    currentComponent: "番茄钟",
    incomingComponent: "喝水提醒",
    replacedItems: ["负一屏页面", "按钮功能绑定", "组件素材"],
    keptItems: ["旧组件包", "安装来源", "回退入口"],
    prompt: "当前负一屏已经安装“番茄钟”。继续安装会把负一屏替换为新组件，旧组件会保留在已安装列表里，可以一键恢复。",
  },
  summary: [
    { id: "source", label: "来源", value: "内置 / AI 生成" },
    { id: "screen", label: "页面", value: "负一屏" },
    { id: "bindings", label: "按钮功能", value: "默认绑定" },
    { id: "install", label: "安装", value: "一键应用" },
  ],
  components: [
    {
      id: "slack-off-countdown",
      name: "摸鱼倒计时",
      category: "内置案例",
      source: "本地时间计算",
      status: "available",
      accent: "blue",
      goal: "用最朴素的方式提醒今天还有多久下班，让用户一眼看见自己的节奏。",
      sharePayload: "分享的是摸鱼倒计时组件，包括下班时间设置、节假日彩蛋和提示文案。",
      capabilities: ["clock.local", "schedule.offwork", "calendar.weekend", "display.metrics"],
      packageIncludes: ["组件说明", "负一屏页面", "按钮绑定", "运行文件", "资源", "分享信息"],
      /* mirror builtin-clawpkgs/slack-off-countdown/runtime/widget.json. mid-day
         demo state: 当前 14:30, off_hour=18, 计算约 50% 已过(实际设备装上
         after-9am-and-before-6pm 都会算实时百分比). */
      dashboard: {
        title: "🌅 摸鱼倒计时",
        eyebrow: "距离今天下班",
        headline: "⏰ 今天 18:00 下班",
        metricLabel: "下班时间",
        metricValue: "18",
        metricUnit: ":00",
        badge: "5",
        note: "🔥 本周已坚持 5 天打卡",
        footer: "点击 切显示 · 长按 重设下班时间",
        progress: { value: 60, label: "今日进度" },
      },
      defaultBindings: [
        { control: "屏幕区域", event: "screen.region.tap", action: "clock.switch_view", label: "切换显示" },
        { control: "屏幕区域", event: "screen.region.long_press", action: "clock.reset_offhour", label: "重新设置下班时间" },
      ],
      screens: [
        {
          name: "摸鱼倒计时",
          purpose: "显示距离今天下班的小时分钟、下班时间和本周坚持天数。",
          regions: [
            { name: "倒计时", action: "clock.switch_view" },
            { name: "下班时间", action: "clock.adjust_offhour" },
            { name: "周末彩蛋", action: "clock.show_weekend_easter_egg" },
          ],
        },
      ],
    },
    {
      id: "tomato-clock",
      name: "番茄钟",
      category: "内置案例",
      source: "本地计时",
      status: "installed",
      accent: "green",
      goal: "经典 25 分钟专注 + 5 分钟休息循环，点击屏幕开始或暂停，长按屏幕重置。",
      sharePayload: "分享的是完整番茄钟组件，包含计时引擎、按钮绑定和分享卡片。",
      capabilities: ["timer.focus", "timer.local_state", "achievement.share"],
      packageIncludes: ["组件说明", "负一屏页面", "按钮绑定", "运行文件", "资源", "分享信息"],
      /* mirror builtin-clawpkgs/tomato-clock/runtime/widget.json focus state
         partway through (remaining_s=900 of 1500 = 40% progress). Demo
         preview shows mid-focus look so user sees the timer-running version. */
      dashboard: {
        title: "🍅 番茄钟",
        eyebrow: "🔴 专注阶段",
        headline: "🎯 专注中",
        metricLabel: "剩余",
        metricValue: "15:00",
        metricUnit: "",
        badge: "2",
        note: "🍅 完成进度看右上 badge",
        progress: { value: 40, label: "本轮进度" },
        footer: "点击 开始/暂停 · 长按 重置",
      },
      defaultBindings: [
        { control: "屏幕区域", event: "screen.region.tap", action: "timer.start_pause", label: "开始 / 暂停" },
        { control: "屏幕区域", event: "screen.region.long_press", action: "timer.reset", label: "重置" },
      ],
      screens: [
        {
          name: "番茄钟",
          purpose: "展示当前倒计时、阶段（专注/休息）、当前轮次。",
          regions: [
            { name: "倒计时", action: "timer.start_pause" },
            { name: "本轮目标", action: "achievement.share" },
            { name: "完成分享", action: "achievement.share" },
          ],
        },
      ],
    },
    {
      id: "drink-reminder",
      name: "喝水提醒",
      category: "内置案例",
      source: "本地间隔提醒",
      status: "available",
      accent: "yellow",
      goal: "每 45 分钟提醒一次喝水，点击屏幕确认已喝水，长按屏幕暂停或恢复。",
      sharePayload: "分享的是喝水提醒组件，包括默认间隔、提醒文案和每日次数统计。",
      capabilities: ["timer.interval", "reminder.local", "persist.daily_count"],
      packageIncludes: ["组件说明", "负一屏页面", "按钮绑定", "运行文件", "资源", "分享信息"],
      /* mirror builtin-clawpkgs/drink-reminder/runtime/widget.json initial render
         (state=reminding, since_last_min=18, interval_min=60, drink_count_today=7). */
      dashboard: {
        title: "💧 喝水提醒",
        eyebrow: "距离上次喝水",
        headline: "🥤 别忘了补水",
        metricLabel: "已隔",
        metricValue: "18",
        metricUnit: "分钟",
        badge: "7",
        note: "📊 今日饮水次数会自动累计",
        footer: "点击 我喝了 · 长按 暂停/恢复",
        progress: { value: 30, label: "本次间隔" },
      },
      defaultBindings: [
        { control: "屏幕区域", event: "screen.region.tap", action: "reminder.acknowledge", label: "我喝了" },
        { control: "屏幕区域", event: "screen.region.long_press", action: "reminder.pause_resume", label: "暂停 / 恢复" },
      ],
      screens: [
        {
          name: "喝水提醒",
          purpose: "显示距离下次喝水还有多久、当前间隔、今日已喝次数。",
          regions: [
            { name: "倒计时", action: "reminder.acknowledge" },
            { name: "间隔", action: "reminder.adjust_interval" },
            { name: "今日记录", action: "reminder.show_history" },
          ],
        },
      ],
    },
    {
      id: "token-usage",
      name: "Token 消耗",
      category: "内置案例",
      source: "桌面 bridge 数据",
      status: "available",
      accent: "blue",
      goal: "把当前 coding agent 的实时 Token 消耗推到设备屏,等价换算成几顿工作午餐。",
      sharePayload: "分享的是 Token 消耗组件,包含桌面桥接数据来源、设备渲染版式和等价换算逻辑。",
      capabilities: ["bridge.tokenUsage", "runtime.stats", "display.metrics"],
      packageIncludes: ["组件说明", "负一屏页面", "按钮绑定", "运行文件", "资源", "分享信息"],
      /* mirror builtin-clawpkgs/token-usage/runtime/widget.json with a typical
         mid-day state (1.30M tokens used, 65% of 2M daily budget shown as
         progress). Real installs swap title to current active agent (Claude
         when Claude Code working, Codex when Codex working) via runtime_stats
         active-source pick. */
      dashboard: {
        title: "🤖 Claude",
        eyebrow: "今日消耗等价于",
        headline: "🍱 约 3.7 顿午餐",
        metricLabel: "今日累计 Token",
        metricValue: "1.30M",
        metricUnit: "TOKEN",
        badge: "3",
        note: "📥 输入 900K · 📤 输出 400K",
        footer: "点击 查看拆分 · 长按 刷新",
        progress: { value: 65, label: "今日预算" },
      },
      defaultBindings: [
        { control: "屏幕区域", event: "screen.region.tap", action: "stats.open_breakdown", label: "查看拆分" },
        { control: "屏幕区域", event: "screen.region.long_press", action: "stats.refresh", label: "刷新统计" },
      ],
      screens: [
        {
          name: "Token 消耗",
          purpose: "显示今日累计 Token、输入/输出拆分和工作午餐换算。",
          regions: [
            { name: "今日累计", action: "stats.open_breakdown" },
            { name: "输入输出", action: "stats.switch_source" },
            { name: "午餐换算", action: "stats.open_breakdown" },
          ],
        },
      ],
    },
  ],
  hardwareControls: [
    {
      id: "knob",
      name: "屏幕前红色编码旋钮",
      events: ["knob.rotate_cw", "knob.rotate_ccw", "knob.rotate_cw / knob.rotate_ccw"],
      productMeaning: "固定用于系统音量调节，不在组件中心绑定为 widget action。",
    },
    {
      id: "screen-region",
      name: "屏幕区域",
      events: ["screen.region.tap", "screen.region.long_press", "system.screen.swipe"],
      productMeaning: "适合承载组件内部动作，例如打开详情、分享、切换页面；屏幕滑动仍用于系统切页。",
    },
  ],
};
