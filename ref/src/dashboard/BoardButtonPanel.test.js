/**
 * [Input] Read BoardButtonPanel.jsx source.
 * [Output] Static Node coverage that the SVG uses a larger non-overlapping callout map, renders each current action label, the compact button editor persists, hover row-highlight wiring exists, voice_ptt row carries the voice-enabled chip, and USB OTA dispatch wiring stays intact.
 * [Pos] test node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "BoardButtonPanel.jsx"), "utf8");

test("BoardButtonPanel exports a default React component", () => {
  assert.match(source, /export default function BoardButtonPanel\s*\(/);
});

test("BoardButtonPanel renders the SVG board map with viewBox preserved", () => {
  assert.match(source, /viewBox="0 0 560 320"/);
  assert.match(source, /board-button-map__device/);
});

test("Each hardware callout uses split label lines so long Chinese action text does not collide", () => {
  assert.match(source, /renderCalloutLabel/);
  assert.match(source, /board-button-panel__callout-label/);
  assert.match(source, /board-button-panel__callout-label-name/);
  assert.match(source, /board-button-panel__callout-label-action/);
  // The set of callout-labelled controls is hardware-only; negative-screen
  // touch gestures are no longer user-configurable here.
  assert.doesNotMatch(source, /top_button/);
  assert.match(source, /encoder_button_short/);
  assert.match(source, /encoder_button/);
  assert.match(source, /encoder_rotate/);
  assert.doesNotMatch(source, /screen_tap/);
  assert.doesNotMatch(source, /screen_long_press/);
});

test("BoardButtonPanel uses __left/__right two-column wrapper layout", () => {
  assert.match(source, /board-button-panel__left/);
  assert.match(source, /board-button-panel__right/);
});

test("OTA hint banner renders under the SVG using board-button-panel__hint class", () => {
  assert.match(source, /board-button-panel__hint/);
  assert.match(source, /voiceConfigOtaState\?\.message/);
  assert.doesNotMatch(source, /message-banner voice-config-message/);
});

test("BoardButtonPanel gives the SVG more room and keeps settings compact", () => {
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");
  assert.match(css, /\.board-button-panel__left\s*{/);
  assert.match(css, /\.board-button-panel__right\s*{/);
  assert.match(css, /\.board-button-panel__svg\s*{[\s\S]*max-width:\s*680px/);
  assert.match(css, /\.voice-button-action-list\s*{[\s\S]*gap:\s*8px/);
  assert.match(css, /\.voice-button-action-row\s*{[\s\S]*padding:\s*8px 10px/);
});

test("OTA hint CSS uses board-button-panel__hint variants", () => {
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");
  assert.match(css, /\.board-button-panel__hint\s*{/);
  assert.match(css, /\.board-button-panel__hint--warning\s*{/);
  assert.match(css, /\.board-button-panel__hint--error\s*{/);
  assert.match(css, /\.board-button-panel__hint--success\s*{/);
  assert.match(css, /\.board-button-panel__hint--info\s*{/);
});

test("Hover effects respect prefers-reduced-motion", () => {
  // CSS-side: transitions must be killed under reduce-motion preference.
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");
  assert.match(css, /@media\s*\(prefers-reduced-motion:\s*reduce\)/);
  assert.match(css, /board-button-panel__callout-label[\s\S]*transition:\s*none/);
});

test("Hovering an SVG callout highlights the matching editor row", () => {
  // Implementation hook: a state `hoveredButtonId` plus pointer events on the SVG button shapes,
  // plus an `is-hovered` class on the matching row.
  assert.match(source, /hoveredButtonId|setHoveredButton/);
  assert.match(source, /is-hovered/);
});

test("The editable rows persist and use BOARD_BUTTON_CONTROL_ROWS + BUTTON_FUNCTION_OPTIONS", () => {
  assert.match(source, /BOARD_BUTTON_CONTROL_ROWS\.map/);
  assert.match(source, /BUTTON_FUNCTION_OPTIONS\.filter/);
  assert.match(source, /voice-button-action-list/);
  assert.match(source, /voice-button-action-select/);
});

test("Rows with a single allowed action render as fixed values instead of selects", () => {
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");
  assert.match(source, /allowedOptions\.length <= 1/);
  assert.match(source, /voice-button-action-fixed/);
  assert.match(css, /\.voice-button-action-fixed\s*\{/);
});

test("Voice_ptt rows display a chip reflecting voice-enabled state", () => {
  assert.match(source, /语音助手已开启|语音已开启/);
  assert.match(source, /未开启/);
  // chip rendering must be conditional on the row's action being voice_ptt
  assert.match(source, /voice_ptt/);
});

test("USB OTA dispatch wiring stays — calls onApplyVoiceConfig and shows the button", () => {
  assert.match(source, /onApplyVoiceConfig/);
  assert.match(source, /通过 USB OTA 下发按钮配置/);
  assert.match(source, /需 USB OTA 生效/);
});

test("BoardButtonPanel is always visible (no Card.Collapsible wrapper in this file)", () => {
  // The panel itself does not collapse — the parent DeviceDashboard places it in a plain Card.
  assert.doesNotMatch(source, /Card\.Collapsible/);
});
