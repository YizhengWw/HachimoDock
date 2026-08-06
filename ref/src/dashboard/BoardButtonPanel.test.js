/**
 * [Input] Read BoardButtonPanel.jsx source.
 * [Output] Static coverage for the image-first hardware workspace, all-visible
 *          ten-gesture P4 groups, physical order, shared voice/prompt switches,
 *          bounded prompts, PTT state, and USB apply wiring.
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
  assert.match(source, /viewBox=\{isP4Runtime \? "0 0 456 320" : "0 0 560 320"\}/);
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

test("BoardButtonPanel uses an overview/editor workspace", () => {
  assert.match(source, /board-button-panel__workspace/);
  assert.match(source, /board-button-panel__left/);
  assert.match(source, /board-button-panel__right/);
  assert.match(source, /board-button-panel__device-stage/);
  assert.match(source, /board-button-panel__workspace--p4/);
});

test("OTA hint banner renders between sync toolbar and workspace", () => {
  assert.match(source, /board-button-panel__hint/);
  assert.match(source, /voiceConfigOtaState\?\.message/);
  assert.match(source, /aria-live="polite"/);
  assert.doesNotMatch(source, /message-banner voice-config-message/);
});

test("Button configuration header is one compact toolbar instead of a duplicated Card header", () => {
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");
  const dashboard = readFileSync(join(here, "..", "DeviceDashboard.jsx"), "utf8");
  assert.match(source, /<h2>按钮配置<\/h2>/);
  assert.match(source, /\{controlGroups\.length\} 个控件/);
  assert.match(source, /voiceConfigDirty \? "待同步" : "已同步"/);
  assert.doesNotMatch(dashboard, /<Card title="按钮配置"/);
  assert.match(css, /\.board-button-panel__toolbar\s*{[\s\S]*min-height:\s*38px[\s\S]*padding:\s*0 0 var\(--space-2\)/);
  assert.match(css, /\.board-button-panel__voice-toggle\s*{[\s\S]*min-height:\s*34px[\s\S]*padding:\s*3px 6px/);
  assert.match(css, /\.voice-config-apply-btn\s*{[\s\S]*min-height:\s*34px[\s\S]*padding:\s*0 12px/);
  assert.match(css, /\.board-button-panel__hint\s*{[\s\S]*min-height:\s*26px[\s\S]*padding:\s*4px 8px/);
});

test("BoardButtonPanel places four physical-order control cards below the device stage", () => {
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");
  assert.match(css, /\.board-button-panel__workspace\s*{[\s\S]*grid-template-columns:\s*minmax\(320px,\s*0\.9fr\)\s*minmax\(0,\s*1\.1fr\)/);
  assert.match(css, /\.board-button-panel__workspace--p4\s*{[\s\S]*grid-template-columns:\s*minmax\(0,\s*1fr\)/);
  assert.match(css, /\.board-button-panel__left\s*{/);
  assert.match(css, /\.board-button-panel__right\s*{/);
  assert.match(css, /\.board-button-panel__svg\s*{[\s\S]*max-width:\s*400px/);
  assert.match(css, /\.board-button-control-groups\s*{[\s\S]*grid-template-columns:\s*repeat\(4/);
  assert.match(css, /\.board-button-control-group\s*{[\s\S]*border:\s*1px solid var\(--line-soft\)/);
  assert.match(css, /\.board-button-control-group__visual\s*{[\s\S]*grid-template-columns:\s*108px/);
  assert.match(css, /\.board-button-control-field\s*{[\s\S]*min-height:\s*80px[\s\S]*padding:\s*10px/);
  assert.match(css, /@media\s*\(max-width:\s*1120px\)[\s\S]*board-button-control-groups[\s\S]*repeat\(2/);
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

test("The editable rows come from the connected runtime and keep the shared action catalog", () => {
  assert.match(source, /buttonControlRowsForRuntime\(runtime\)/);
  assert.match(source, /groupControlRows\(controlRows, isP4Runtime\)/);
  assert.match(source, /controlGroups\.map/);
  assert.match(source, /group\.rows\.map/);
  assert.match(source, /BUTTON_FUNCTION_OPTIONS\.filter/);
  assert.match(source, /board-button-control-groups/);
  assert.match(source, /voice-button-action-select/);
});

test("Every physical control and all twelve gestures stay visible at once", () => {
  assert.match(source, /controlGroups\.map/);
  assert.match(source, /group\.rows\.map/);
  assert.match(source, /<fieldset/);
  assert.match(source, /aria-label="实体控件与手势配置"/);
  assert.doesNotMatch(source, /board-button-panel__hardware-note/);
  assert.doesNotMatch(source, /按键只响应短按/);
  assert.doesNotMatch(source, /全部手势保持平铺/);
  assert.doesNotMatch(source, /board-button-panel__editor-header/);
  assert.doesNotMatch(source, /role="tab"/);
  assert.doesNotMatch(source, /role="tabpanel"/);
});

test("Every gesture can reveal its own bounded Code Agent prompt directly below its action", () => {
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");
  const dashboard = readFileSync(join(here, "..", "DeviceDashboard.jsx"), "utf8");
  assert.match(dashboard, /const P4_CUSTOM_ACTION_OPTIONS = \[[\s\S]*"agent_prompt"/);
  assert.match(source, /currentActionId === "agent_prompt"/);
  assert.match(source, /board-button-control-field__prompt/);
  assert.match(source, /<textarea/);
  assert.match(source, /按键后直接发送给 Code Agent/);
  assert.match(source, /maxLength=\{120\}/);
  assert.match(source, /placeholder="例如：总结当前进度，检查报错并继续实现。"/);
  assert.match(source, /buttonValues\[row\.id\]/);
  assert.match(source, /\/120/);
  assert.doesNotMatch(source, /promptEditorsOpen/);
  assert.doesNotMatch(source, /board-button-panel__prompt-editors/);
  assert.match(css, /\.board-button-control-field__prompt\s*{/);
  assert.match(css, /\.board-button-control-field__prompt textarea\.voice-button-action-value\s*{[\s\S]*min-height:\s*72px/);
});

test("The sync action uses the shared Button with stable loading semantics", () => {
  assert.match(source, /import Button from "\.\.\/shell\/Button"/);
  assert.match(source, /variant="primary"/);
  assert.match(source, /loading=\{voiceConfigOtaState\?\.pending\}/);
  assert.match(source, /loadingLabel="正在同步…"/);
});

test("An unverified runtime does not fall back to the Linux hardware illustration", () => {
  const dashboard = readFileSync(join(here, "..", "DeviceDashboard.jsx"), "utf8");
  assert.match(dashboard, /if \(runtimeId === P4_RUNTIME_ID\) return P4_BUTTON_CONTROL_ROWS/);
  assert.match(dashboard, /return \[\]/);
  assert.match(source, /controlRows\.length === 0/);
  assert.match(source, /等待设备协议握手/);
  assert.match(source, /board-button-panel__unverified/);
});

test("P4 map shows three hardware keys plus the joystick and every gesture row", () => {
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");
  const dashboard = readFileSync(join(here, "..", "DeviceDashboard.jsx"), "utf8");
  assert.match(source, /p4_sw1_short/);
  assert.match(source, /p4_sw2_short/);
  assert.match(source, /p4_sw3_short/);
  assert.match(dashboard, /p4_sw1_long/);
  assert.match(dashboard, /p4_sw2_long/);
  assert.match(dashboard, /p4_sw3_long/);
  assert.match(source, /p4_joystick/);
  assert.match(dashboard, /p4_encoder_press/);
  assert.match(dashboard, /p4_encoder_long/);
  assert.match(dashboard, /p4_encoder_ccw/);
  assert.match(dashboard, /p4_encoder_cw/);
  assert.match(dashboard, /p4_joystick_up/);
  assert.match(dashboard, /p4_joystick_down/);
  assert.match(source, /voice-button-action-value/);
  assert.match(css, /\.board-button-map__hardware-key\s*\{/);
  assert.match(css, /\.voice-button-action-value\s*\{/);
});

test("Each physical group owns an accessible standalone SVG control illustration", () => {
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");
  assert.match(source, /function ControlGlyph/);
  assert.match(source, /role="img"/);
  assert.match(source, /aria-label=\{`\$\{label\} 实体控件示意`\}/);
  assert.match(source, /controlId=\{group\.id\}/);
  assert.match(source, /board-button-control-glyph__key/);
  assert.match(source, /board-button-control-glyph__encoder/);
  assert.match(css, /\.board-button-control-glyph\s*{/);
  assert.match(css, /\.board-button-control-glyph__key\s*{/);
  assert.match(css, /\.board-button-control-glyph__encoder\s*{/);
});

test("Session switching stays in the hardware action dropdown", () => {
  assert.doesNotMatch(source, /目标会话/);
  assert.doesNotMatch(source, /p4-session-config/);
  assert.doesNotMatch(source, /onSessionChange/);
  assert.doesNotMatch(source, /formatVoiceSessionOption/);
});

test("Removed negative-screen and legacy actions stay out of the P4 hardware dropdown", () => {
  const dashboard = readFileSync(join(here, "..", "DeviceDashboard.jsx"), "utf8");
  const options = dashboard.match(/const P4_CUSTOM_ACTION_OPTIONS = \[([\s\S]*?)\];/);
  assert.ok(options);
  assert.doesNotMatch(options[1], /agent_enter|miniapp_screen_tap|miniapp_screen_long_press|miniapp_action|page_toggle/);
});

test("Short presses show voice input as a disabled long-press-only option", () => {
  assert.match(source, /const isShortPress = isP4Runtime && row\.event\.endsWith\("\.short_press"\)/);
  assert.match(source, /isShortPress && option\.id === "voice_ptt"/);
  assert.match(source, /requiresLongPress\s*\?\s*"需要长按"/);
  assert.match(source, /disabled=\{Boolean\(disabledReason\)\}/);
});

test("Every action remains available on multiple gestures", () => {
  assert.doesNotMatch(source, /findButtonActionOwner/);
  assert.doesNotMatch(source, /已绑定：/);
  assert.match(source, /const disabledReason = requiresLongPress/);
  assert.match(source, /title=\{disabledReason \|\| undefined\}/);
});

test("Component-owned actions are not rendered as global hardware actions", () => {
  assert.match(source, /buttonLabels = \{\}/);
  assert.doesNotMatch(source, /currentActionId === "miniapp_action"/);
  assert.doesNotMatch(source, /voice-button-action-value--readonly/);
});

test("P4 map matches the physical left-to-right SW1, SW2, SW3, joystick layout", () => {
  assert.match(source, /board-button-map__body--p4/);
  assert.match(source, /board-button-map__control-deck/);
  assert.match(source, /\["p4_sw1", "p4_sw1_short", 84, "SW1"\]/);
  assert.match(source, /\["p4_sw2", "p4_sw2_short", 150, "SW2"\]/);
  assert.match(source, /\["p4_sw3", "p4_sw3_short", 216, "SW3"\]/);
  assert.match(source, /board-button-map__encoder--p4[\s\S]*cx="328" cy="242"/);
  assert.match(source, /x=\{x\} y="218" width="48" height="48" rx="12"/);
  assert.match(source, /board-button-map__encoder-label/);
  assert.match(source, /p4_joystick/);
  assert.match(source, /!isP4Runtime && controlRows\.map/);
  assert.match(source, /row\.controlId && hoveredButtonId === row\.controlId/);
});

test("Rows with a single allowed action render as fixed values instead of selects", () => {
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");
  assert.match(source, /allowedOptions\.length <= 1/);
  assert.match(source, /voice-button-action-fixed/);
  assert.match(css, /\.voice-button-action-fixed\s*\{/);
});

test("Voice_ptt rows display a chip reflecting voice-enabled state", () => {
  assert.match(source, /voiceConfig\.enabled \? "语音输入已开启"/);
  assert.match(source, /未开启/);
  // chip rendering must be conditional on the row's action being voice_ptt
  assert.match(source, /voice_ptt/);
});

test("Button configuration shows voice state while Voice Assistant owns enablement", () => {
  const assistant = readFileSync(join(here, "VoiceAssistantPanel.jsx"), "utf8");
  assert.doesNotMatch(source, /board-button-panel__voice-toggle/);
  assert.match(source, /board-button-panel__voice-chip/);
  assert.match(assistant, /onVoiceEnabledChange/);
  assert.match(assistant, /ariaLabel="启用按键语音"/);
});

test("USB OTA dispatch wiring stays — calls onApplyVoiceConfig and shows the button", () => {
  assert.match(source, /onApplyVoiceConfig/);
  assert.match(source, /同步到设备/);
  assert.match(source, /待同步/);
  assert.match(source, /已同步/);
});

test("BoardButtonPanel is always visible (no Card.Collapsible wrapper in this file)", () => {
  // The panel itself does not collapse — the parent DeviceDashboard places it in a plain Card.
  assert.doesNotMatch(source, /Card\.Collapsible/);
});
