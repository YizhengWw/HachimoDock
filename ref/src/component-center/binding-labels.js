/**
 * [Input] Widget buttons.json binding records across screen gestures, P4 switches, and joystick input.
 * [Output] User-facing physical-control labels, concise game-play instructions derived from
 *          the actual package bindings, and a component-action allowlist that hides legacy
 *          package-authored system navigation from previews and installs.
 * [Pos] helper node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

export function formatBindingControl(binding = {}) {
  const event = String(binding.event || "").trim();
  if (event === "button.sw1.short_press") return "SW1 · 短按";
  if (event === "button.sw2.short_press") return "SW2 · 短按";
  if (event === "button.sw3.short_press") return "SW3 · 短按";
  if (event === "button.encoder.short_press") return "前方摇杆 · 中按";
  if (event === "button.encoder.long_press") return "前方摇杆 · 中按长按";
  if (event === "joystick.up") return "前方摇杆 · 向上";
  if (event === "joystick.down") return "前方摇杆 · 向下";
  if (event === "knob.rotate_cw / knob.rotate_ccw") return "前方摇杆 · 左右";
  if (event === "knob.rotate_cw") return "前方摇杆 · 向右";
  if (event === "knob.rotate_ccw") return "前方摇杆 · 向左";
  if (event === "screen.region.tap") return "负一屏屏幕 · 点击";
  if (event === "screen.region.long_press") return "负一屏屏幕 · 长按";
  return binding.control || event || "未指定";
}

const JOYSTICK_DIRECTION_EVENTS = new Set([
  "joystick.up",
  "joystick.down",
  "knob.rotate_cw",
  "knob.rotate_ccw",
  "knob.rotate_cw / knob.rotate_ccw",
]);

const COMPACT_CONTROL_LABELS = {
  "button.sw1.short_press": "SW1",
  "button.sw2.short_press": "SW2",
  "button.sw3.short_press": "SW3",
  "button.encoder.short_press": "摇杆中按",
  "button.encoder.long_press": "摇杆中按长按",
  "screen.region.tap": "屏幕点击",
  "screen.region.long_press": "屏幕长按",
};

function bindingActionLabel(binding = {}) {
  return String(binding.label || binding.action || "").trim();
}

/** Build one short, mapping-aware sentence for game cards and their detail view. */
export function buildComponentPlayGuide(bindings = []) {
  const routed = (Array.isArray(bindings) ? bindings : [])
    .filter(isRoutedWidgetBinding)
    .filter((binding) => bindingActionLabel(binding));
  if (routed.length === 0) return "";

  const directionLabels = [...new Set(routed
    .filter((binding) => JOYSTICK_DIRECTION_EVENTS.has(String(binding.event || "").trim()))
    .map(bindingActionLabel))];
  const parts = directionLabels.length > 0
    ? [`摇杆：${directionLabels.join("、")}`]
    : [];
  routed
    .filter((binding) => !JOYSTICK_DIRECTION_EVENTS.has(String(binding.event || "").trim()))
    .forEach((binding) => {
      const event = String(binding.event || "").trim();
      const control = COMPACT_CONTROL_LABELS[event] || formatBindingControl(binding);
      parts.push(`${control}：${bindingActionLabel(binding)}`);
    });
  parts.push("退出跟随设备全局设置");
  return parts.join("；");
}

const ROUTED_WIDGET_EVENTS = new Set([
  "screen.region.tap",
  "screen.region.long_press",
  "button.sw1.short_press",
  "button.sw2.short_press",
  "button.sw3.short_press",
  "button.encoder.short_press",
  "button.encoder.long_press",
  "knob.rotate_cw",
  "knob.rotate_ccw",
  "joystick.up",
  "joystick.down",
  "knob.rotate_cw / knob.rotate_ccw",
]);

const COMPONENT_SYSTEM_ACTIONS = new Set([
  "page_toggle",
  "page_enter",
  "page_back",
  "page_main",
  "page_app",
  "component_center",
]);

export function isRoutedWidgetBinding(binding = {}) {
  return (
    ROUTED_WIDGET_EVENTS.has(String(binding.event || "").trim())
    && !COMPONENT_SYSTEM_ACTIONS.has(String(binding.action || "").trim())
  );
}
