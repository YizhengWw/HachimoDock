/**
 * [Input] Widget buttons.json binding records across screen gestures, P4 switches, and joystick input.
 * [Output] User-facing physical-control labels plus a single allowlist predicate shared by component-center previews and installs.
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

export function isRoutedWidgetBinding(binding = {}) {
  return ROUTED_WIDGET_EVENTS.has(String(binding.event || "").trim());
}
