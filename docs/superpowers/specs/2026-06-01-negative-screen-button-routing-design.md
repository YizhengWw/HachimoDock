# 负一屏组件按键路由修复 — 设计

## 1. 背景 / 问题

内置负一屏组件（摸鱼倒计时 / 番茄钟 / 喝水提醒 / Token）的按钮映射从不生效：
UI 显示「切换显示→红色按钮」「重设下班→红色按钮长按」「调整下班→旋钮」，
但真机上按红钮、转旋钮都没反应。

**根因：两套按键词表对不上。**

- widget 的 `buttons.json` 用**原生硬件词表**寻址：`红色按钮 / 旋钮 / 屏幕区域`
  + `button.primary.short_press|long_press / knob.rotate_cw / knob.rotate_ccw /
  screen.region.tap|long_press`。`board-widget-runtime.py` 用精确 (control,event)
  匹配这张表。
- 板端输入层只读 `.button-config`（系统动作词表），且把 3 个通用动作
  `negative_screen_primary|secondary|adjust` **压平**成 3 个通用 widget 事件。
  其中红钮被错误翻译成 `screen.region.tap`（**冒充屏幕点击**），与真正的屏幕点击
  撞同一个事件；红钮的原生 `button.primary.*` 从不被发出 → `clock.switch_view` /
  `clock.reset_offhour` 永远收不到。

## 2. 设计原则（铁律）

- **每个物理控件唯一定义**，禁止一个键被两个功能重复绑定。
- **优先级**：`voice_ptt` > 系统功能(`system_page`/`system_reset`/`volume_adjust`)
  > 负一屏 widget(`negative_screen_*`) > 默认兜底(切屏/重启)。**负一屏次优先**。
- **切屏永远靠屏幕滑动兜底**（`board_touch_input` swipe），绝不因按键让位而丢失。

## 3. 物理控件 → widget 原生事件（widget 激活且 `.screen-page=stats` 时）

当某控件在 `.button-config` 被指派给 `negative_screen_*` 时，板端投递该控件的
**原生** `{control,event}` 到 `.widget-events`（不再压平）：

| 物理控件 | 投递事件 | 说明 |
|---|---|---|
| 红钮短按 | `{红色按钮, button.primary.short_press}` | |
| 红钮长按 | `{红色按钮, button.primary.long_press}` | 长按无独立 `.button-config` 项，**继承短按归属** |
| 旋钮旋转 | `{旋钮, knob.rotate_cw / knob.rotate_ccw}` | 仅 `negative_screen_adjust` 路由 |
| 屏幕点击/长按 | `{屏幕区域, screen.region.tap|long_press}` | 发**实际手势**的原生事件，不按 primary/secondary 槽位串线 |
| 旋钮按键 | —（不路由）| widget 无原生绑定，桌面端已去掉其 `negative_screen_*` 选项 |

## 4. 冲突决策

1. **红钮冒充屏幕点击**（重复定义）→ 改发原生红钮事件。
2. **旋钮按键冒充屏幕**（重复定义）→ 桌面去掉其负一屏选项；板端即便收到也消费不冒充。
3. **红钮长按：widget(重设下班) vs 系统重启** → 红钮一旦设为负一屏，**短按+长按都给
   widget**；系统重启/重置保留在**旋钮长按**(`system_reset`，默认就有家)。
4. **旋钮：切屏 vs 音量 vs 负一屏调整** → **默认音量**(`volume_adjust`)；widget 调整需
   用户显式把旋钮设为「负一屏调整」。修掉了 `DEFAULT_BUTTON_ACTIONS.encoder_rotate`
   原为 `system_page` 导致新音量功能默认失效的不一致。

## 5. 改动文件

| 文件 | 改动 |
|---|---|
| `board-runtime/src/board_rotary_input.c` | `br_action_is_negative_screen` / `br_rotary_top_button_owned_by_widget` 助手；红钮负一屏路由改发原生事件 + 长按继承归属 + owned 时抑制长按重启；旋钮只认 `negative_screen_adjust` |
| `board-runtime/src/board_touch_input.c` | 触屏负一屏路由改发实际手势的原生事件 |
| `ref/src/DeviceDashboard.jsx` | 旋钮按键去掉 `negative_screen_*` 选项；旋钮旋转去掉 `negative_screen_primary` 选项；旋钮默认 `system_page → volume_adjust` |
| 内置 widget `buttons.json` | **不变**（本就是原生词表，是修复后的正确契约） |

## 6. 开箱即用：安装时一键应用推荐按键（已实现）

板端默认仍是红钮=`voice_ptt`（语音默认关）、旋钮=音量；但**安装/激活负一屏组件时**，
桌面端自动应用「推荐按键」预设并 USB OTA 下发，让显示的绑定开箱即真。

- 预设 `WIDGET_RECOMMENDED_BUTTON_ACTIONS`（`ref/src/DeviceDashboard.jsx`）：
  `top_button → negative_screen_primary`、`encoder_rotate → negative_screen_adjust`，
  保留 `encoder_button_short → system_page`（切屏）、`encoder_button → system_reset`（重置），
  屏幕点击/长按维持负一屏。
- `applyRecommendedButtonConfigForWidget`：写共享 store（`VOICE_CONFIG_STORAGE_KEY`）+ 派发
  `storage` 事件（设备页 `DeviceDashboard` 监听后重读，面板实时反映）+ 经
  `dispatchBoardButtonConfig` OTA（`voiceEnabled:false`，因为红钮原是语音触发）。
- `ComponentCenter.installSelectedComponent` 在安装管线后调用它，并 toast 提示
  「已应用推荐按键…覆盖了原音量/语音…可在设备页改回」。未连设备时只存 store，连上后下发。
- 连带修正：`ComponentCenter.CONTROL_OPTIONS` 的红钮短/长按不再标 `systemReserved`
  （该标记基于本次修掉的旧硬绑定），`isSystemReservedControl` 已删除——这也消除了它与
  既有引导/生成提示「可把红钮绑定给 widget」的矛盾。

权衡：应用预设会覆盖用户原有的旋钮音量 / 红钮语音绑定（toast 告知，可在设备页改回）；
若安装时语音处于开启状态，预设会把语音关掉（红钮是默认语音触发）。

## 7. 验证

- ✅ 本机：`board-rotary-input` / `board-touch-input` build 干净；`board-runtime-tests`
  通过；桌面 `npm test` 285/285 通过。
- ⏳ 真机（需 redeploy 到 Pi）：把红钮设「负一屏主操作」、旋钮设「负一屏调整」并 USB OTA
  下发，然后按红钮短/长按、转旋钮、点屏幕，验证 `.widget-events` 行与
  `board-widget-runtime` 日志命中对应 `clock.*` action。
