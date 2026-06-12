/**
 * [Input] Widget buttons.json binding records.
 * [Output] User-facing physical-control labels that distinguish button location and gesture.
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
