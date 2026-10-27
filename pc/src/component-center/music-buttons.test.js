import test from "node:test";
import assert from "node:assert/strict";
import { COMPONENT_CONTROL_OPTIONS, componentControlOptionAllowed, defaultControlLabelForBinding, optionForControlLabel } from "./button-config.js";
// The music-player package is retired and excluded from public exports. Keep
// its binding compatibility covered without adding it back to the builtins.
const bindings = [
  { action: "media.toggle", control: "SW1", event: "button.sw1.short_press", label: "播放或暂停" },
  { action: "media.lyrics", control: "SW1", event: "button.sw1.long_press", label: "歌词模式或返回" },
];
test("legacy music bindings retain distinct SW1 short and long press through PC label round-trip", () => {
  const events = bindings.map(binding => optionForControlLabel(defaultControlLabelForBinding(binding)).event);
  assert.equal(new Set(events).size, bindings.length);
  assert.equal(events[bindings.findIndex(b => b.action === "media.lyrics")], "button.sw1.long_press");
  assert.equal(events[bindings.findIndex(b => b.action === "media.toggle")], "button.sw1.short_press");
});
test("SW1 long press is offered only for the controlled media lyric action", () => {
  const option = COMPONENT_CONTROL_OPTIONS.find(o => o.event === "button.sw1.long_press");
  assert.equal(componentControlOptionAllowed(option, { action: "media.lyrics" }, { mediaSource: "audio.player" }), true);
  assert.equal(componentControlOptionAllowed(option, { action: "media.toggle" }, { id: "music-player" }), false);
  assert.equal(componentControlOptionAllowed(option, { action: "media.lyrics" }, { id: "other" }), false);
});
