/**
 * [Input] Widget buttons.json binding records.
 * [Output] User-facing physical-control labels that distinguish button location
 *          and gesture, plus the install-time bindable CONTROL_OPTIONS and the
 *          helpers ComponentCenter uses to resolve a binding's display label.
 * [Pos] helper node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

export function formatBindingControl(binding = {}) {
  const event = String(binding.event || "").trim();
  if (event === "button.primary.short_press") return "已停用硬件输入";
  if (event === "button.primary.long_press") return "已停用硬件输入";
  if (event === "knob.rotate_cw / knob.rotate_ccw") return "前方旋钮 · 旋转";
  if (event === "knob.rotate_cw") return "前方旋钮 · 顺时针";
  if (event === "knob.rotate_ccw") return "前方旋钮 · 逆时针";
  if (event === "screen.region.tap") return "负一屏屏幕 · 点击";
  if (event === "screen.region.long_press") return "负一屏屏幕 · 长按";
  return binding.control || event || "未指定";
}

const ROUTED_WIDGET_EVENTS = new Set([
  "screen.region.tap",
  "screen.region.long_press",
]);

export function isRoutedWidgetBinding(binding = {}) {
  return ROUTED_WIDGET_EVENTS.has(String(binding.event || "").trim());
}

/* Controls a widget action can be bound to at install time. Each maps to a
 * NATIVE board event the widget runtime matches in buttons.json. Screen region
 * events are forwarded by the touch runtime when the widget is active. Screen
 * swipe stays a pure system page-switch gesture and is intentionally NOT
 * bindable here. Knob rotation is fixed to system volume and is not offered as
 * a live widget control in this UI.
 *
 * 与设备端 board-runtime 同步：负一屏路由见 board_rotary_input.c
 * br_rotary_dispatch_button_action 与 board_touch_input.c；swipe
 * 仍由 screen_page.c 截走切页，因此不在可绑定列表里。
 */
export const CONTROL_OPTIONS = [
  {
    label: "屏幕点击",
    shortLabel: "点击",
    control: "屏幕区域",
    event: "screen.region.tap",
    help: "点 stats 页屏幕区域触发这个组件动作。",
  },
  {
    label: "屏幕长按",
    shortLabel: "长按",
    control: "屏幕区域",
    event: "screen.region.long_press",
    help: "长按 stats 页屏幕区域触发这个组件动作。",
  },
];

export const CONTROL_HELP = Object.fromEntries(
  CONTROL_OPTIONS.map((option) => [option.label, option.help]),
);

export function bindingKey(componentId, action) {
  return `${componentId}:${action}`;
}

// Display label for a binding from the install-time CONTROL_OPTIONS vocabulary
// ("屏幕点击"). Distinct from formatBindingControl above, which renders the
// richer status label ("负一屏屏幕 · 点击") shown elsewhere.
export function defaultControlLabelForBinding(binding) {
  const event = binding.event || "";
  const control = binding.control || "";
  const exactMatch = CONTROL_OPTIONS.find(
    (option) => option.control === control && option.event === event,
  );
  if (exactMatch) return exactMatch.label;
  const eventMatch = CONTROL_OPTIONS.find((option) => option.event === event);
  if (eventMatch) return eventMatch.label;
  const controlMatch = CONTROL_OPTIONS.find((option) => option.control === control);
  return controlMatch?.label || control || CONTROL_OPTIONS[0].label;
}

export function optionForControlLabel(label) {
  return CONTROL_OPTIONS.find((option) => option.label === label) || null;
}
