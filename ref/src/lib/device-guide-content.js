/**
 * [Input] Static device-control guide content for the first-launch + re-openable
 *         modal in DeviceDashboard.
 * [Output] Card data + storage key + canonical labels used by DeviceGuideModal,
 *          including labels for the front red encoder knob/button, touch screen,
 *          and install-time widget button-function gestures.
 * [Pos] lib helper for ref/src/DeviceGuideModal.jsx
 * [Sync] If buttons map on the device (board_rotary_input.c / board_touch_input.c)
 *        changes meaning, update CARDS here. No hardcoded copy in the JSX.
 */

// localStorage key — single source of truth so we don't drift if the modal is
// opened/closed/reset from multiple places later.
export const DEVICE_GUIDE_SEEN_KEY = "pet-manager.device-guide-seen";

// Canonical labels for the physical controls. Used by every card so the
// emoji + name stays consistent across the carousel.
export const CONTROLS = {
  encoder: { emoji: "🔴", name: "屏幕前红色编码旋钮（可按压）" },
  touch: { emoji: "📺", name: "触摸屏" },
};

// Canonical labels for the two device screen pages. Source-of-truth mapping
// from board-runtime src/screen_page.c (main = pet animation, stats = widget).
export const SCREENS = {
  main: { emoji: "🐕", name: "宠物动画", id: "main" },
  stats: { emoji: "📊", name: "负一屏", id: "stats" },
};

// Carousel cards. Keep copy short — modal is 520px wide and shouldn't scroll.
// Each card has a title (rendered in modal header) + content (a list of rows
// or a free-form node id the modal switches on).
export const CARDS = [
  {
    id: "screen-switch",
    title: "🎉 设备绑定完成，先看下基础操作",
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
