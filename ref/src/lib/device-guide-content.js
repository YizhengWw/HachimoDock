/**
 * [Input] Static device-control guide content for the first-launch + re-openable
 *         modal in DeviceDashboard.
 * [Output] Card data + storage key + canonical labels used by DeviceGuideModal,
 *          including compact screen/physical-control labels, all ten P4
 *          gestures, and install-time widget button functions.
 * [Pos] lib helper for ref/src/DeviceGuideModal.jsx
 * [Sync] If buttons map on the device (board_rotary_input.c / board_touch_input.c)
 *        changes meaning, update CARDS here. No hardcoded copy in the JSX.
 */

import {
  ONBOARDING_PAGE_IDS,
  onboardingStorageKey,
} from "./onboarding-state.js";

// localStorage key — single source of truth so we don't drift if the modal is
// opened/closed/reset from multiple places later.
export const DEVICE_GUIDE_SEEN_KEY = onboardingStorageKey(ONBOARDING_PAGE_IDS.DEVICE);

// Canonical labels for the physical controls. Used by every card so the
// emoji + name stays consistent across the carousel.
export const CONTROLS = {
  encoder: { emoji: "旋", shortLabel: "旋钮", name: "屏幕前编码旋钮" },
  touch: { emoji: "屏", shortLabel: "屏幕", name: "触摸屏" },
  p4Encoder: { emoji: "旋", shortLabel: "旋钮", name: "ESP32-P4 旋钮" },
  sw1: { emoji: "1", shortLabel: "SW1", name: "SW1" },
  sw2: { emoji: "2", shortLabel: "SW2", name: "SW2" },
  sw3: { emoji: "3", shortLabel: "SW3", name: "SW3" },
};

// Canonical labels for the two device screen pages. Source-of-truth mapping
// from board-runtime src/screen_page.c (main = pet animation, stats = widget).
export const SCREENS = {
  main: { icon: "pet", name: "桌宠主屏", id: "main" },
  components: { icon: "components", name: "组件菜单", id: "components" },
  stats: { icon: "stats", name: "负一屏", id: "stats" },
};

// Guide cards. Keep copy short so the compact modal does not need internal scrolling.
// Each card has a title (rendered in modal header) + content (a list of rows
// or a free-form node id the modal switches on).
export const CARDS = [
  {
    id: "screen-switch",
    title: "🎉 设备绑定完成，先看下基础操作",
    shortTitle: "页面切换",
    headline: "设备屏幕有两个 \"页面\"，随时可以切换",
    canonicalControl: "encoder",
    canonicalActionText: "短按屏幕前红色编码旋钮 = 切到另一页",
    otherWays: [
      {
        control: "encoder",
        text: "转动屏幕前红色编码旋钮 — 调节系统音量",
      },
      {
        control: "touch",
        text: "屏幕滑动 — 任意方向都能触发切屏",
      },
    ],
  },
  {
    id: "controls",
    title: "🎮 设备上的可用控件",
    shortTitle: "设备控件",
    controls: [
      {
        control: "encoder",
        rows: [
          { gesture: "短按", action: "切屏 main ↔ stats" },
          { gesture: "长按", action: "重启设备运行时" },
          { gesture: "顺时针 / 逆时针旋转", action: "调节系统音量" },
          { gesture: "按住 8 秒", action: "⚠️ 重置配网（删 WiFi）", warning: true },
        ],
      },
      {
        control: "touch",
        rows: [
          { gesture: "任意方向滑动", action: "切屏" },
          { gesture: "点击 / 长按", action: "上报给客户端（统计用）" },
        ],
      },
    ],
  },
  {
    id: "widget-takeover",
    title: "🧩 装上 widget 之后，按钮功能跟着组件走",
    shortTitle: "组件按键",
    headline: "组件中心安装时可把屏幕点击/长按绑定给当前 widget；屏幕滑动仍用于切屏。",
    example: {
      name: "Token 消耗",
      rows: [
        { control: "touch", gesture: "点击", action: "查看统计拆分 / 执行主操作" },
        { control: "touch", gesture: "长按", action: "刷新或打开更多操作" },
        { control: "encoder", gesture: "旋转", action: "固定调节系统音量" },
      ],
    },
    footnotes: [
      "不同组件的按钮功能可以不同，安装前在【组件中心】的按钮功能面板里确认。",
      "每个 widget 会把 buttons.json 随安装下发，action 与 runtime/widget.json transitions 对齐。",
    ],
  },
];

export const P4_CARDS = [
  {
    id: "screen-switch",
    title: "用旋钮完成板端导航",
    shortTitle: "旋钮导航",
    headline: "转动选择，短按进入；旋钮长按默认不绑定。",
    screenIds: ["main", "components"],
    canonicalControl: "p4Encoder",
    canonicalActionText: "默认短按确认，左右旋选择上一个或下一个会话；返回（取消）由 SW3 短按完成。",
    otherWays: [
      { control: "p4Encoder", text: "右旋默认：下一个" },
      { control: "p4Encoder", text: "左旋默认：上一个" },
      { control: "p4Encoder", text: "短按默认：确认" },
      { control: "p4Encoder", text: "长按默认：暂不绑定" },
    ],
    gestures: [
      { gesture: "左旋", action: "上一个" },
      { gesture: "右旋", action: "下一个" },
      { gesture: "短按", action: "确认 / 进入" },
      { gesture: "长按", action: "暂不绑定" },
    ],
    supportingText: "这些默认动作可以在 PC 端独立调整；SW3 短按默认承担清晰的返回路径。",
  },
  {
    id: "controls",
    title: "三颗按键，各自承担一个角色",
    shortTitle: "三颗按键",
    headline: "短按与长按互不冲突；先使用安全默认值，再按习惯修改。",
    controls: [
      {
        control: "sw1",
        rows: [
          { gesture: "短按", action: "暂不绑定" },
          { gesture: "长按", action: "按住说话" },
        ],
      },
      {
        control: "sw2",
        rows: [
          { gesture: "短按", action: "组件中心" },
          { gesture: "长按", action: "暂不绑定" },
        ],
      },
      {
        control: "sw3",
        rows: [
          { gesture: "短按", action: "返回（取消）" },
          { gesture: "长按", action: "暂不绑定" },
        ],
      },
    ],
  },
  {
    id: "widget-takeover",
    title: "把 10 个手势调成你的习惯",
    shortTitle: "个性配置",
    headline: "设备导航与组件动作分层保存，修改一个组件不会打乱全局操作。",
    example: {
      name: "推荐起点",
      rows: [
        { control: "sw1", gesture: "长按", action: "语音输入" },
        { control: "sw2", gesture: "短按", action: "打开组件中心" },
        { control: "sw3", gesture: "短按", action: "返回 / 取消" },
        { control: "p4Encoder", gesture: "短按", action: "确认 / 进入" },
        { control: "p4Encoder", gesture: "左旋 / 右旋", action: "上一个 / 下一个" },
      ],
    },
    footnotes: [
      "SW1/SW2/SW3 的短按和长按，加上旋钮的四种手势，共 10 个独立入口。",
      "组件按键只在该组件打开时生效，不覆盖设备页面的默认导航。",
    ],
  },
];
