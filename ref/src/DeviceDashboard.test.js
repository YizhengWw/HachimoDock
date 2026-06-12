/**
 * [Input] DeviceDashboard.jsx orchestrator source + dashboard subcomponent sources.
 * [Output] Static Node coverage that the dashboard composes PageShell + 4 cards in spec order (状态条 / 当前展示 / 按钮配置 / 语音助手折叠), pulls state from useDeviceContext (no local polling), sends the full visible button map through USB OTA with backend-held board ack confirmation and stale-writer reconnect retry, syncs state-specific appearance WAV cues, removes the old runtime/desktop-pet-channel panels, places DashboardActionsMenu in PageShell actions, places BoardButtonPanel inside a plain Card and VoiceAssistantPanel inside Card.Collapsible, keeps board voice action injection status wired, and guards against stale legacy bridge injection.
 * [Pos] test node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));

function readSource(fileName) {
  return readFileSync(join(srcDir, fileName), "utf8");
}

function readRepoFile(...parts) {
  return readFileSync(join(srcDir, "..", ...parts), "utf8");
}

function cssRuleBlock(source, selector) {
  const escapedSelector = selector.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const match = source.match(new RegExp(`${escapedSelector}\\s*\\{([^}]*)\\}`));
  assert.ok(match, `expected CSS rule for ${selector}`);
  return match[1];
}

// ---- KEPT AS-IS: modal footer spacing ----

test("channel switch confirmation owns its modal footer spacing", () => {
  const modal = readSource("ChannelSwitchConfirmModal.jsx");
  const css = readSource("styles.css");

  assert.match(modal, /channel-switch-confirm-modal__actions/);
  assert.match(modal, /跟随主体会从/);
  assert.doesNotMatch(modal, /新渠道/);
  assert.doesNotMatch(modal, /对应形象/);
  assert.doesNotMatch(modal, /一套形象/);
  assert.doesNotMatch(modal, /appearance-channel-modal__actions/);
  assert.match(css, /\.channel-switch-confirm-modal__actions\s*\{[\s\S]*padding:\s*8px 20px 20px;/);
});

// ---- PORTED: channel switch persistence now lives in ChannelMatrixCard ----

test("agent appearance matrix filters installed agents and syncs only the followed agent", () => {
  const source = readSource("dashboard/ChannelMatrixCard.jsx");

  assert.match(source, /agentOptions\.filter\(\(agent\) => agent\.detected\)/);
  assert.match(source, /BUILTIN_TERRIER_APPEARANCE_ID/);
  assert.match(source, /saveAgentAppearance\(agentId, appearance\.id\)/);
  assert.match(source, /agentId === activeAgentId/);
  assert.match(source, /setPendingFollow\(\{ agentId, appearance \}\)/);
  assert.match(source, /applyDesktopPet\(agentId, appearance/);
  assert.doesNotMatch(source, /shouldConfirmChannelSwitch\(/);
  assert.doesNotMatch(source, /pendingActivate/);
});

// ---- PORTED: voice button configuration now in BoardButtonPanel ----

test("voice button configuration exposes direct per-button settings before OTA", () => {
  const source = readSource("dashboard/BoardButtonPanel.jsx");
  const dashboard = readSource("DeviceDashboard.jsx");
  const css = readSource("styles.css");

  assert.match(source, /voice-button-action-list/);
  assert.match(source, /voice-button-action-select/);
  assert.match(source, /BOARD_BUTTON_CONTROL_ROWS\.map/);
  assert.match(source, /BUTTON_FUNCTION_OPTIONS\.filter/);
  assert.match(source, /通过 USB OTA 下发按钮配置/);
  assert.match(source, /需 USB OTA 生效/);
  assert.match(dashboard, /label:\s*"不绑定"[\s\S]*忽略该输入/);

  assert.match(css, /\.voice-config-switch\s*\{/);
  assert.match(css, /\.voice-button-config-section\s*\{/);
  assert.match(css, /\.voice-button-action-list\s*\{/);
  assert.match(css, /\.voice-button-action-select\s*\{/);
  assert.match(css, /\.voice-config-footer\s*\{/);
});

test("front encoder rotation is fixed to volume adjustment in the device UI", () => {
  const dashboard = readSource("DeviceDashboard.jsx");
  const rotateRowMatch = dashboard.match(/id:\s*"encoder_rotate"[\s\S]*?actionOptions:\s*\[([^\]]+)\]/);
  assert.ok(rotateRowMatch, "expected encoder_rotate control row");
  assert.match(rotateRowMatch[1], /"volume_adjust"/);
  assert.doesNotMatch(rotateRowMatch[1], /"system_page"/);
  assert.doesNotMatch(rotateRowMatch[1], /"negative_screen_adjust"/);
  assert.doesNotMatch(rotateRowMatch[1], /"disabled"/);
  assert.match(dashboard, /encoder_rotate:\s*"volume_adjust"/);
  assert.doesNotMatch(dashboard, /encoder_rotate:\s*"negative_screen_adjust"/);
  assert.match(dashboard, /action:\s*row\.actionOptions\.includes\(buttonActions\[row\.id\]\)\s*\?\s*buttonActions\[row\.id\]\s*:\s*row\.defaultAction/);
});

test("negative-screen touch gestures are not exposed as button config rows", () => {
  const dashboard = readSource("DeviceDashboard.jsx");
  const rowsBlock = dashboard.match(/export const BOARD_BUTTON_CONTROL_ROWS = \[([\s\S]*?)\];/);
  assert.ok(rowsBlock, "expected BOARD_BUTTON_CONTROL_ROWS");
  assert.doesNotMatch(rowsBlock[1], /screen_tap/);
  assert.doesNotMatch(rowsBlock[1], /screen_long_press/);
  assert.doesNotMatch(rowsBlock[1], /screen\.region\.tap/);
  assert.doesNotMatch(rowsBlock[1], /screen\.region\.long_press/);
  assert.doesNotMatch(dashboard, /screen_tap:\s*"negative_screen_primary"/);
  assert.doesNotMatch(dashboard, /screen_long_press:\s*"negative_screen_secondary"/);
});

// ---- PORTED: board button map now in BoardButtonPanel ----

test("button configuration shows a board button map with current assignments", () => {
  const source = readSource("dashboard/BoardButtonPanel.jsx");
  const css = readSource("styles.css");

  // Constants and board-runtime source still live in the orchestrator or are imported.
  const orchestrator = readSource("DeviceDashboard.jsx");
  assert.match(orchestrator, /BOARD_BUTTON_CONTROL_ROWS/);

  // Panel-level callout labels.
  assert.match(source, /board-button-panel__callout-label/);

  assert.match(css, /\.board-button-map__screen\s*\{[\s\S]*fill:\s*#050609;/);
  assert.doesNotMatch(css, /board-button-map__pet/);
  assert.doesNotMatch(css, /board-button-map__pet-ear/);
  assert.doesNotMatch(css, /board-button-map__pet-shadow/);
  assert.doesNotMatch(css, /board-button-map__pet-face/);
  assert.doesNotMatch(css, /board-button-map__screen-bubble/);
});

// ---- PORTED: audio bridge toggle still in orchestrator; panel slice in VoiceAssistantPanel ----

test("board audio enable starts local runtimes and targets the active board", () => {
  const source = readSource("DeviceDashboard.jsx");

  assert.match(source, /const targetBoardDeviceId = onlineBoardDeviceId \|\| usb\.boardDeviceId \|\| binding\.boardDeviceId;/);
  assert.match(source, /if \(action === "start"\) \{[\s\S]*invoke\("ensure_bridge_runtime"/);
  assert.match(source, /if \(action === "start"\) \{[\s\S]*invoke\("ensure_voice_runtime"/);
  assert.match(source, /boardDeviceId: targetBoardDeviceId/);
  assert.match(source, /\[binding\.boardDeviceId, onlineBoardDeviceId, usb\.boardDeviceId, voiceConfig\.trigger\]/);

  const panelSource = readSource("dashboard/VoiceAssistantPanel.jsx");
  assert.match(panelSource, /audioBlockingReason/);
  assert.match(panelSource, /启动前会自动检查本地 Bridge 和 voice-service/);
  assert.match(panelSource, /disabled=\{[\s\S]*?audioBridgePending[\s\S]*?audioBlockingReason[\s\S]*?voiceConfig\.enabled/);
  assert.doesNotMatch(panelSource, /disabled=\{state\.audioBridgePending \|\| !!blockingReason\}/);
});

test("board voice action injection status is surfaced in the voice panel", () => {
  const source = readSource("DeviceDashboard.jsx");
  const panelSource = readSource("dashboard/VoiceAssistantPanel.jsx");
  const rust = readRepoFile("src-tauri", "src", "lib.rs");

  assert.match(source, /postMockButtonInject/);
  assert.match(source, /\/mock-button-inject/);
  assert.match(source, /http_request_text/);
  assert.match(source, /listen\("usb-message"/);
  assert.match(source, /listen\("usb-input-action-result"/);
  assert.match(source, /deviceVoiceFlow/);
  assert.match(source, /payload\.transient === true/);
  assert.match(source, /payload\.pending === true/);
  assert.match(source, /phase:\s*"waiting_reply"/);
  assert.match(source, /桥接连接瞬时抖动/);
  assert.match(source, /sendMockButtonInject/);
  assert.match(source, /tokenPreview/);
  assert.match(source, /replyPreview/);
  assert.match(rust, /"tokenPreview": reply_preview/);
  assert.match(rust, /"replyPreview": reply_preview/);
  assert.match(rust, /"pending": true/);
  assert.match(rust, /等待模型回复/);
  assert.match(source, /agentId: selectedAgentId/);
  assert.match(source, /sessionId: voiceState\.busSessionId \|\| "auto"/);

  assert.match(panelSource, /测试输入（模拟按钮语音转文字）/);
  assert.match(panelSource, /发送到当前会话/);
  assert.match(panelSource, /模型回复预览/);
  assert.match(panelSource, /设备语音状态/);
  assert.match(panelSource, /设备识别文本/);
});

test("voice session list waits for bridge readiness and refreshes after packaged cold start", () => {
  const source = readSource("DeviceDashboard.jsx");

  assert.match(source, /selectedBusAgentReady/);
  assert.match(source, /if \(!selectedBusAgentReady\) return undefined;/);
  assert.match(source, /fetchBusSessions\(selectedAgentId, ctl\.signal\)/);
  assert.match(source, /setInterval\(run, 5000\)/);
  assert.match(source, /\[selectedAgentId, selectedBusAgentReady\]/);
});

test("voice action injection avoids stale legacy bridge runtimes", () => {
  const source = readSource("DeviceDashboard.jsx");
  const rust = readRepoFile("src-tauri", "src", "lib.rs");

  assert.match(source, /ports\.push\(BRIDGE_HTTP_PRIMARY_PORT\)/);
  assert.doesNotMatch(source, /BRIDGE_HTTP_LEGACY_PORT/);
  assert.doesNotMatch(source, /ports\.push\(BRIDGE_HTTP_PRIMARY_PORT,\s*BRIDGE_HTTP_LEGACY_PORT\)/);

  assert.match(rust, /fn stop_legacy_bridge_runtime\(\)/);
  assert.match(rust, /fn stop_bridge_launch_agent\(/);
  assert.match(rust, /stop_bridge_launch_agent\(&runtime_paths\);/);
  assert.match(rust, /stop_legacy_bridge_runtime\(\);/);
  assert.match(rust, /launchctl/);
  assert.match(rust, /let ports = \[DEFAULT_BRIDGE_PORT\];/);
  assert.doesNotMatch(rust, /let ports = \[DEFAULT_BRIDGE_PORT,\s*LEGACY_BRIDGE_PORT\];/);
});

test("wireless channel switching falls back to MQTT when USB write is stale", () => {
  const rust = readRepoFile("src-tauri", "src", "lib.rs");
  const match = rust.match(
    /async fn dispatch_remote_cli_binding[\s\S]*?\/\/ ── Desktop device ID ──/,
  );
  assert.ok(match, "expected dispatch_remote_cli_binding block");
  const dispatch = match[0];

  assert.match(dispatch, /let mut usb_error: Option<String> = None;/);
  assert.doesNotMatch(dispatch, /usb_manager\.send_state\(&previous_source, &disabled_payload\)\?;/);
  assert.doesNotMatch(dispatch, /usb_manager\.send\("control\/remote-cli-binding", &payload\)\?;/);
  assert.match(dispatch, /let dispatch_error = mqtt_error\.clone\(\)\.or_else\(\|\| usb_error\.clone\(\)\);/);
});

// ---- PORTED: JS-side in orchestrator; panel-side in BoardButtonPanel; Rust stays ----

test("board button config sends the full visible button map over USB OTA", () => {
  const source = readSource("DeviceDashboard.jsx");
  const rust = readRepoFile("src-tauri", "src", "lib.rs");

  assert.match(source, /VOICE_BUTTON_OPTIONS/);
  assert.match(source, /trigger:\s*"encoder_button\.hold"/);
  assert.doesNotMatch(source, /top_button\.hold/);
  assert.match(source, /voiceConfig/);
  assert.match(source, /onApplyVoiceConfig/);
  assert.match(source, /VOICE_CONFIG_STORAGE_KEY/);
  assert.match(source, /buildBoardButtonConfigBindings/);
  // button_config_signal invoke now lives in the shared dispatchBoardButtonConfig
  // helper, reused by both the manual OTA button and the component-center install.
  assert.match(source, /export async function dispatchBoardButtonConfig/);
  assert.match(source, /invoke\("button_config_signal"/);
  assert.match(source, /bindings: buildBoardButtonConfigBindings\(buttonActions\)/);
  assert.match(source, /const ack = await dispatchBoardButtonConfig\(/);
  assert.match(source, /voiceEnabled: voiceConfig\.enabled/);
  assert.match(source, /requestId/);
  assert.match(source, /ack\?\.ack\?\.bindingCount/);
  assert.match(source, /按钮配置已写入板端/);
  assert.doesNotMatch(source, /waitForButtonConfigAck/);
  assert.doesNotMatch(source, /ackWaiter/);
  assert.doesNotMatch(source, /await ackWaiter\.ready/);
  assert.doesNotMatch(source, /完整按钮配置已通过 USB OTA 下发到板端/);

  const panelSource = readSource("dashboard/BoardButtonPanel.jsx");
  assert.match(panelSource, /按钮功能/);
  assert.match(panelSource, /需 USB OTA 生效/);
  assert.match(panelSource, /通过 USB OTA 下发按钮配置/);

  assert.match(rust, /struct ButtonConfigBinding/);
  assert.match(rust, /fn button_config_signal/);
  assert.match(rust, /"button_config"/);
  assert.match(rust, /"request_id"/);
  assert.match(rust, /"bindings"/);
  assert.match(rust, /usb_manager\.send\("control\/command"/);
  assert.match(rust, /"usbSent"/);
  assert.match(rust, /BUTTON_CONFIG_ACK_WAITERS/);
  assert.match(rust, /"button-config-ack"/);
  assert.match(rust, /recv_timeout\(Duration::from_secs\(BUTTON_CONFIG_ACK_TIMEOUT_SECS\)\)/);
  assert.match(rust, /reconnect_usb_serial_for_command/);
  assert.match(rust, /send_button_config_and_wait_for_ack/);
  assert.match(rust, /USB 重新连接失败：未找到可用串口/);
  assert.match(rust, /"bindingCount"/);
  assert.match(rust, /"ack"/);
});

test("widget install no longer owns board button presets", () => {
  const source = readSource("DeviceDashboard.jsx");

  assert.doesNotMatch(source, /WIDGET_RECOMMENDED_BUTTON_ACTIONS/);
  assert.doesNotMatch(source, /applyRecommendedButtonConfigForWidget/);
  assert.doesNotMatch(source, /top_button:\s*"negative_screen_primary"/);

  // The panel re-reads the config when another surface writes it.
  assert.match(source, /window\.addEventListener\("storage", onStorage\)/);
  assert.match(source, /setVoiceConfig\(loadVoiceConfigFromStorage\(\)\)/);
});

test("appearance OTA syncs done and error WAV cues beside the matching videos", () => {
  const rust = readRepoFile("src-tauri", "src", "usb_serial.rs");
  const tauri = readRepoFile("src-tauri", "src", "lib.rs");

  assert.match(rust, /audioPath/);
  assert.match(rust, /format!\("videos\/\{\}\.wav", family_name\)/);
  assert.match(rust, /打开音效文件失败/);
  assert.match(rust, /const APPEARANCE_ASSET_CHUNK_SIZE/);
  assert.match(rust, /fn appearance_asset_chunk_delay/);
  assert.match(rust, /APPEARANCE_ASSET_SERIAL_BYTES_PER_SEC/);
  assert.doesNotMatch(rust, /Duration::from_millis\(3\)/);
  assert.match(tauri, /fn default_appearance_audio_cue_name\(family: &str\)/);
  assert.match(tauri, /"waiting_user" => Some\("waiting_user\.wav"\)/);
  assert.match(tauri, /audio-overrides\.json/);
  assert.match(tauri, /family_entry\["audioPath"\]\s*=\s*serde_json::json!\(format!\([\s\S]*custom-appearances\/builtin-terrier\/videos\/\{\}[\s\S]*audio_name[\s\S]*\)\);/);
});

// ---- NEW: 4-section IA tests (added in Step 1) ----

test("dashboard composes PageShell + 4 cards in the spec order (区 1 → 区 4)", () => {
  const source = readSource("DeviceDashboard.jsx");

  // Imports shell + dashboard children.
  assert.match(source, /from\s+"\.\/shell\/DeviceContext\.jsx"/);
  assert.match(source, /from\s+"\.\/shell\/ToastStack\.jsx"/);
  assert.match(source, /PageShell/);
  assert.match(source, /Card/);
  assert.match(source, /Card\.Collapsible/);
  assert.match(source, /DeviceStatusBar/);
  assert.match(source, /ChannelMatrixCard/);
  assert.match(source, /BoardButtonPanel/);
  assert.match(source, /VoiceAssistantPanel/);
  assert.match(source, /DashboardActionsMenu/);

  // The render returns a PageShell, in spec order.
  const idxStatusBar = source.indexOf("<DeviceStatusBar");
  const idxCurrent = source.indexOf("<ChannelMatrixCard");
  const idxButtons = source.indexOf("<BoardButtonPanel");
  const idxVoice = source.indexOf("<VoiceAssistantPanel");
  assert.ok(idxStatusBar !== -1 && idxCurrent !== -1 && idxButtons !== -1 && idxVoice !== -1);
  assert.ok(idxStatusBar < idxCurrent, "区 1 before 区 2");
  assert.ok(idxCurrent < idxButtons, "区 2 before 区 3");
  assert.ok(idxButtons < idxVoice, "区 3 before 区 4");
});

test("dashboard pulls device state from useDeviceContext (no local polling)", () => {
  const source = readSource("DeviceDashboard.jsx");

  assert.match(source, /useDeviceContext\(/);
  assert.match(source, /useToast\(/);
  // The old local polling effects are gone.
  assert.doesNotMatch(source, /invoke\("usb_get_status"\)/);
  assert.doesNotMatch(source, /invoke\("check_device_availability"\)/);
  assert.doesNotMatch(source, /invoke\("detect_local_agents"\)/);
  assert.doesNotMatch(source, /invoke\("load_bridge_profile"\)/);
  assert.doesNotMatch(source, /listAppearances\(\)/);
});

test("dashboard deletes the old runtime/Bridge card and inline panels", () => {
  const source = readSource("DeviceDashboard.jsx");

  assert.doesNotMatch(source, /function DesktopPetAssignmentPanel/);
  assert.doesNotMatch(source, /function VoiceAssistantPanel/);
  assert.doesNotMatch(source, /function BoardButtonConfigPanel/);
  assert.doesNotMatch(source, /function BoardButtonMap/);
  assert.doesNotMatch(source, /function AgentAppearancePickerModal/);
  assert.doesNotMatch(source, /dashboard-runtime-card/);
  assert.doesNotMatch(source, /运行状态/);
  assert.doesNotMatch(source, /desktop-pet-channel-card/);
});

test("dashboard places the actions menu inside PageShell actions slot", () => {
  const source = readSource("DeviceDashboard.jsx");

  assert.match(source, /actions=\{[\s\S]*?<DashboardActionsMenu/);
});

test("dashboard puts BoardButtonPanel inside a plain Card (always visible) and VoiceAssistantPanel inside Card.Collapsible", () => {
  const source = readSource("DeviceDashboard.jsx");

  const buttonsIdx = source.indexOf("<BoardButtonPanel");
  const cardBeforeButtons = source.lastIndexOf("<Card", buttonsIdx);
  const collapsibleBeforeButtons = source.lastIndexOf("<Card.Collapsible", buttonsIdx);
  assert.ok(cardBeforeButtons > collapsibleBeforeButtons, "BoardButtonPanel must live in a plain Card");

  const voiceIdx = source.indexOf("<VoiceAssistantPanel");
  const collapsibleBeforeVoice = source.lastIndexOf("<Card.Collapsible", voiceIdx);
  assert.ok(collapsibleBeforeVoice !== -1 && collapsibleBeforeVoice < voiceIdx, "VoiceAssistantPanel must live in Card.Collapsible");
});

test("DeviceDashboard wires WifiApplyModal and passes onApplyWifi only when USB is connected", () => {
  const source = readSource("DeviceDashboard.jsx");
  assert.match(source, /WifiApplyModal/);
  assert.match(source, /import\s+WifiApplyModal\s+from\s+["']\.\/dashboard\/WifiApplyModal["']/);
  /* Only expose the menu item when the device is reachable over USB. */
  assert.match(source, /onApplyWifi[\s\S]{0,80}(usbConnected|usb\.connected|transport)/);
});
