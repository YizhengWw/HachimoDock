/**
 * [Input] DeviceDashboard.jsx orchestrator, pure P4 Session service, and dashboard subcomponent sources.
 * [Output] Static Node coverage for dashboard layout/configuration, Agent-isolated
 * strict active-only P4 conversation sizing with 60-second terminal retention,
 * two-page joystick routing, cursor lifecycle delivery, serialized USB follow switching, Codex-visible and
 * MiMoCode current-caret voice delivery, client-authoritative ACK-gated board
 * configuration, ID-deeplink-confirmed macOS Session recovery, and stale bridge/USB
 * guards, SW1-confirm/SW3-back defaults and migration,
 * immediate saved-ASR voice rearming, and exact-board appearance recovery.
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

test("agent appearance matrix keeps unavailable channels visible and syncs only the followed agent", () => {
  const source = readSource("dashboard/ChannelMatrixCard.jsx");

  assert.match(source, /visibleAgents/);
  assert.match(source, /is-undetected/);
  assert.match(source, /未检测到 CLI/);
  assert.match(source, /BUILTIN_TERRIER_APPEARANCE_ID/);
  assert.match(source, /saveAgentAppearance\(agentId, appearance\.id\)/);
  assert.match(source, /agentId === activeAgentId/);
  assert.match(source, /setPendingFollow\(\{ agentId, appearance \}\)/);
  assert.match(source, /applyDesktopPet\(agentId, appearance/);
  assert.doesNotMatch(source, /shouldConfirmChannelSwitch\(/);
  assert.doesNotMatch(source, /pendingActivate/);
});

test("Agent and appearance card exposes manual scan status and action", () => {
  const source = readSource("DeviceDashboard.jsx");

  assert.match(source, /agentScanSummary/);
  assert.match(source, /refreshAgents/);
  assert.match(source, /handleRefreshAgents/);
  assert.match(source, /aria-label="重新扫描本机 CLI Agent"/);
  assert.match(source, /loading=\{agentScan\?\.pending === true\}/);
  assert.match(source, /重新扫描/);
});

test("global P4 session visibility follows adjacent Agent snapshots", () => {
  const source = readSource("DeviceDashboard.jsx");
  const matrix = readSource("dashboard/ChannelMatrixCard.jsx");
  const sessionFeed = readSource("dashboard/useAgentSessionFeed.js");
  const sessionSync = readSource("dashboard/useP4SessionSync.js");
  const sessionDisplay = readSource("lib/session-display.js");
  const sessionService = readSource("lib/p4-session-service.js");

  assert.match(source, /loadSessionDisplayEnabled/);
  assert.match(source, /saveSessionDisplayEnabled/);
  assert.doesNotMatch(source, /loadSessionDisplayCount/);
  assert.doesNotMatch(source, /saveSessionDisplayCount/);
  assert.doesNotMatch(source, /reconcileDeviceSessionQueue\(/);
  assert.match(sessionService, /reconcileDeviceSessionQueue\(/);
  assert.doesNotMatch(source, /p4SessionDisplayLimit/);
  assert.doesNotMatch(source, /selectedAgentId === "codex"\s*\?\s*\(sessionDisplayEnabled/);
  assert.match(sessionSync, /displayEnabled \? visibleSessions : \[\]/);
  assert.match(sessionSync, /const switchCandidates = selectableSessions/);
  assert.doesNotMatch(sessionSync, /routingSessions\.forEach\(append\)/);
  assert.match(sessionSync, /const selectedDeviceSessionTitle = selectedVisibleSession/);
  assert.doesNotMatch(
    sessionSync,
    /const selectedDeviceSessionTitle = selectedRoutingSession/,
  );
  assert.match(sessionSync, /cycleVoiceSessionId\(sessionId, switchCandidates, direction\)/);
  assert.match(sessionSync, /filterActiveDeviceSessions\(selectableSessions\)/);
  assert.doesNotMatch(sessionSync, /filterActiveDeviceSessions\(routingSessions\)/);
  assert.match(sessionSync, /sessionCount: selectableSessions\.length/);
  assert.match(sessionSync, /activeSessionIds,/);
  assert.match(sessionSync, /displayEnabled: cardsEnabled/);
  assert.match(sessionSync, /transitionRevision: deviceSessionTransitionRevision\(session\)/);
  assert.match(sessionSync, /terminalUntilMs: deviceSessionTerminalUntilMs\(session\)/);
  assert.match(
    sessionSync,
    /sessions: buildP4DeviceSessionTransportPayload\(deviceSessions\)/,
  );
  assert.match(sessionService, /terminalRemainingMs: Math\.min\(/);
  assert.doesNotMatch(
    sessionSync,
    /sessions:\s*deviceSessions,/,
  );
  assert.match(source, /useAgentSessionFeed\(/);
  assert.match(source, /useP4SessionSync\(/);
  assert.match(sessionFeed, /buildP4ConversationQueue\(/);
  assert.match(source, /showSessionDisplaySetting=\{isP4Runtime\}/);
  assert.match(source, /sessionDisplayEnabled=\{sessionDisplayEnabled\}/);
  assert.match(source, /onSessionDisplayEnabledChange=\{onSessionDisplayEnabledChange\}/);
  assert.doesNotMatch(source, /onSessionDisplayCountChange/);
  assert.match(matrix, /import Switch from "\.\.\/shell\/Switch"/);
  assert.doesNotMatch(matrix, /SESSION_DISPLAY_COUNT_OPTIONS/);
  assert.doesNotMatch(matrix, /sessionDisplayCount/);
  assert.match(matrix, /仅显示当前运行中的 Agent 对话，结束或出错后保留 60 秒/);
  assert.match(sessionDisplay, /DEVICE_SESSION_TERMINAL_HOLD_MS = 60_000/);
  assert.match(sessionDisplay, /statusRevision = normalizedTransitionRevision\(current\?\.statusUpdatedAt\)/);
  assert.doesNotMatch(sessionDisplay, /currentTime\s*-\s*statusUpdatedAt/);
});

test("P4 sessions consume ordered lifecycle events without accelerating metadata scans", () => {
  const source = readSource("dashboard/useAgentSessionFeed.js");
  const sessionSync = readSource("dashboard/useP4SessionSync.js");

  assert.match(source, /fetchAgentSessionEvents\(/);
  assert.match(source, /P4_SESSION_EVENT_POLL_MS = 750/);
  assert.match(source, /P4_SESSION_SNAPSHOT_POLL_MS = 5000/);
  assert.match(source, /type: "apply_event"/);
  assert.match(source, /pendingEvents\.shift\(\)/);
  assert.match(source, /P4_SESSION_EVENT_DISPATCH_GAP_MS/);
  assert.match(source, /type: "tick_terminal"/);
  assert.match(source, /mergeP4SessionSnapshot/);
  assert.match(source, /mergeP4SessionEvent/);
  assert.match(sessionSync, /bindingQueueRef\.current[\s\S]*invoke\("set_p4_session_binding"/);
  assert.doesNotMatch(source, /setInterval\(run, 3000\)/);
  const streamReset = source.match(
    /if \(result\.reset \|\| streamChanged\) \{([\s\S]*?)\n\s*\}/,
  );
  assert.ok(streamReset, "expected event stream reset handling");
  assert.match(streamReset[1], /pendingEvents\.length = 0/);
  assert.doesNotMatch(streamReset[1], /dispatch\(\{ type: "reset" \}\)/);
});
test("follow changes clear the prior Agent sessions before loading the new snapshot", () => {
  const source = readSource("DeviceDashboard.jsx");
  const sessionFeed = readSource("dashboard/useAgentSessionFeed.js");
  const rust = readRepoFile("src-tauri", "src", "lib.rs");

  assert.match(sessionFeed, /agentId:\s*""/);
  assert.match(sessionFeed, /agentId,/);
  assert.match(sessionFeed, /const matchesAgent = state\.agentId === agentId/);
  assert.match(sessionFeed, /previousAgentIdRef\.current/);
  assert.match(sessionFeed, /dispatch\(\{ type: "reset" \}\)/);
  assert.match(sessionFeed, /type:\s*"replace_snapshot",[\s\S]*agentId,/);
  assert.match(source, /busSessions:\s*selectedAgentBusSessions/);
  assert.match(source, /busRoutingSessions:\s*selectedAgentRoutingSessions/);
  assert.match(rust, /fn reset_p4_session_binding_for_agent/);
  assert.match(rust, /p4_session_agent_switch_required/);
  assert.match(rust, /"sessionId": "auto"/);
  assert.match(rust, /"sessions": \[\]/);
  assert.match(rust, /"agentId": &target_source/);
  assert.match(rust, /"activeSessionIds": \[\]/);
  assert.match(rust, /sessions_reset/);
});

// ---- PORTED: voice button configuration now in BoardButtonPanel ----

test("voice button configuration groups physical controls before device sync", () => {
  const source = readSource("dashboard/BoardButtonPanel.jsx");
  const dashboard = readSource("DeviceDashboard.jsx");
  const css = readSource("styles.css");

  assert.match(source, /board-button-control-groups/);
  assert.match(source, /groupControlRows/);
  assert.match(source, /voice-button-action-select/);
  assert.match(source, /buttonControlRowsForRuntime\(runtime\)/);
  assert.match(source, /controlGroups\.map/);
  assert.match(source, /group\.rows\.map/);
  assert.match(source, /BUTTON_FUNCTION_OPTIONS\.filter/);
  assert.match(source, /voice-button-action-value/);
  assert.match(source, /import Button from "\.\.\/shell\/Button"/);
  assert.doesNotMatch(source, /import Switch from "\.\.\/shell\/Switch"/);
  assert.match(source, /同步到设备/);
  assert.match(source, /待同步/);
  assert.match(dashboard, /label:\s*"不绑定"[\s\S]*忽略该输入/);

  assert.match(css, /\.switch\s*\{/);
  assert.match(css, /\.board-button-panel__workspace\s*\{/);
  assert.match(css, /\.board-button-control-groups\s*\{/);
  assert.match(css, /\.board-button-control-group\s*\{/);
  assert.match(css, /\.board-button-control-group__rows\s*\{/);
  assert.match(css, /\.voice-button-action-select\s*\{/);
  assert.match(css, /\.board-button-panel__toolbar\s*\{/);
});

test("dashboard keeps the client button map authoritative across reconnect and firmware OTA", () => {
  const dashboard = readSource("DeviceDashboard.jsx");
  const rust = readRepoFile("src-tauri", "src", "lib.rs");
  const usb = readRepoFile("src-tauri", "src", "usb_serial.rs");
  const p4Input = readRepoFile("..", "esp-p4-runtime", "main", "pet_p4_input.c");
  const p4Protocol = readRepoFile("..", "esp-p4-runtime", "main", "pet_p4_protocol.c");
  const linuxBoard = readRepoFile("..", "legacy", "board-runtime", "src", "board_server.c");

  assert.match(dashboard, /invoke\("usb_get_button_config"/);
  assert.match(dashboard, /boardButtonConfigMatchesClient\(clientConfig, boardConfig, usb\.runtime\)/);
  assert.match(dashboard, /await syncClientConfig\(\)/);
  assert.match(dashboard, /已使用客户端按钮配置覆盖设备/);
  assert.match(dashboard, /设备配置读取失败，正在直接写入客户端按钮配置/);
  assert.doesNotMatch(dashboard, /saveVoiceConfigToStorage\(persistedNext\)/);
  assert.doesNotMatch(dashboard, /已从板端读取按钮配置并更新客户端缓存/);
  assert.match(dashboard, /buttonConfigHydratedFor !== p4TargetBoardDeviceId/);
  assert.match(dashboard, /startingRevision !== buttonConfigRevisionRef\.current/);
  assert.match(dashboard, /component_center/);
  assert.match(rust, /async fn usb_get_button_config/);
  assert.match(rust, /manager\.query_button_config/);
  assert.match(usb, /"input\/config-query"/);
  assert.match(usb, /"input\/config-state"/);
  assert.match(usb, /query_button_config[\s\S]*with_asset_transfer_guard/);
  assert.match(rust, /button_config_signal[\s\S]*with_asset_transfer_guard/);
  assert.match(p4Protocol, /strcmp\(topic, "input\/config-query"\)/);
  assert.match(p4Input, /pet_p4_input_send_config_state/);
  assert.match(p4Input, /cJSON_AddItemToObject\(config, "bindings", bindings\)/);
  assert.match(linuxBoard, /br_handle_button_config_query/);
  assert.match(linuxBoard, /server->config\.button_config_path/);
  assert.match(linuxBoard, /"input\/config-state"/);
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
  assert.match(dashboard, /const action = row\.actionOptions\.includes\(normalizedButtonActions\[row\.id\]\)/);
});

test("ESP32-P4 exposes button presses plus all four joystick directions", () => {
  const dashboard = readSource("DeviceDashboard.jsx");
  const panel = readSource("dashboard/BoardButtonPanel.jsx");
  const rows = dashboard.match(/export const P4_BUTTON_CONTROL_ROWS = \[([\s\S]*?)\];/);
  const visibleOptions = dashboard.match(/export const BUTTON_FUNCTION_OPTIONS = \[([\s\S]*?)\];/);
  const p4Options = dashboard.match(/const P4_CUSTOM_ACTION_OPTIONS = \[([\s\S]*?)\];/);

  assert.ok(rows, "expected P4_BUTTON_CONTROL_ROWS");
  assert.ok(visibleOptions, "expected BUTTON_FUNCTION_OPTIONS");
  assert.ok(p4Options, "expected P4_CUSTOM_ACTION_OPTIONS");
  assert.match(dashboard, /export const DEFAULT_BUTTON_ACTIONS = \{[\s\S]*?p4_sw1_short:\s*"page_enter"/);
  assert.match(dashboard, /p4_sw1_long:\s*"voice_ptt"/);
  assert.match(dashboard, /p4_sw2_short:\s*"component_center"/);
  assert.match(dashboard, /p4_sw2_long:\s*"disabled"/);
  assert.match(dashboard, /export const DEFAULT_BUTTON_ACTIONS = \{[\s\S]*?p4_sw3_short:\s*"page_back"/);
  assert.match(dashboard, /p4_sw3_long:\s*"disabled"/);
  assert.match(dashboard, /p4_encoder_press:\s*"page_enter"/);
  assert.match(dashboard, /p4_encoder_long:\s*"disabled"/);
  assert.match(dashboard, /p4_encoder_cw:\s*"session_next"/);
  assert.match(dashboard, /p4_encoder_ccw:\s*"session_previous"/);
  assert.match(dashboard, /p4_joystick_up:\s*"disabled"/);
  assert.match(dashboard, /p4_joystick_down:\s*"disabled"/);
  assert.match(dashboard, /event:\s*"button\.sw1\.short_press"/);
  assert.match(dashboard, /event:\s*"button\.sw1\.long_press"/);
  assert.match(dashboard, /holdEvent:\s*"button\.sw1\.hold"/);
  assert.match(dashboard, /event:\s*"button\.sw2\.short_press"/);
  assert.match(dashboard, /event:\s*"button\.sw2\.long_press"/);
  assert.match(dashboard, /holdEvent:\s*"button\.sw2\.hold"/);
  assert.match(dashboard, /event:\s*"button\.sw3\.short_press"/);
  assert.match(dashboard, /event:\s*"button\.sw3\.long_press"/);
  assert.match(dashboard, /holdEvent:\s*"button\.sw3\.hold"/);
  assert.match(dashboard, /event:\s*"button\.encoder\.short_press"/);
  assert.match(dashboard, /event:\s*"button\.encoder\.long_press"/);
  assert.match(dashboard, /holdEvent:\s*"button\.encoder\.hold"/);
  assert.match(dashboard, /event:\s*"knob\.rotate_cw"/);
  assert.match(dashboard, /event:\s*"knob\.rotate_ccw"/);
  assert.match(dashboard, /event:\s*"joystick\.up"/);
  assert.match(dashboard, /event:\s*"joystick\.down"/);
  assert.match(visibleOptions[1], /id: "agent_prompt", label: "发送自定义指令"[\s\S]*id: "voice_ptt", label: "语音输入"/);
  assert.match(visibleOptions[1], /id: "session_previous", label: "上一个"/);
  assert.match(visibleOptions[1], /id: "session_next", label: "下一个"/);
  assert.match(dashboard, /直接发送给当前 Code Agent/);
  assert.match(visibleOptions[1], /id: "component_center", label: "切换宠物\/组件"/);
  assert.match(visibleOptions[1], /id: "page_enter", label: "确认"/);
  assert.match(visibleOptions[1], /id: "page_back", label: "返回（取消）"/);
  assert.doesNotMatch(visibleOptions[1], /继续 Agent|触发当前负一屏|当前组件动作|切换屏幕/);
  assert.doesNotMatch(p4Options[1], /agent_enter|miniapp_screen_tap|miniapp_screen_long_press|miniapp_action|page_toggle/);
  assert.match(dashboard, /p4_encoder_press[\s\S]*defaultAction: "page_enter"/);
  assert.match(dashboard, /p4_encoder_long[\s\S]*defaultAction: "disabled"[\s\S]*voiceFallbackAction: "disabled"/);
  assert.match(dashboard, /LEGACY_P4_DEFAULT_BUTTON_ACTIONS/);
  assert.match(dashboard, /P4_V2_DEFAULT_BUTTON_ACTIONS/);
  assert.match(dashboard, /P4_V3_DEFAULT_BUTTON_ACTIONS/);
  assert.match(dashboard, /P4_V4_DEFAULT_BUTTON_ACTIONS/);
  assert.match(dashboard, /P4_V5_DEFAULT_BUTTON_ACTIONS/);
  assert.match(dashboard, /P4_V6_DEFAULT_BUTTON_ACTIONS/);
  assert.match(dashboard, /storedButtonModelVersion === 6[\s\S]*P4_V6_DEFAULT_BUTTON_ACTIONS/);
  assert.match(dashboard, /action === "agent_prompt" \|\| action === "miniapp_action"/);
  assert.match(dashboard, /buttonLabels/);
  assert.doesNotMatch(dashboard, /label: "返回首页"/);
  assert.doesNotMatch(dashboard, /label: "返回负一屏"/);
  assert.match(dashboard, /incomingAction === "page_main" \|\| incomingAction === "page_app"/);
  assert.doesNotMatch(dashboard, /id: "page_stats"/);
  assert.match(dashboard, /"session_next"/);
  assert.match(dashboard, /"session_previous"/);
  assert.match(dashboard, /flatMap\(\(row\) =>/);
  assert.match(dashboard, /row\.holdEvent/);
  assert.match(dashboard, /row\.holdEvent,\s*action:\s*voiceEnabled \? "voice_ptt" : "disabled"/);
  assert.doesNotMatch(dashboard, /P4_DISABLED_SWITCH_LEGACY_BINDINGS/);
  assert.match(dashboard, /buttonModelVersion/);
  assert.match(dashboard, /clampButtonActionValue/);
  assert.match(dashboard, /new TextEncoder\(\)\.encode/);
  assert.match(panel, /runtime[\s\S]*esp-p4/);
  assert.doesNotMatch(panel, /findButtonActionOwner|已绑定：/);
  assert.doesNotMatch(panel, /nextActions\[item\.id\] = item\.voiceFallbackAction/);
  assert.match(panel, /maxLength=\{120\}/);
});

test("Negative-screen proxy actions stay runtime-compatible without appearing in the P4 menu", () => {
  const dashboard = readSource("DeviceDashboard.jsx");
  const rust = readRepoFile("src-tauri", "src", "lib.rs");
  const firmware = readRepoFile("..", "esp-p4-runtime", "main", "pet_p4_input.c");
  const miniapp = readRepoFile("..", "esp-p4-runtime", "main", "pet_p4_miniapp.c");
  const p4Options = dashboard.match(/const P4_CUSTOM_ACTION_OPTIONS = \[([\s\S]*?)\];/);

  assert.match(dashboard, /"miniapp_screen_tap"/);
  assert.match(dashboard, /"miniapp_screen_long_press"/);
  assert.ok(p4Options);
  assert.doesNotMatch(p4Options[1], /miniapp_screen_tap|miniapp_screen_long_press/);
  assert.match(rust, /"miniapp_screen_tap"/);
  assert.match(rust, /"miniapp_screen_long_press"/);
  assert.match(firmware, /miniapp_event = "screen\.region\.tap"/);
  assert.match(firmware, /miniapp_event = "screen\.region\.long_press"/);
  assert.doesNotMatch(firmware, /pet_p4_miniapp_dispatch_input\(\s*event_name/);
  assert.doesNotMatch(miniapp, /strcmp\(event_name, "button\.sw1\.short_press"\)/);
  assert.doesNotMatch(miniapp, /strcmp\(event_name, "button\.sw1\.long_press"\)/);
});

test("SW2 toggles top-level pages while previous and next stay inside the current page", () => {
  const dashboard = readSource("DeviceDashboard.jsx");
  const sessionSync = readSource("dashboard/useP4SessionSync.js");
  const panel = readSource("dashboard/BoardButtonPanel.jsx");
  const rust = readRepoFile("src-tauri", "src", "lib.rs");
  const firmware = readRepoFile("..", "esp-p4-runtime", "main", "pet_p4_input.c");

  assert.match(dashboard, /label: "上一个"/);
  assert.match(dashboard, /label: "下一个"/);
  assert.match(dashboard, /label: "切换宠物\/组件"/);
  assert.doesNotMatch(panel, /P4 目标会话/);
  assert.doesNotMatch(panel, /p4-session-config/);
  assert.match(rust, /"session_next"/);
  assert.match(rust, /"session_previous"/);
  assert.match(firmware, /"session_next"/);
  assert.match(firmware, /"session_previous"/);
  assert.match(firmware, /center_open \? "main" : "components"/);
  assert.match(firmware, /state->current_session_index = selected \+ 1/);
  assert.match(firmware, /pet_p4_miniapp_catalog_move\(direction\)/);
  assert.match(firmware, /copy_text\(action_override, action_override_size, "component_select"\)/);
  assert.doesNotMatch(firmware, /main_open \? "components" : "main"/);
  assert.doesNotMatch(firmware, /"page_previous" : "page_next"/);
  assert.match(sessionSync, /action === "session_next"/);
  assert.match(sessionSync, /action === "session_previous"/);
});

test("voice session state is render-safe before the first bus response", () => {
  const sessionFeed = readSource("dashboard/useAgentSessionFeed.js");
  assert.match(sessionFeed, /sessions:\s*EMPTY_SESSIONS/);
  assert.match(sessionFeed, /routingSessions:\s*EMPTY_SESSIONS/);
  assert.doesNotMatch(sessionFeed, /sessions:\s*null/);
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
  const rust = readRepoFile("src-tauri", "src", "lib.rs");

  assert.match(source, /const targetBoardDeviceId = onlineBoardDeviceId \|\| usb\.boardDeviceId \|\| binding\.boardDeviceId;/);
  assert.match(source, /if \(action === "start"\) \{[\s\S]*ensureBridgeRuntime\(\)/);
  assert.match(source, /ensure_device_voice_runtime/);
  assert.match(source, /isP4Runtime \? "ensure_device_voice_runtime" : "ensure_voice_runtime"/);
  assert.match(source, /boardDeviceId: targetBoardDeviceId/);
  assert.match(source, /\[activeVoiceTriggerId, binding\.boardDeviceId, isP4Runtime, onlineBoardDeviceId, usb\.boardDeviceId\]/);
  assert.match(rust, /fn ensure_device_voice_runtime\s*\(/);
  assert.match(rust, /pc_audio::built_in_stt_status\(\)/);
  assert.match(rust, /pc_microphone_enabled = action == "start" && !p4_usb_connected/);
  assert.match(rust, /relay\.configure\(action == "start", pcm_relay_port, !p4_usb_connected\)/);

  const panelSource = readSource("dashboard/VoiceAssistantPanel.jsx");
  assert.match(panelSource, /audioBlockingReason/);
  assert.match(panelSource, /使用设备麦克风录音并通过 USB 转发/);
  assert.match(panelSource, /disabled=\{[\s\S]*?audioBridgePending[\s\S]*?audioBlockingReason[\s\S]*?voiceConfig\.enabled/);
  assert.doesNotMatch(panelSource, /disabled=\{state\.audioBridgePending \|\| !!blockingReason\}/);
});

test("voice enablement binds a real hardware PTT action and applies it immediately", () => {
  const source = readSource("DeviceDashboard.jsx");
  const panelSource = readSource("dashboard/VoiceAssistantPanel.jsx");

  assert.match(source, /export function applyVoiceEnabledForRuntime/);
  assert.match(source, /next\.buttonActions\[selectedRow\.id\]|\? "voice_ptt"/);
  assert.match(source, /P4_DEFAULT_VOICE_TRIGGER = "sw1\.hold"/);
  assert.match(source, /resolveButtonConfigForRuntime\(requestedConfigInput, usb\.runtime\)/);
  assert.match(source, /const requestedRuntimeVoiceEnabled = requested\.voiceEnabled/);
  assert.match(source, /const updateVoiceEnabled = useCallback/);
  assert.match(source, /await applyVoiceConfigOverUsb\(next\)/);
  assert.match(source, /onVoiceEnabledChange=\{updateVoiceEnabled\}/);
  assert.match(panelSource, /onVoiceEnabledChange\(enabled\)/);
});

test("board voice action injection status is surfaced in the voice panel", () => {
  const dashboardSource = readSource("DeviceDashboard.jsx");
  const voiceRouterSource = readSource("dashboard/useDeviceVoiceRouter.js");
  const source = `${dashboardSource}\n${voiceRouterSource}`;
  const panelSource = readSource("dashboard/VoiceAssistantPanel.jsx");
  const rust = readRepoFile("src-tauri", "src", "lib.rs");

  assert.match(dashboardSource, /useDeviceVoiceRouter/);
  assert.match(source, /postMockButtonInject/);
  assert.match(source, /\/mock-button-inject/);
  assert.match(source, /http_request_text/);
  assert.match(source, /listen\("usb-message"/);
  assert.match(source, /listen\("usb-input-action-result"/);
  assert.match(source, /listen\("voice-transcript"/);
  assert.match(source, /deviceVoiceFlow/);
  assert.match(source, /utteranceId/);
  assert.match(source, /revision/);
  assert.match(source, /payload\.transient === true/);
  assert.match(source, /payload\.pending === true/);
  assert.match(source, /const composerMode = normalizeText\(payload\.composerMode\)\.toLowerCase\(\)/);
  assert.match(source, /composerMode: action\.composerMode \|\| baseState\.flow\.composerMode/);
  assert.match(source, /\["visible", "focused-input"\]\.includes\(action\.composerMode\)[\s\S]*?composerError/);
  assert.match(source, /composerMode === "focused-input"[\s\S]*?MiMoCode 当前光标/);
  assert.match(rust, /context\.target\.agent_id == "mimocode"/);
  assert.match(rust, /capture_focused_text_target/);
  assert.match(rust, /insert_at_focused_text_target/);
  assert.match(rust, /submit_at_focused_text_target/);
  assert.match(rust, /"composerMode": "focused-input"/);
  assert.match(rust, /已通过设备确认键发送 MiMoCode 语音草稿/);
  assert.match(
    source,
    /if \(phase === "cancelled"\)[\s\S]*?设备录音已取消[\s\S]*?else if \(!ok\)[\s\S]*?板端录音处理失败/,
  );
  assert.match(source, /action\.pending[\s\S]*?"waiting_reply"/);
  assert.match(source, /桥接连接瞬时抖动/);
  assert.match(source, /sendMockButtonInject/);
  assert.match(source, /tokenPreview/);
  assert.match(source, /replyPreview/);
  assert.match(rust, /"tokenPreview": reply_preview/);
  assert.match(rust, /"replyPreview": reply_preview/);
  assert.match(rust, /"pending": true/);
  assert.match(rust, /等待模型回复/);
  assert.match(rust, /VISIBLE_COMPOSER_SUBMIT_TIMEOUT: Duration = Duration::from_secs\(8\)/);
  assert.match(rust, /bridge\.confirm\(revision, &text\)/);
  assert.doesNotMatch(source, /后台回退|后台会话桥接/);
  assert.match(source, /agentId: selectedAgentId/);
  assert.match(source, /sessionId: p4SessionSync\.sessionId \|\| "auto"/);

  assert.match(panelSource, /诊断与测试/);
  assert.match(panelSource, /模拟文本注入/);
  assert.match(panelSource, /发送到当前会话/);
  assert.match(panelSource, /模型回复预览/);
  assert.match(panelSource, /设备语音状态/);
  assert.match(panelSource, /实时识别文本/);
});

test("voice session events start before adapter discovery finishes", () => {
  const source = readSource("DeviceDashboard.jsx");
  const sessionFeed = readSource("dashboard/useAgentSessionFeed.js");

  assert.doesNotMatch(source, /selectedBusAgentReady/);
  assert.match(sessionFeed, /fetchAgentSessionEvents\(/);
  assert.match(sessionFeed, /fetchAgentSessions\(agentId, ctl\.signal\)/);
  assert.match(sessionFeed, /scheduleEventPoll/);
  assert.match(sessionFeed, /scheduleSnapshotRefresh/);
  assert.match(sessionFeed, /\[agentId, dismissedSessions, displayEnabled\]/);
});

test("P4 reconnect automatically rearms device microphone relay without PC capture", () => {
  const source = readSource("DeviceDashboard.jsx");

  assert.match(source, /const rearmDeviceVoice = async \(\) =>/);
  assert.match(source, /const bridgeRuntime = await ensureBridgeRuntime\(\);\s*if \(cancelled\) return;/);
  assert.match(source, /invoke\("ensure_device_voice_runtime"\)/);
  assert.match(source, /input: \{ interactive: false \},\s*\}\);\s*if \(cancelled\) return;/);
  assert.match(source, /input: \{ interactive: false \}/);
  assert.match(source, /if \(voiceRuntime\?\.deferred\)/);
  assert.match(source, /onCredentialReady=\{resumeDeviceVoiceAfterCredentialAccess\}/);
  assert.match(source, /message: "设备麦克风已自动恢复"/);
  assert.match(source, /voiceButton: activeVoiceTriggerId/);
  assert.match(source, /buttonConfigHydratedFor !== p4TargetBoardDeviceId/);
  assert.match(source, /\|\| !runtimeVoiceEnabled/);
});

test("Button-selected conversations are temporary and bound to status and input routing", () => {
  const source = readSource("DeviceDashboard.jsx");
  const sessionFeed = readSource("dashboard/useAgentSessionFeed.js");
  const sessionSync = readSource("dashboard/useP4SessionSync.js");
  const sessionService = readSource("lib/p4-session-service.js");
  const panelSource = readSource("dashboard/BoardButtonPanel.jsx");
  const rust = readRepoFile("src-tauri", "src", "lib.rs");

  assert.doesNotMatch(source, /P4_SESSION_STORAGE_PREFIX/);
  assert.doesNotMatch(source, /p4SessionStorageKey/);
  assert.match(sessionSync, /P4_MANUAL_SESSION_TIMEOUT_MS/);
  assert.match(sessionSync, /selectedAt/);
  assert.match(source, /sessionsLoaded: sessionFeed\.loaded/);
  assert.match(sessionService, /buildP4RoutingSessions/);
  assert.match(sessionFeed, /buildP4ConversationQueue/);
  assert.match(sessionSync, /filterActiveDeviceSessions/);
  assert.doesNotMatch(sessionSync, /P4_SESSION_DONE_HOLD_MS/);
  assert.doesNotMatch(sessionSync, /P4_SESSION_STALE_MS/);
  assert.match(sessionSync, /action === "session_next"/);
  assert.match(sessionSync, /action === "session_previous"/);
  assert.match(sessionSync, /set_p4_session_binding/);
  assert.match(source, /boardDeviceId: p4TargetBoardDeviceId/);
  assert.match(sessionSync, /const confirmedEmptyQueue = sessionsLoaded/);
  assert.match(
    sessionSync,
    /const autoRoutingSession = selectableSessions\[0\] \|\| routingSessions\?\.\[0\] \|\| sessions\?\.\[0\]/,
  );
  assert.match(sessionSync, /const autoFollow = sessionId === "auto" && Boolean\(selectedRoutingSession\)/);
  assert.match(sessionSync, /!selectedRoutingSession[\s\S]*!confirmedEmptyQueue/);
  assert.match(sessionFeed, /case "reset"/);
  assert.match(sessionSync, /normalizeText\(selectedRoutingSession\?\.id\)/);
  assert.match(sessionSync, /sessionId: requestedSessionId/);
  assert.match(sessionSync, /autoFollow,/);
  assert.match(sessionSync, /export function formatDeviceSessionTitle/);
  assert.match(sessionSync, /export function formatDeviceSessionContent/);
  assert.match(sessionSync, /content: formatDeviceSessionContent\(session, title\)/);
  assert.match(sessionSync, /sessionTitle: selectedSessionTitle/);
  assert.match(sessionSync, /deviceTitle: selectedDeviceSessionTitle/);
  assert.match(sessionSync, /sessionIndex: selectedSessionIndex/);
  assert.match(sessionSync, /sessionCount: selectableSessions\.length/);
  assert.match(sessionSync, /action === "session_clear"/);
  assert.match(sessionSync, /p4SessionActivitySignature/);
  assert.match(sessionSync, /sessions: buildP4DeviceSessionTransportPayload\(deviceSessions\)/);
  assert.match(sessionService, /session\?\.model[\s\S]*codex-auto-review/);
  assert.match(sessionFeed, /eventsInFlight/);
  assert.doesNotMatch(sessionFeed, /catch[\s\S]*sessions:\s*\[\]/);
  assert.match(sessionSync, /deviceSessionsSignature,[\s\S]*usbConnected/);
  assert.doesNotMatch(panelSource, /P4 目标会话/);
  assert.match(rust, /fn set_p4_session_binding/);
  assert.match(rust, /fn usb_session_binding_allows/);
  assert.match(rust, /fn resolve_usb_inject_target/);
  assert.match(rust, /"sessionId": session_id/);
  assert.match(rust, /"session\/current"/);
  assert.match(rust, /"sessions": device_sessions/);
  assert.match(rust, /"content": content/);
  assert.match(rust, /device_title: Option<String>/);
  assert.match(rust, /"title": device_title/);
  assert.match(rust, /"notice": device_notice/);
});

test("Codex conversation switching locates the desktop task without background voice fallback", () => {
  const source = readSource("DeviceDashboard.jsx");
  const sessionSync = readSource("dashboard/useP4SessionSync.js");
  const rust = readRepoFile("src-tauri", "src", "lib.rs");
  const composer = readRepoFile("src-tauri", "src", "codex_composer.rs");

  assert.match(sessionSync, /isDeviceSessionTargetUnique/);
  assert.match(sessionSync, /resultSessionId !== requestedSessionId/);
  assert.match(rust, /CodexComposerBridge::focus_session/);
  assert.match(rust, /VISIBLE_COMPOSER_UNAVAILABLE/);
  assert.match(rust, /VISIBLE_COMPOSER_FAILED/);
  assert.match(composer, /Open-CodexSession/);
  assert.match(composer, /codex:\/\/threads\/\{session_id\}/);
  assert.match(composer, /function Open-CodexSessionById/);
  assert.match(composer, /Start-Process \$deepLink/);
  assert.match(composer, /Test-WorkspaceAncestor/);
  const navigation = composer.match(
    /function Invoke-CodexSessionRow\([\s\S]*?function Open-CodexSession/,
  );
  assert.ok(navigation, "expected Codex session navigation block");
  assert.match(composer, /const CODEX_COMPOSER_STARTUP_TIMEOUT_SECS: u64 = 7/);
  assert.match(composer, /WINDOWS_COMPOSER_PROCESS_MEMORY_LIMIT_BYTES/);
  assert.match(composer, /JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE/);
  assert.match(composer, /JOB_OBJECT_LIMIT_PROCESS_MEMORY/);
  assert.match(composer, /Start-Sleep -Milliseconds 120/);
  assert.match(composer, /function Ensure-CodexForeground/);
  assert.match(composer, /function Wait-CodexWindowRoot/);
  assert.match(composer, /function Test-FiniteWindowRectangle/);
  assert.match(composer, /\[double\]::IsInfinity\(\$number\)/);
  assert.match(composer, /\[Math\]::Max\(\[double\]1, \[double\]\(\$rect\.Width \* \$rect\.Height\)\)/);
  assert.match(composer, /if \(\$hasFiniteRectangle -and -not \$root\.Current\.IsOffscreen -and/);
  assert.match(composer, /\[CodexVoiceNative\]::RestoreWindow\(\$handle\)/);
  assert.match(composer, /AutomationElement\]::FromHandle\(\[IntPtr\]\$handle\)/);
  assert.match(composer, /AppActivate\(\[int\]\$root\.Current\.ProcessId\)/);
  assert.match(composer, /ControlType\]::Group\.Id/);
  assert.match(navigation[0], /Find-CodexSessionRows/);
  assert.match(navigation[0], /Test-CodexSessionPoint/);
  assert.match(navigation[0], /\$clickDeadline = \(Get-MonotonicMilliseconds\) \+ 1200/);
  assert.doesNotMatch(navigation[0], /InvokePattern/);
  const openSession = composer.match(
    /function Open-CodexSession\([\s\S]*?function Get-ComposerText/,
  );
  assert.ok(openSession, "expected Codex open-session block");
  assert.match(
    openSession[0],
    /Ensure-CodexForeground \$window\.Root 'for session selection'[\s\S]*?Test-SelectedSessionTitle/,
  );
  assert.match(openSession[0], /catch \{ continue \}/);
  const exactSession = composer.match(
    /function Open-CodexSessionById\([\s\S]*?function Get-ComposerText/,
  );
  assert.ok(exactSession, "expected exact ChatGPT session navigation block");
  assert.match(
    exactSession[0],
    /Ensure-CodexForeground[\s\S]*?for already selected session voice input[\s\S]*?Test-SelectedSessionTitle/,
  );
  const currentVisible = composer.match(
    /function Get-CurrentVisibleTarget\([\s\S]*?function Assert-TargetCurrent/,
  );
  assert.ok(currentVisible, "expected current-visible Agent lookup block");
  assert.match(currentVisible[0], /foreach \(\$window in \$windows\)/);
  assert.match(currentVisible[0], /AppActivate\(\[int\]\$window\.ProcessId\)/);
  const explicitFailure = rust.match(
    /VisibleComposerSubmitOutcome::ExplicitFailure\(error\)([\s\S]*?)VisibleComposerSubmitOutcome::Unconfirmed/,
  );
  assert.ok(explicitFailure, "expected explicit visible-composer failure branch");
  assert.doesNotMatch(explicitFailure[1], /submit_device_voice_via_agent_bus/);
});

test("Claude Desktop Code switching uses exact sessions and foreground voice delivery", () => {
  const source = readSource("DeviceDashboard.jsx");
  const rust = readRepoFile("src-tauri", "src", "lib.rs");
  const composer = readRepoFile("src-tauri", "src", "codex_composer.rs");

  assert.doesNotMatch(source, /selectedBusAgentReady/);
  assert.match(rust, /CodexComposerBridge::focus_claude_session/);
  assert.match(rust, /CodexComposerBridge::start_claude/);
  assert.match(composer, /claude_desktop_session_target_from_root/);
  assert.match(composer, /function Get-ClaudeDesktopSessionState/);
  assert.match(composer, /FocusedDesktopSessionId/);
  assert.match(composer, /function Open-ClaudeSession/);
  assert.match(composer, /function Find-ClaudeSessionRows/);
  assert.match(composer, /function Invoke-ClaudeSessionRow/);
  assert.match(composer, /function Get-ClaudeWindows/);
  assert.match(composer, /function Get-OrLaunchClaudeWindows/);
  assert.match(composer, /com\.squirrel\.AnthropicClaude\.claude/);
  assert.match(composer, /Claude_pzs8sxrjxfjjc!Claude/);
  assert.match(composer, /Test-ClaudeSelectedSession/);
  assert.match(composer, /AppActivate\(\[int\]\$state\.ProcessId\)/);
  const windowsClaudeNavigation = composer.match(
    /function Open-ClaudeSession\([\s\S]*?function Test-WorkspaceAncestor/,
  );
  assert.ok(windowsClaudeNavigation, "expected Windows Claude navigation block");
  assert.match(windowsClaudeNavigation[0], /Invoke-ClaudeSessionRow/);
  assert.doesNotMatch(windowsClaudeNavigation[0], /Start-Process/);
});

test("macOS Codex conversation switching uses native accessibility foreground delivery", () => {
  const composer = readRepoFile("src-tauri", "src", "codex_composer.rs");
  const macComposer = readRepoFile("src-tauri", "src", "codex_composer_macos.rs");
  const cargo = readRepoFile("src-tauri", "Cargo.toml");
  const dashboard = readSource("DeviceDashboard.jsx");
  const app = readSource("App.jsx");

  assert.match(composer, /#\[cfg\(target_os = "macos"\)\][\s\S]*mod macos;/);
  assert.match(composer, /#\[cfg\(target_os = "macos"\)\][\s\S]*impl CodexComposerBridge/);
  assert.match(cargo, /cfg\(target_os = "macos"\)/);
  assert.match(cargo, /accessibility = "0\.2"/);
  assert.match(macComposer, /AXIsProcessTrusted/);
  assert.match(macComposer, /AXIsProcessTrustedWithOptions/);
  assert.match(macComposer, /kAXTrustedCheckOptionPrompt/);
  const permissionRequest = macComposer.match(
    /pub\(super\) fn request_accessibility_permission\(\)[\s\S]*?\n}/,
  );
  assert.ok(permissionRequest, "expected native Accessibility consent request");
  assert.match(permissionRequest[0], /if accessibility_permission_granted\(\)[\s\S]*?return true/);
  assert.match(permissionRequest[0], /AXIsProcessTrustedWithOptions/);
  assert.match(permissionRequest[0], /arm_accessibility_settings_redirect\(\)/);
  assert.match(macComposer, /Privacy_Accessibility/);
  assert.match(macComposer, /com\.apple\.settings\.PrivacySecurity\.extension/);
  assert.doesNotMatch(macComposer, /com\.apple\.preference\.security/);
  assert.match(macComposer, /SettingsActivationGate::new\(system_settings_is_active\(\)\)/);
  assert.match(macComposer, /activation_gate\.update\(system_settings_is_active\(\)\)/);
  assert.match(
    macComposer,
    /ACCESSIBILITY_SETTINGS_ROUTE_DELAY: Duration = Duration::from_secs\(4\)/,
  );
  assert.match(macComposer, /WATCHER_ACTIVE/);
  assert.match(app, /hasTauriRuntime\(\)[\s\S]*?invoke\("request_codex_accessibility_permission"\)/);
  assert.doesNotMatch(dashboard, /需要辅助功能权限/);
  assert.doesNotMatch(dashboard, /macosAccessibilityNoticeShown/);
  const permissionGuard = macComposer.match(
    /fn ensure_accessibility_permission\(\)[\s\S]*?\n}/,
  );
  assert.ok(permissionGuard, "expected macOS Accessibility enforcement block");
  assert.match(permissionGuard[0], /request_accessibility_permission\(\)/);
  assert.match(macComposer, /AXUIElementPostKeyboardEvent/);
  assert.match(macComposer, /pub\(super\) fn focus_session/);
  assert.match(macComposer, /pub\(super\) fn begin_voice/);
  assert.match(macComposer, /pub\(super\) fn begin_current_voice/);
  assert.match(macComposer, /pub\(super\) fn update_voice/);
  assert.match(macComposer, /pub\(super\) fn confirm_voice/);
  assert.match(macComposer, /当前会话与设备选中的会话不一致/);
  const exactVoiceStart = macComposer.match(
    /pub\(super\) fn begin_voice[\s\S]*?pub\(super\) fn begin_current_voice/,
  );
  assert.ok(exactVoiceStart, "expected macOS exact-session voice start block");
  assert.match(exactVoiceStart[0], /find_target/);
  assert.match(
    exactVoiceStart[0],
    /Err\(session_match_error\)[\s\S]*?agent == MacosAgent::Codex[\s\S]*?!session_id\.trim\(\)\.is_empty\(\)[\s\S]*?find_current_visible_target/,
  );
  assert.doesNotMatch(exactVoiceStart[0], /press_unique_session_row/);
  assert.match(exactVoiceStart[0], /current_visible_target: pin_visible_target\.then_some\(target\)/);
  const uniqueRowGuard = macComposer.match(
    /fn press_unique_session_row[\s\S]*?pub\(super\) fn focus_session/,
  );
  assert.ok(uniqueRowGuard, "expected reusable unique Session-row guard");
  assert.match(uniqueRowGuard[0], /rows\.len\(\) > 1 && !workspace_label\.is_empty\(\)/);
  assert.match(uniqueRowGuard[0], /rows\.retain\(\|row\| row\.workspace_matches\)/);
  assert.match(uniqueRowGuard[0], /if rows\.is_empty\(\)/);
  assert.match(uniqueRowGuard[0], /if rows\.len\(\) > 1/);
  assert.match(uniqueRowGuard[0], /row\.target[\s\S]*?\.press\(\)/);
  assert.match(composer, /macos::MacosAgent::Claude/);
  assert.match(macComposer, /com\.anthropic\.claudefordesktop/);
  assert.match(macComposer, /com\.openai\.codex/);
  assert.match(macComposer, /com\.openai\.chat/);
  assert.match(macComposer, /fn primary_or_launch_agent_window/);
  assert.match(macComposer, /args\(\["-b", \*bundle_identifier\]\)/);
  assert.match(macComposer, /CFString::new\("AXMinimized"\)/);
  const currentAgentLaunch = macComposer.match(
    /fn primary_or_launch_agent_window[\s\S]*?fn focus_agent_front_window/,
  );
  assert.ok(currentAgentLaunch, "expected macOS current-Agent launch block");
  assert.match(currentAgentLaunch[0], /ensure_accessibility_permission\(\)\?/);
  assert.match(macComposer, /MacosAgent::Claude/);
  assert.match(macComposer, /write your prompt to claude/);
  assert.match(macComposer, /type \/ for commands/);
  assert.match(macComposer, /session_deeplink\(agent, session_id, requested_deep_link\)/);
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
  assert.match(rust, /DEFAULT_BRIDGE_PORT\r?\n\s*\);/);
  assert.match(rust, /error\.is_connect\(\) && attempt < 8/);
  assert.match(rust, /"requestId": request_id\.clone\(\)/);
  assert.doesNotMatch(rust, /let ports = \[DEFAULT_BRIDGE_PORT,\s*LEGACY_BRIDGE_PORT\];/);
});

test("channel switching is exact-board USB-only with no Bridge MQTT fallback", () => {
  const rust = readRepoFile("src-tauri", "src", "lib.rs");
  const match = rust.match(
    /async fn dispatch_remote_cli_binding[\s\S]*?\/\/ ── Desktop device ID ──/,
  );
  assert.ok(match, "expected dispatch_remote_cli_binding block");
  const dispatch = match[0];

  assert.match(dispatch, /if !usb_status\.connected/);
  assert.match(dispatch, /requested_board_device_id != board_device_id/);
  assert.match(dispatch, /send_to_board\(/);
  assert.match(dispatch, /"control\/remote-cli-binding"/);
  assert.doesNotMatch(dispatch, /publish-remote-binding/);
  assert.doesNotMatch(dispatch, /mqtt_sent/);
});

// ---- PORTED: JS-side in orchestrator; panel-side in BoardButtonPanel; Rust stays ----

test("board button config sends twelve visible gestures as sixteen internal bindings", () => {
  const source = readSource("DeviceDashboard.jsx");
  const rust = readRepoFile("src-tauri", "src", "lib.rs");

  assert.match(source, /VOICE_BUTTON_OPTIONS/);
  assert.match(source, /trigger:\s*"encoder_button\.hold"/);
  assert.match(source, /P4_DEFAULT_VOICE_TRIGGER = "sw1\.hold"/);
  assert.doesNotMatch(source, /top_button\.hold/);
  assert.match(source, /voiceConfig/);
  assert.match(source, /onApplyVoiceConfig/);
  assert.match(source, /VOICE_CONFIG_STORAGE_KEY/);
  assert.match(source, /buildBoardButtonConfigBindings/);
  assert.doesNotMatch(source, /P4_DISABLED_SWITCH_LEGACY_BINDINGS/);
  // button_config_signal invoke now lives in the shared dispatchBoardButtonConfig
  // helper, reused by both the manual OTA button and the component-center install.
  assert.match(source, /export async function dispatchBoardButtonConfig/);
  assert.match(source, /invoke\("button_config_signal"/);
  assert.match(source, /bindings: buildBoardButtonConfigBindings\(buttonActions, buttonValues, runtime, voiceEnabled\)/);
  assert.match(source, /const ack = await dispatchBoardButtonConfig\(/);
  assert.match(source, /voiceEnabled: requestedRuntimeVoiceEnabled/);
  assert.match(source, /按钮配置已写入，正在准备设备麦克风识别通道/);
  assert.match(source, /invoke\("ensure_device_voice_runtime"\)/);
  assert.match(source, /invoke\("audio_bridge_signal"/);
  assert.match(source, /语音通道未就绪/);
  const buttonDispatchIndex = source.indexOf("const ack = await dispatchBoardButtonConfig(");
  const voiceRuntimeIndex = source.indexOf(
    "const bridgeRuntime = await ensureBridgeRuntime()",
    buttonDispatchIndex,
  );
  assert.ok(buttonDispatchIndex >= 0 && buttonDispatchIndex < voiceRuntimeIndex);
  assert.match(source, /requestId/);
  assert.match(source, /ack\?\.ack\?\.bindingCount/);
  assert.match(source, /按钮配置已写入板端/);
  assert.doesNotMatch(source, /waitForButtonConfigAck/);
  assert.doesNotMatch(source, /ackWaiter/);
  assert.doesNotMatch(source, /await ackWaiter\.ready/);
  assert.doesNotMatch(source, /完整按钮配置已通过 USB OTA 下发到板端/);

  const panelSource = readSource("dashboard/BoardButtonPanel.jsx");
  assert.match(panelSource, /个手势/);
  assert.match(panelSource, /12 GESTURES/);
  assert.match(panelSource, /已同步/);
  assert.match(panelSource, /同步到设备/);

  assert.match(rust, /struct ButtonConfigBinding/);
  assert.match(rust, /fn button_config_signal/);
  assert.match(rust, /"button_config"/);
  assert.match(rust, /"request_id"/);
  assert.match(rust, /"bindings"/);
  assert.match(rust, /usb_manager\.send_to_board\(expected_board_device_id, topic, command_payload\)/);
  assert.match(rust, /"input\/config"/);
  assert.match(rust, /"input\/config-ack"/);
  assert.match(rust, /value: Option<String>/);
  assert.match(rust, /"agent_enter"/);
  assert.match(rust, /"agent_prompt"/);
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

test("voice bus polling self-heals the managed bridge after a local port failure", () => {
  const source = readSource("DeviceDashboard.jsx");
  const client = readSource("lib/agent-bus-client.js");
  const rust = readRepoFile("src-tauri", "src", "lib.rs");

  assert.match(source, /let inFlight = false;/);
  assert.match(client, /AGENT_BUS_REQUEST_TIMEOUT_MS = 3000/);
  assert.match(client, /setTimeout\(\(\) => ctl\.abort\(\), AGENT_BUS_REQUEST_TIMEOUT_MS\)/);
  assert.match(source, /const runtime = await ensureBridgeRuntime\(\)/);
  assert.match(source, /if \(cancelled \|\| ctl\.signal\.aborted\) return;/);
  assert.match(source, /if \(!runtime\?\.running\)/);
  assert.match(
    source,
    /body = await fetchAgentBusStatus\(ctl\.signal\);[\s\S]*body = await fetchAgentBusStatus\(ctl\.signal\);/,
  );
  assert.match(
    rust,
    /pid\.filter\(\|candidate\| process_exists\(\*candidate\)\)[\s\S]*wait_for_bridge_ready\(DEFAULT_BRIDGE_PORT, 6, 250\)/,
  );
  assert.match(
    rust,
    /managed bridge \{existing_pid\} is alive but unresponsive; replacing it[\s\S]*stop_managed_bridge/,
  );
  assert.match(rust, /probe_agent_bus_running\(DEFAULT_AGENT_BUS_PORT\)/);
  assert.match(rust, /GET \/agent\/health HTTP\/1\.1/);
});

test("P4 voice polling uses the device ASR runtime instead of legacy voice-service", () => {
  const source = readSource("DeviceDashboard.jsx");
  const runtimeStart = source.indexOf("// Poll voice-runtime.");
  const runtimeEnd = source.indexOf("// Refetch sessions", runtimeStart);
  const runtimeEffect = source.slice(runtimeStart, runtimeEnd);

  assert.match(runtimeEffect, /isP4Runtime\s*\?\s*await invoke\("ensure_device_voice_runtime"/);
  assert.match(runtimeEffect, /input: \{ interactive: false \}/);
  assert.match(runtimeEffect, /:\s*await invoke\("ensure_voice_runtime"\)/);
});

test("managed Node runtimes clear inspector options and bind the expected lifecycle", () => {
  const rust = readRepoFile("src-tauri", "src", "lib.rs");

  assert.match(rust, /command\.env_remove\("NODE_OPTIONS"\)/);
  assert.match(rust, /command\.env\("PET_MANAGER_PARENT_PID", std::process::id\(\)\.to_string\(\)\)/);
  assert.match(rust, /command\.env\("AGENT_BUS_PORT", DEFAULT_AGENT_BUS_PORT\.to_string\(\)\)/);
  assert.match(rust, /fn voice_runtime_lifecycle_lock\(\)/);
  assert.match(rust, /VOICE_SERVICE_AGENT_ID_FILE_NAME/);
});

test("voice bus polling stays active in the background and skips unchanged render updates", () => {
  const source = readSource("DeviceDashboard.jsx");
  const sessionFeed = readSource("dashboard/useAgentSessionFeed.js");
  const statusStart = source.indexOf("// Poll voice-bus status.");
  const runtimeStart = source.indexOf("// Poll voice-runtime.", statusStart);
  const sessionsEnd = source.indexOf("// Reflect board-originated voice_input", runtimeStart);
  const statusEffect = source.slice(statusStart, runtimeStart);
  const sessionsEffect = source.slice(runtimeStart, sessionsEnd);

  assert.doesNotMatch(statusEffect, /document\.visibilityState/);
  assert.doesNotMatch(sessionFeed, /document\.visibilityState/);
  assert.match(source, /fingerprint === state\.busStatusFingerprint/);
  assert.match(sessionFeed, /fingerprint === state\.fingerprint/);
});

test("voice runtime polling preserves cadence, avoids overlap, and deduplicates renders", () => {
  const source = readSource("DeviceDashboard.jsx");
  const start = source.indexOf("// Poll voice-runtime.");
  const end = source.indexOf("// Refetch sessions for the current agent.", start);
  const effect = source.slice(start, end);

  assert.match(effect, /let inFlight = false;/);
  assert.match(effect, /if \(cancelled \|\| inFlight\) return;/);
  assert.match(effect, /finally \{[\s\S]*inFlight = false;/);
  assert.match(effect, /setInterval\(run, 5000\)/);
  assert.doesNotMatch(effect, /document\.visibilityState/);
  assert.match(source, /fingerprint === state\.voiceRuntimeFingerprint/);
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
  assert.match(rust, /const DEFAULT_USB_SERIAL_BAUD/);
  assert.match(rust, /fn usb_uart_wire_bytes_per_sec/);
  assert.doesNotMatch(rust, /Duration::from_millis\(3\)/);
  assert.match(tauri, /fn default_appearance_audio_cue_name\(family: &str\)/);
  assert.match(tauri, /"waiting_user" => Some\("waiting_user\.wav"\)/);
  assert.match(tauri, /audio-overrides\.json/);
  assert.match(tauri, /fn ensure_builtin_terrier_source/);
  assert.match(tauri, /custom-appearances\/builtin-terrier\/videos\/\{audio_name\}/);
  assert.match(tauri, /prepare_p4_appearance_by_id/);
});

test("manual P4 appearance recovery carries the exact board identity", () => {
  const source = readSource("DeviceDashboard.jsx");

  assert.match(
    source,
    /invoke\("usb_sync_appearance", \{[\s\S]*appearanceId,[\s\S]*boardDeviceId: p4TargetBoardDeviceId/,
  );
  assert.match(source, /\[p4TargetBoardDeviceId, push\]/);
});

// ---- NEW: 4-section IA tests (added in Step 1) ----

test("dashboard places Agent and appearance before button configuration", () => {
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

  // Agent/appearance comes directly after device status, followed by physical
  // button configuration, while voice remains the final collapsible region.
  const idxStatusBar = source.indexOf("<DeviceStatusBar");
  const idxCurrent = source.indexOf("<ChannelMatrixCard");
  const idxButtons = source.indexOf("<BoardButtonPanel");
  const idxVoice = source.indexOf("<VoiceAssistantPanel");
  assert.ok(idxStatusBar !== -1 && idxCurrent !== -1 && idxButtons !== -1 && idxVoice !== -1);
  assert.ok(idxStatusBar < idxCurrent, "device status before Agent/appearance");
  assert.ok(idxCurrent < idxButtons, "Agent/appearance before button config");
  assert.ok(idxButtons < idxVoice, "button config before voice");
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

test("dashboard freezes the P4 target board when diagnostics or firmware modal opens", () => {
  const source = readSource("DeviceDashboard.jsx");

  const firmwareModal = source.match(/<FirmwareUpdateModal[\s\S]*?\/>/);
  const diagnosticsModal = source.match(/<DeviceDiagnosticsModal[\s\S]*?\/>/);
  assert.ok(firmwareModal, "expected FirmwareUpdateModal");
  assert.ok(diagnosticsModal, "expected DeviceDiagnosticsModal");
  assert.match(source, /setFirmwareTargetBoardDeviceId\(usb\.boardDeviceId \|\| ""\)/);
  assert.match(source, /setDiagnosticsTargetBoardDeviceId\(usb\.boardDeviceId \|\| ""\)/);
  assert.match(firmwareModal[0], /expectedBoardDeviceId=\{firmwareTargetBoardDeviceId\}/);
  assert.match(diagnosticsModal[0], /expectedBoardDeviceId=\{diagnosticsTargetBoardDeviceId\}/);
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

test("dashboard keeps buttons always visible and opens the collapsible voice assistant by default", () => {
  const source = readSource("DeviceDashboard.jsx");

  const buttonsIdx = source.indexOf("<BoardButtonPanel");
  const cardBeforeButtons = source.lastIndexOf("<Card", buttonsIdx);
  const collapsibleBeforeButtons = source.lastIndexOf("<Card.Collapsible", buttonsIdx);
  assert.ok(cardBeforeButtons > collapsibleBeforeButtons, "BoardButtonPanel must live in a plain Card");

  const voiceIdx = source.indexOf("<VoiceAssistantPanel");
  const collapsibleBeforeVoice = source.lastIndexOf("<Card.Collapsible", voiceIdx);
  assert.ok(collapsibleBeforeVoice !== -1 && collapsibleBeforeVoice < voiceIdx, "VoiceAssistantPanel must live in Card.Collapsible");
  assert.match(source.slice(collapsibleBeforeVoice, voiceIdx), /defaultOpen/);
});

test("saved ASR changes rearm an eligible connected P4 listener without a restart", () => {
  const source = readSource("DeviceDashboard.jsx");
  const callback = source.match(
    /const resumeDeviceVoiceAfterCredentialAccess = useCallback\(\(\) => \{[\s\S]*?\n  \}\, \[[\s\S]*?\n  \]\);/,
  );
  assert.ok(callback, "expected ASR credential-ready callback");
  assert.match(callback[0], /!isP4Runtime/);
  assert.match(callback[0], /!runtimeVoiceEnabled/);
  assert.match(callback[0], /!usb\.connected/);
  assert.match(callback[0], /!p4TargetBoardDeviceId/);
  assert.match(callback[0], /toggleAudioBridge\("start"\)/);
  assert.doesNotMatch(callback[0], /audioBridgeDeferred/);
});

test("DeviceDashboard does not expose the unsupported P4 WiFi flow", () => {
  const source = readSource("DeviceDashboard.jsx");
  assert.doesNotMatch(source, /WifiApplyModal/);
  assert.doesNotMatch(source, /onApplyWifi/);
  assert.doesNotMatch(source, /usb_apply_wifi/);
});
