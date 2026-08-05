/**
 * [Input] Bound device, useDeviceContext for state, Tauri voice/USB events, API-settings navigation, and useToast for notices.
 * [Output] Priority-ordered four-region device dashboard with runtime-aware Linux/P4 control maps,
 *          twelve exposed P4 button/joystick gestures (SW1-SW3 short/long plus
 *          joystick center short/long and four directions), an internal hold transport for PTT,
 *          shared voice enablement across button and assistant surfaces,
 *          configurable enter/back navigation,
 *          bounded Agent prompt/session-switch bindings, ACK-gated USB config,
 *          exact-board identity on emergency appearance downlinks,
 *          board-authoritative button-config hydration with local caching,
 *          deduplicated managed-bridge recovery/input injection, device-side Session title sync,
 *          Agent-owned Session state that cannot leak across follow changes,
 *          with polling/reduction and device synchronization delegated to focused hooks,
 *          plus pure routing/merge/terminal-TTL policy in the P4 Session service,
 *          persisted global conversation visibility with active-only admission,
 *          explicit-transition 60-second done/error retention, serialized P4 downlinks,
 *          empty-queue clearing and full active-ID signals,
 *          exact visible-card encoder selection isolated from background routing,
 *          manual and focus-refreshed local Agent discovery with visible scan feedback,
 *          a default-open status-first voice console,
 *          immediate P4 voice rearming after saved ASR configuration changes,
 *          explicit-only Codex/Claude Desktop task navigation for selected P4 conversations,
 *          MiMoCode-only macOS final-text delivery plus Return at the captured current caret,
 *          app-shell/native-operation-owned macOS Accessibility consent,
 *          shared first-visit onboarding state with a reopenable page guide,
 *          and an ESP32-P4 A/B firmware update entry.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useMemo, useReducer, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { RefreshCw } from "lucide-react";
import DeviceGuideModal from "./DeviceGuideModal.jsx";
import {
  ONBOARDING_PAGE_IDS,
  shouldAutoOpenOnboarding,
} from "./lib/onboarding-state.js";
import PageShell from "./shell/PageShell.jsx";
import Card from "./shell/Card.jsx";
import Button from "./shell/Button.jsx";
import { useDeviceContext } from "./shell/DeviceContext.jsx";
import { useToast } from "./shell/ToastStack.jsx";
import DeviceStatusBar from "./dashboard/DeviceStatusBar.jsx";
import ChannelMatrixCard from "./dashboard/ChannelMatrixCard.jsx";
import BoardButtonPanel from "./dashboard/BoardButtonPanel.jsx";
import { enforceUniqueButtonActions } from "./dashboard/button-action-policy.js";
import VoiceAssistantPanel, { buildVoiceSummary, formatVoiceSessionOption } from "./dashboard/VoiceAssistantPanel.jsx";
import { useAgentSessionFeed } from "./dashboard/useAgentSessionFeed.js";
import { useDeviceVoiceRouter } from "./dashboard/useDeviceVoiceRouter.js";
import { useP4SessionSync } from "./dashboard/useP4SessionSync.js";
import DashboardActionsMenu from "./dashboard/DashboardActionsMenu.jsx";
import FirmwareUpdateModal from "./dashboard/FirmwareUpdateModal.jsx";
import DeviceDiagnosticsModal from "./dashboard/DeviceDiagnosticsModal.jsx";
import { ACTIVE_APPEARANCE_KEY } from "./lib/desktop-pet-assignment.js";
import { BUILTIN_TERRIER_APPEARANCE_ID } from "./lib/builtin-appearances.js";
import { ensureBridgeRuntime } from "./lib/bridge-runtime.js";
import {
  SESSION_DISPLAY_ENABLED_STORAGE_KEY,
  loadSessionDisplayEnabled,
  saveSessionDisplayEnabled,
} from "./lib/session-display.js";
import { fetchAgentBusStatus } from "./lib/agent-bus-client.js";
export {
  buildP4ConversationQueue,
  buildP4DeviceSessionTransportPayload,
  buildP4RoutingSessions,
  filterDismissedP4Sessions,
  mergeP4SessionEvent,
  mergeP4SessionSnapshot,
  p4SessionActivitySignature,
} from "./lib/p4-session-service.js";
export {
  cycleVoiceSessionId,
  formatDeviceSessionContent,
  formatDeviceSessionTitle,
  isDeviceSessionTargetUnique,
  P4_MANUAL_SESSION_TIMEOUT_MS,
} from "./dashboard/useP4SessionSync.js";
import {
  DEVICE_BUTTON_CONFIG_MODEL_VERSION,
  DEVICE_BUTTON_CONFIG_STORAGE_KEY,
} from "./component-center/button-config.js";

// ---------- Voice config storage + constants (re-exported for BoardButtonPanel) ----------

export const VOICE_CONFIG_STORAGE_KEY = DEVICE_BUTTON_CONFIG_STORAGE_KEY;
export const DEFAULT_VOICE_CONFIG = { enabled: false, trigger: "encoder_button.hold" };
export const P4_RUNTIME_ID = "esp-p4";
export const P4_DEFAULT_VOICE_TRIGGER = "sw1.hold";
export const P4_DEFAULT_PROMPT = "继续当前任务并给出下一步。";

export const DEFAULT_BUTTON_ACTIONS = {
  encoder_button_short: "system_page",
  encoder_button: "system_reset",
  encoder_rotate: "volume_adjust",
  p4_sw1_short: "disabled",
  p4_sw1_long: "voice_ptt",
  p4_sw2_short: "component_center",
  p4_sw2_long: "disabled",
  p4_sw3_short: "page_back",
  p4_sw3_long: "disabled",
  p4_encoder_press: "page_enter",
  p4_encoder_long: "disabled",
  p4_encoder_cw: "session_next",
  p4_encoder_ccw: "session_previous",
  p4_joystick_up: "disabled",
  p4_joystick_down: "disabled",
};

export const P4_VOICE_BUTTON_OPTIONS = [
  { id: P4_DEFAULT_VOICE_TRIGGER, rowId: "p4_sw1_long", label: "SW1 长按", detail: "按住 SW1 开始录音，松开后提交。", event: "button.sw1.hold" },
  { id: "sw2.hold", rowId: "p4_sw2_long", label: "SW2 长按", detail: "按住 SW2 开始录音，松开后提交。", event: "button.sw2.hold" },
  { id: "sw3.hold", rowId: "p4_sw3_long", label: "SW3 长按", detail: "按住 SW3 开始录音，松开后提交。", event: "button.sw3.hold" },
  { id: "encoder_button.hold", rowId: "p4_encoder_long", label: "摇杆中按长按", detail: "按住摇杆中键开始录音，松开后提交。", event: "button.encoder.hold" },
];
export const VOICE_BUTTON_OPTIONS = P4_VOICE_BUTTON_OPTIONS;

export const BUTTON_FUNCTION_OPTIONS = [
  { id: "agent_prompt", label: "发送自定义指令", detail: "按下对应手势后，将该按钮下方填写的指令直接发送给当前 Code Agent。" },
  { id: "voice_ptt", label: "语音输入", detail: "长按开始录音，松开后提交；同一时间只允许一个长按手势作为语音触发。" },
  { id: "session_previous", label: "上一个", detail: "切换到当前 Agent 的上一个可用会话，并让状态和语音输入跟随它。" },
  { id: "session_next", label: "下一个", detail: "切换到当前 Agent 的下一个可用会话，并让状态和语音输入跟随它。" },
  { id: "session_clear", label: "清空主页会话", detail: "清除设备主页当前显示的全部会话；新会话或新活动会自动重新显示。" },
  { id: "component_center", label: "组件中心", detail: "进入板端组件目录，用摇杆选择并打开已安装组件。" },
  { id: "page_enter", label: "确认", detail: "从桌宠首页进入当前组件；组件已打开时优先执行组件自己的按键映射。" },
  { id: "page_back", label: "返回（取消）", detail: "取消当前选择，或从当前组件返回上一级。" },
  { id: "disabled", label: "不绑定", detail: "下发 disabled，让新版板端忽略该输入；不会继续触发系统切页或负一屏操作。" },
  { id: "system_page", label: "系统切页", detail: "保持 main / stats 页面切换，适合旋钮短按。" },
  { id: "system_reset", label: "系统重置", detail: "保留长按重启或重置配网等板端默认能力。" },
  { id: "volume_adjust", label: "音量调节", detail: "旋钮旋转调节系统总音量，屏幕顶部短暂显示音量条。切页可继续用屏幕滑动。" },
];

// Runtime/component compatibility only. These actions remain understood when
// reading an older board snapshot, but are intentionally absent from the P4 UI.
const LEGACY_BUTTON_FUNCTION_OPTIONS = [
  { id: "agent_enter", label: "继续 Agent", detail: "向当前 Agent 会话发送安全的继续指令。" },
  { id: "miniapp_screen_tap", label: "触发当前负一屏点击", detail: "仅在负一屏打开时，触发当前组件已有的屏幕点击动作。" },
  { id: "miniapp_screen_long_press", label: "触发当前负一屏长按", detail: "仅在负一屏打开时，触发当前组件已有的屏幕长按动作。" },
  { id: "miniapp_action", label: "当前组件动作", detail: "由组件中心安装并确认后写入，动作 id 随组件 buttons.json 保存。" },
  { id: "page_toggle", label: "切换屏幕", detail: "在宠物首页与当前负一屏之间切换。" },
  { id: "page_main", label: "切换屏幕", detail: "兼容旧版的首页切换配置。" },
  { id: "page_app", label: "切换屏幕", detail: "兼容旧版的负一屏切换配置。" },
];

export const BOARD_BUTTON_CONTROL_ROWS = [
  { id: "encoder_button_short", label: "前方旋钮短按", event: "button.encoder.short_press", defaultAction: "system_page", actionOptions: ["system_page", "disabled"] },
  { id: "encoder_button", label: "前方旋钮长按", event: "button.encoder.long_press", voiceTriggerId: "encoder_button.hold", defaultAction: "system_reset", actionOptions: ["voice_ptt", "system_reset", "disabled"] },
  { id: "encoder_rotate", label: "前方旋钮旋转", event: "knob.rotate_cw / knob.rotate_ccw", defaultAction: "volume_adjust", actionOptions: ["volume_adjust"] },
];

const P4_CUSTOM_ACTION_OPTIONS = [
  "agent_prompt",
  "session_previous",
  "session_next",
  "session_clear",
  "component_center",
  "page_enter",
  "page_back",
  "disabled",
];
export const P4_BUTTON_CONTROL_ROWS = [
  { id: "p4_sw1_short", controlId: "p4_sw1", label: "SW1 短按", event: "button.sw1.short_press", defaultAction: "disabled", actionOptions: P4_CUSTOM_ACTION_OPTIONS, supportsValue: true },
  { id: "p4_sw1_long", controlId: "p4_sw1", label: "SW1 长按", event: "button.sw1.long_press", holdEvent: "button.sw1.hold", voiceTriggerId: P4_DEFAULT_VOICE_TRIGGER, defaultAction: "voice_ptt", voiceFallbackAction: "disabled", actionOptions: ["voice_ptt", ...P4_CUSTOM_ACTION_OPTIONS], supportsValue: true },
  { id: "p4_sw2_short", controlId: "p4_sw2", label: "SW2 短按", event: "button.sw2.short_press", defaultAction: "component_center", defaultValue: P4_DEFAULT_PROMPT, actionOptions: P4_CUSTOM_ACTION_OPTIONS, supportsValue: true },
  { id: "p4_sw2_long", controlId: "p4_sw2", label: "SW2 长按", event: "button.sw2.long_press", holdEvent: "button.sw2.hold", voiceTriggerId: "sw2.hold", defaultAction: "disabled", voiceFallbackAction: "disabled", actionOptions: ["voice_ptt", ...P4_CUSTOM_ACTION_OPTIONS], supportsValue: true },
  { id: "p4_sw3_short", controlId: "p4_sw3", label: "SW3 短按", event: "button.sw3.short_press", defaultAction: "page_back", actionOptions: P4_CUSTOM_ACTION_OPTIONS, supportsValue: true },
  { id: "p4_sw3_long", controlId: "p4_sw3", label: "SW3 长按", event: "button.sw3.long_press", holdEvent: "button.sw3.hold", voiceTriggerId: "sw3.hold", defaultAction: "disabled", voiceFallbackAction: "disabled", actionOptions: ["voice_ptt", ...P4_CUSTOM_ACTION_OPTIONS], supportsValue: true },
  { id: "p4_joystick_up", controlId: "p4_joystick", label: "摇杆向上", event: "joystick.up", defaultAction: "disabled", actionOptions: P4_CUSTOM_ACTION_OPTIONS, supportsValue: true },
  { id: "p4_joystick_down", controlId: "p4_joystick", label: "摇杆向下", event: "joystick.down", defaultAction: "disabled", actionOptions: P4_CUSTOM_ACTION_OPTIONS, supportsValue: true },
  { id: "p4_encoder_ccw", controlId: "p4_joystick", label: "摇杆向左", event: "knob.rotate_ccw", defaultAction: "session_previous", actionOptions: P4_CUSTOM_ACTION_OPTIONS, supportsValue: true },
  { id: "p4_encoder_cw", controlId: "p4_joystick", label: "摇杆向右", event: "knob.rotate_cw", defaultAction: "session_next", actionOptions: P4_CUSTOM_ACTION_OPTIONS, supportsValue: true },
  { id: "p4_encoder_press", controlId: "p4_joystick", label: "摇杆中按短按", event: "button.encoder.short_press", defaultAction: "page_enter", actionOptions: P4_CUSTOM_ACTION_OPTIONS, supportsValue: true },
  { id: "p4_encoder_long", controlId: "p4_joystick", label: "摇杆中按长按", event: "button.encoder.long_press", holdEvent: "button.encoder.hold", voiceTriggerId: "encoder_button.hold", defaultAction: "disabled", voiceFallbackAction: "disabled", actionOptions: ["voice_ptt", ...P4_CUSTOM_ACTION_OPTIONS], supportsValue: true },
];

const LEGACY_P4_DEFAULT_BUTTON_ACTIONS = {
  p4_sw1_short: "disabled",
  p4_sw2_short: "agent_enter",
  p4_sw3_short: "agent_prompt",
  p4_encoder_press: "disabled",
  p4_encoder_long: "disabled",
  p4_encoder_cw: "page_toggle",
  p4_encoder_ccw: "page_toggle",
};

const P4_V2_DEFAULT_BUTTON_ACTIONS = {
  p4_sw1_short: "agent_enter",
  p4_sw1_long: "voice_ptt",
  p4_sw2_short: "agent_prompt",
  p4_sw2_long: "disabled",
  p4_sw3_short: "disabled",
  p4_sw3_long: "disabled",
  p4_encoder_press: "page_enter",
  p4_encoder_long: "page_back",
  p4_encoder_cw: "session_next",
  p4_encoder_ccw: "session_previous",
};

const P4_V3_DEFAULT_BUTTON_ACTIONS = {
  p4_sw1_short: "disabled",
  p4_sw1_long: "voice_ptt",
  p4_sw2_short: "disabled",
  p4_sw2_long: "disabled",
  p4_sw3_short: "component_center",
  p4_sw3_long: "disabled",
  p4_encoder_press: "page_enter",
  p4_encoder_long: "page_back",
  p4_encoder_cw: "session_next",
  p4_encoder_ccw: "session_previous",
};

const P4_V4_DEFAULT_BUTTON_ACTIONS = {
  p4_sw1_short: "disabled",
  p4_sw1_long: "voice_ptt",
  p4_sw2_short: "component_center",
  p4_sw2_long: "disabled",
  p4_sw3_short: "page_back",
  p4_sw3_long: "disabled",
  p4_encoder_press: "page_enter",
  p4_encoder_long: "disabled",
  p4_encoder_cw: "session_next",
  p4_encoder_ccw: "session_previous",
};

const ALL_BUTTON_CONTROL_ROWS = [...BOARD_BUTTON_CONTROL_ROWS, ...P4_BUTTON_CONTROL_ROWS];

export function buttonControlRowsForRuntime(runtime) {
  const runtimeId = String(runtime || "").trim().toLowerCase();
  if (runtimeId === P4_RUNTIME_ID) return P4_BUTTON_CONTROL_ROWS;
  if (["linux", "linux-board", "raspberry-pi", "raspberry", "radxa"].includes(runtimeId)) {
    return BOARD_BUTTON_CONTROL_ROWS;
  }
  return [];
}

export function actionOptionById(actionId) {
  return [...BUTTON_FUNCTION_OPTIONS, ...LEGACY_BUTTON_FUNCTION_OPTIONS]
    .find((option) => option.id === actionId) || BUTTON_FUNCTION_OPTIONS[0];
}

export function clampButtonActionValue(value, maxBytes = 159) {
  let output = "";
  for (const char of String(value || "")) {
    if (new TextEncoder().encode(output + char).length > maxBytes) break;
    output += char;
  }
  return output;
}

export function buildBoardButtonConfigBindings(buttonActions = {}, buttonValues = {}, runtime = "", voiceEnabled = false) {
  const controlRows = buttonControlRowsForRuntime(runtime);
  const runtimeId = String(runtime || "").trim().toLowerCase();
  const normalizedButtonActions = runtimeId === P4_RUNTIME_ID
    ? enforceUniqueButtonActions(controlRows, buttonActions)
    : buttonActions;
  const bindings = controlRows.flatMap((row) => {
    const action = row.actionOptions.includes(normalizedButtonActions[row.id])
      ? normalizedButtonActions[row.id]
      : row.defaultAction;
    const binding = { event: row.otaEvent || row.event, action };
    if (action === "agent_prompt" || action === "miniapp_action") {
      binding.value = clampButtonActionValue(buttonValues[row.id] || row.defaultValue || "");
    }
    if (!row.holdEvent) return [binding];
    if (action === "voice_ptt") {
      return [
        { event: row.event, action: "disabled" },
        { event: row.holdEvent, action: voiceEnabled ? "voice_ptt" : "disabled" },
      ];
    }
    return [binding, { event: row.holdEvent, action: "disabled" }];
  });
  return bindings;
}

function createButtonConfigRequestId() {
  return `button-config-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
}

function normalizeVoiceConfig(value = {}) {
  const triggerIds = new Set(VOICE_BUTTON_OPTIONS.map((o) => o.id));
  const trigger = triggerIds.has(value.trigger) ? value.trigger : DEFAULT_VOICE_CONFIG.trigger;
  const incoming = value.buttonActions && typeof value.buttonActions === "object" ? value.buttonActions : {};
  const incomingValues = value.buttonValues && typeof value.buttonValues === "object" ? value.buttonValues : {};
  const incomingLabels = value.buttonLabels && typeof value.buttonLabels === "object" ? value.buttonLabels : {};
  const storedButtonModelVersion = Number(value.buttonModelVersion || 0);
  const migrateLegacyP4Defaults = storedButtonModelVersion !== DEVICE_BUTTON_CONFIG_MODEL_VERSION
    && Object.keys(incoming).some((key) => key.startsWith("p4_"));
  const previousP4Defaults = storedButtonModelVersion === 4
    ? P4_V4_DEFAULT_BUTTON_ACTIONS
    : storedButtonModelVersion === 3
    ? P4_V3_DEFAULT_BUTTON_ACTIONS
    : storedButtonModelVersion === 2
      ? P4_V2_DEFAULT_BUTTON_ACTIONS
      : LEGACY_P4_DEFAULT_BUTTON_ACTIONS;
  const resolvedButtonActions = ALL_BUTTON_CONTROL_ROWS.reduce((next, row) => {
    const incomingAction = incoming[row.id] ?? (row.legacyId ? incoming[row.legacyId] : undefined);
    const migratedAction = incomingAction === "page_main" || incomingAction === "page_app"
      ? "page_toggle"
      : incomingAction;
    const actionAfterModelMigration = migrateLegacyP4Defaults
      && previousP4Defaults[row.id] === migratedAction
      ? undefined
      : migratedAction;
    next[row.id] = row.actionOptions.includes(actionAfterModelMigration)
      ? actionAfterModelMigration
      : DEFAULT_BUTTON_ACTIONS[row.id] || row.defaultAction;
    return next;
  }, {});
  const buttonActions = {
    ...resolvedButtonActions,
    ...enforceUniqueButtonActions(P4_BUTTON_CONTROL_ROWS, resolvedButtonActions),
  };
  const buttonValues = ALL_BUTTON_CONTROL_ROWS.reduce((next, row) => {
    const incomingValue = incomingValues[row.id] ?? (row.legacyId ? incomingValues[row.legacyId] : undefined);
    if (row.supportsValue) next[row.id] = clampButtonActionValue(incomingValue || row.defaultValue || "");
    return next;
  }, {});
  const buttonLabels = ALL_BUTTON_CONTROL_ROWS.reduce((next, row) => {
    const label = incomingLabels[row.id] ?? (row.legacyId ? incomingLabels[row.legacyId] : undefined);
    if (typeof label === "string" && label.trim()) next[row.id] = label.trim().slice(0, 32);
    return next;
  }, {});
  return {
    buttonModelVersion: DEVICE_BUTTON_CONFIG_MODEL_VERSION,
    enabled: value.enabled === true,
    trigger,
    buttonActions,
    buttonValues,
    buttonLabels,
  };
}

export function mergeBoardButtonConfig(currentConfig = {}, response = {}, runtime = "") {
  const boardConfig = response?.config && typeof response.config === "object"
    ? response.config
    : response;
  const bindings = Array.isArray(boardConfig?.bindings) ? boardConfig.bindings : [];
  const bindingsByEvent = new Map(
    bindings
      .filter((binding) => binding && typeof binding === "object")
      .map((binding) => [String(binding.event || "").trim(), binding]),
  );
  const current = normalizeVoiceConfig(currentConfig);
  const buttonActions = { ...current.buttonActions };
  const buttonValues = { ...current.buttonValues };
  const buttonLabels = { ...current.buttonLabels };
  const rows = buttonControlRowsForRuntime(runtime);

  rows.forEach((row) => {
    const primaryBinding = bindingsByEvent.get(row.otaEvent || row.event);
    const holdBinding = row.holdEvent ? bindingsByEvent.get(row.holdEvent) : null;
    const preserveDisabledVoiceMapping = row.voiceTriggerId
      && primaryBinding?.action === "disabled"
      && holdBinding?.action === "disabled"
      && current.buttonActions[row.id] === "voice_ptt";
    const boardBinding = holdBinding?.action === "voice_ptt"
      ? holdBinding
      : preserveDisabledVoiceMapping
        ? { ...primaryBinding, action: "voice_ptt" }
        : primaryBinding;
    const boardAction = String(boardBinding?.action || "").trim();
    const action = row.actionOptions.includes(boardAction) ? boardAction : row.defaultAction;
    const value = clampButtonActionValue(boardBinding?.value || row.defaultValue || "");

    buttonActions[row.id] = action;
    if (row.supportsValue) buttonValues[row.id] = value;
    if (action === "miniapp_action") {
      const cachedValue = String(current.buttonValues[row.id] || "");
      buttonLabels[row.id] = cachedValue === value && current.buttonLabels[row.id]
        ? current.buttonLabels[row.id]
        : value || "当前组件动作";
    } else {
      delete buttonLabels[row.id];
    }
  });

  const voiceRow = rows.find((row) => (
    row.voiceTriggerId && buttonActions[row.id] === "voice_ptt"
  ));
  const runtimeId = String(runtime || "").trim().toLowerCase();
  const p4VoiceBindingEnabled = rows.some((row) => (
    row.voiceTriggerId
    && row.holdEvent
    && bindingsByEvent.get(row.holdEvent)?.action === "voice_ptt"
  ));
  const boardVoiceEnabled = typeof boardConfig?.voiceEnabled === "boolean"
    ? boardConfig.voiceEnabled
    : typeof boardConfig?.voice_enabled === "boolean"
      ? boardConfig.voice_enabled
      : runtimeId === P4_RUNTIME_ID
        ? p4VoiceBindingEnabled
        : Boolean(voiceRow);
  const boardVoiceButton = String(
    boardConfig?.voiceButton || boardConfig?.voice_button || "",
  ).trim();
  const trigger = runtimeId === P4_RUNTIME_ID
    ? voiceRow?.voiceTriggerId
      || (P4_VOICE_BUTTON_OPTIONS.some((option) => option.id === boardVoiceButton)
        ? boardVoiceButton
        : P4_DEFAULT_VOICE_TRIGGER)
    : VOICE_BUTTON_OPTIONS.some((option) => option.id === boardVoiceButton)
    ? boardVoiceButton
    : voiceRow?.voiceTriggerId || current.trigger;

  return normalizeVoiceConfig({
    ...current,
    enabled: boardVoiceEnabled,
    trigger,
    buttonActions,
    buttonValues,
    buttonLabels,
  });
}

export function applyVoiceEnabledForRuntime(value = {}, enabled = false, runtime = "") {
  const next = normalizeVoiceConfig({ ...value, enabled: enabled === true });
  if (String(runtime || "").trim().toLowerCase() === P4_RUNTIME_ID) {
    const voiceRows = P4_BUTTON_CONTROL_ROWS.filter((row) => row.voiceTriggerId);
    const selectedRow = voiceRows.find((row) => next.buttonActions[row.id] === "voice_ptt")
      || voiceRows.find((row) => row.voiceTriggerId === next.trigger)
      || voiceRows[0];
    if (enabled) {
      voiceRows.forEach((row) => {
        next.buttonActions[row.id] = row.id === selectedRow.id
          ? "voice_ptt"
          : next.buttonActions[row.id] === "voice_ptt"
            ? row.voiceFallbackAction || row.defaultAction || "disabled"
            : next.buttonActions[row.id];
      });
    }
    next.trigger = selectedRow?.voiceTriggerId || P4_DEFAULT_VOICE_TRIGGER;
    return next;
  }
  const voiceRows = buttonControlRowsForRuntime(runtime).filter((row) => row.voiceTriggerId);
  if (voiceRows.length === 0) return next;

  if (enabled) {
    const selectedRow = voiceRows.find(
      (row) => next.buttonActions[row.id] === "voice_ptt",
    ) || voiceRows[0];
    voiceRows.forEach((row) => {
      next.buttonActions[row.id] = row.id === selectedRow.id
        ? "voice_ptt"
        : row.voiceFallbackAction || row.defaultAction || "disabled";
    });
    next.trigger = selectedRow.voiceTriggerId;
    next.enabled = true;
    return next;
  }

  voiceRows.forEach((row) => {
    if (next.buttonActions[row.id] === "voice_ptt") {
      next.buttonActions[row.id] = row.voiceFallbackAction || row.defaultAction || "disabled";
    }
  });
  next.enabled = false;
  return next;
}

export function applyVoiceTriggerForRuntime(value = {}, trigger = "", runtime = "") {
  const next = normalizeVoiceConfig(value);
  const voiceRows = buttonControlRowsForRuntime(runtime).filter((row) => row.voiceTriggerId);
  const selectedRow = voiceRows.find((row) => row.voiceTriggerId === trigger)
    || voiceRows.find((row) => row.voiceTriggerId === next.trigger)
    || voiceRows[0];
  if (!selectedRow) return next;

  voiceRows.forEach((row) => {
    if (row.id === selectedRow.id) {
      next.buttonActions[row.id] = "voice_ptt";
    } else if (next.buttonActions[row.id] === "voice_ptt") {
      next.buttonActions[row.id] = row.voiceFallbackAction || row.defaultAction || "disabled";
    }
  });
  next.trigger = selectedRow.voiceTriggerId;
  return next;
}

function loadVoiceConfigFromStorage() {
  try {
    const raw = localStorage.getItem(VOICE_CONFIG_STORAGE_KEY);
    if (raw) return normalizeVoiceConfig(JSON.parse(raw));
  } catch {}
  return normalizeVoiceConfig({});
}

function saveVoiceConfigToStorage(next) {
  try {
    localStorage.setItem(VOICE_CONFIG_STORAGE_KEY, JSON.stringify(normalizeVoiceConfig(next)));
  } catch {}
}

// Single place that OTAs a button config to the board. Used by both the device
// dashboard's manual "下发" button and the component center's install flow, so
// the invoke shape lives in exactly one spot.
export async function dispatchBoardButtonConfig({ boardDeviceId, buttonActions, buttonValues, runtime, voiceButton, voiceEnabled }) {
  const requestId = createButtonConfigRequestId();
  return invoke("button_config_signal", {
    boardDeviceId,
    requestId,
    bindings: buildBoardButtonConfigBindings(buttonActions, buttonValues, runtime, voiceEnabled),
    voiceButton,
    voiceEnabled,
  });
}

// ---------- Voice-bus helpers ----------
const BRIDGE_HTTP_PRIMARY_PORT = 23333;

function normalizeText(value) {
  return typeof value === "string" ? value.trim() : "";
}

async function postMockButtonInject(payload) {
  await ensureBridgeRuntime().catch(() => {});

  const ports = [];
  try {
    const runtime = await invoke("load_bridge_runtime_status");
    const runtimePort = Number(runtime?.port);
    if (Number.isFinite(runtimePort) && runtimePort > 0) ports.push(runtimePort);
  } catch {
    // Older desktop runtimes do not expose this command; use the managed bridge port below.
  }
  ports.push(BRIDGE_HTTP_PRIMARY_PORT);

  let lastError = null;
  for (const port of [...new Set(ports)]) {
    try {
      const url = `http://127.0.0.1:${port}/mock-button-inject`;
      const result = await invoke("http_request_text", {
        url,
        method: "POST",
        headersJson: JSON.stringify({
          "Content-Type": "application/json",
          Accept: "application/json",
        }),
        body: JSON.stringify(payload),
        timeoutMs: 120000,
      });
      const parsed = (() => {
        try {
          return JSON.parse(result?.body || "{}");
        } catch {
          return {};
        }
      })();
      if (!result?.ok || parsed?.ok === false) {
        throw new Error(parsed?.error || `mock-button-inject http ${result?.status || 0}`);
      }
      return parsed;
    } catch (err) {
      lastError = err;
    }
  }
  throw lastError || new Error("mock-button-inject unavailable");
}

// ---------- Voice reducer (lean — only what VoiceAssistantPanel needs) ----------
const VOICE_INITIAL_STATE = {
  busStatus: null,
  busStatusFingerprint: "",
  voiceRuntime: null,
  voiceRuntimeFingerprint: "",
  audioBridgeEnabled: false,
  audioBridgePending: false,
  audioBridgeMessage: "",
  audioBridgeLastResult: null,
  audioBridgeDeferred: false,
  audioBridgeActivity: "idle",
  mockInjectInput: "",
  mockInjectPending: false,
  mockInjectMessage: "",
  mockInjectReply: "",
  mockInjectOk: null,
};

function voiceReducer(state, action) {
  switch (action.type) {
    case "set_bus_status": {
      const fingerprint = JSON.stringify(action.value ?? null);
      if (fingerprint === state.busStatusFingerprint) return state;
      return { ...state, busStatus: action.value, busStatusFingerprint: fingerprint };
    }
    case "set_voice_runtime": {
      const fingerprint = JSON.stringify(action.value ?? null);
      if (fingerprint === state.voiceRuntimeFingerprint) return state;
      return { ...state, voiceRuntime: action.value, voiceRuntimeFingerprint: fingerprint };
    }
    case "set_mock_inject_input": return { ...state, mockInjectInput: action.value || "" };
    case "set_mock_inject_pending":
      return {
        ...state,
        mockInjectPending: Boolean(action.value),
        ...(action.value ? { mockInjectMessage: "", mockInjectReply: "", mockInjectOk: null } : {}),
      };
    case "set_mock_inject_result":
      return {
        ...state,
        mockInjectPending: false,
        mockInjectOk: action.ok,
        mockInjectMessage: action.message || "",
        mockInjectReply: action.reply || "",
      };
    case "set_audio_bridge_pending": return { ...state, audioBridgePending: action.value };
    case "set_audio_bridge_state":
      return {
        ...state,
        audioBridgeEnabled: action.enabled,
        audioBridgePending: false,
        audioBridgeLastResult: action.deferred ? null : action.ok ? "ok" : "error",
        audioBridgeDeferred: action.deferred === true,
        audioBridgeMessage: action.message || "",
      };
    case "set_audio_bridge_activity":
      return {
        ...state,
        audioBridgeActivity: action.phase || "idle",
        audioBridgeLastResult: action.ok === false
          ? "error"
          : action.phase === "end"
            ? "ok"
            : null,
        audioBridgeMessage: action.message || "",
      };
    default: return state;
  }
}

// ---------- Component ----------

export default function DeviceDashboard({ binding, onUnbind, onOpenApiSettings }) {
  const {
    usb,
    deviceOnline,
    onlineBoardDeviceId,
    currentDisplay,
    agentScan,
    refreshAgents,
  } = useDeviceContext();
  const { push } = useToast();

  const [voiceState, voiceDispatch] = useReducer(voiceReducer, VOICE_INITIAL_STATE);
  const onDeviceVoiceAudioActivity = useCallback((activity) => {
    voiceDispatch({ type: "set_audio_bridge_activity", ...activity });
  }, []);
  const { deviceVoiceFlow } = useDeviceVoiceRouter({
    onAudioActivity: onDeviceVoiceAudioActivity,
  });
  const [voiceConfig, setVoiceConfig] = useState(loadVoiceConfigFromStorage);
  const [sessionDisplayEnabled, setSessionDisplayEnabled] = useState(
    loadSessionDisplayEnabled,
  );
  const [voiceConfigDirty, setVoiceConfigDirty] = useState(false);
  const [voiceConfigOtaState, setVoiceConfigOtaState] = useState({ pending: false, tone: "", message: "" });
  const [buttonConfigHydratedFor, setButtonConfigHydratedFor] = useState("");
  const [guideOpen, setGuideOpen] = useState(false);
  const [firmwareModalOpen, setFirmwareModalOpen] = useState(false);
  const [diagnosticsModalOpen, setDiagnosticsModalOpen] = useState(false);
  const [firmwareTargetBoardDeviceId, setFirmwareTargetBoardDeviceId] = useState("");
  const [diagnosticsTargetBoardDeviceId, setDiagnosticsTargetBoardDeviceId] = useState("");
  const [dismissedP4Sessions, setDismissedP4Sessions] = useState({});
  const buttonConfigRevisionRef = useRef(0);
  const buttonConfigQueryTokenRef = useRef(0);
  const isP4Runtime = String(usb.runtime || "").toLowerCase() === P4_RUNTIME_ID;
  const hasKnownRuntime = buttonControlRowsForRuntime(usb.runtime).length > 0;
  const p4VoiceRow = isP4Runtime
    ? P4_BUTTON_CONTROL_ROWS.find((row) => row.voiceTriggerId && voiceConfig.buttonActions[row.id] === "voice_ptt")
    : null;
  const runtimeVoiceEnabled = isP4Runtime
    ? voiceConfig.enabled && Boolean(p4VoiceRow)
    : voiceConfig.enabled;
  const activeVoiceTriggerId = isP4Runtime
    ? p4VoiceRow?.voiceTriggerId || P4_DEFAULT_VOICE_TRIGGER
    : voiceConfig.trigger;
  const runtimeVoiceConfig = useMemo(
    () => isP4Runtime
      ? { ...voiceConfig, enabled: runtimeVoiceEnabled, trigger: activeVoiceTriggerId }
      : voiceConfig,
    [activeVoiceTriggerId, isP4Runtime, runtimeVoiceEnabled, voiceConfig],
  );
  const runtimeVoiceTriggerOptions = isP4Runtime
    ? P4_VOICE_BUTTON_OPTIONS
    : VOICE_BUTTON_OPTIONS.filter((option) => option.id === "encoder_button.hold");
  const selectedVoiceTrigger = runtimeVoiceTriggerOptions.find((option) => option.id === activeVoiceTriggerId)
    || runtimeVoiceTriggerOptions[0];
  const buttonConfigTargetBoardDeviceId = usb.connected
    ? (usb.boardDeviceId || onlineBoardDeviceId || binding?.boardDeviceId || "")
    : "";
  const agentScanSummary = agentScan?.error
    ? "扫描失败，请重试"
    : agentScan?.pending
      ? "正在读取本机 CLI"
      : agentScan?.scannedAt
        ? `已检测 ${agentScan.detectedCount} 个 Agent`
        : "读取本机已安装的 CLI Agent";

  const handleRefreshAgents = useCallback(async () => {
    try {
      const agents = await refreshAgents();
      const detectedCount = agents.filter((agent) => agent.detected).length;
      push({
        tone: "success",
        title: "Agent 扫描完成",
        message: `检测到 ${detectedCount} 个本机 Agent。`,
        ttl: 3200,
      });
    } catch (error) {
      push({
        tone: "error",
        title: "Agent 扫描失败",
        message: error?.message || String(error),
      });
    }
  }, [push, refreshAgents]);

  // When another surface (e.g. the component center installing a widget) writes
  // a new button config to the shared store and fires a "storage" event, reload
  // it so this panel reflects the applied preset instead of going stale.
  useEffect(() => {
    const onStorage = (event) => {
      if (!event.key || event.key === VOICE_CONFIG_STORAGE_KEY) {
        buttonConfigRevisionRef.current += 1;
        setVoiceConfig(loadVoiceConfigFromStorage());
        setVoiceConfigDirty(false);
      }
      if (!event.key || event.key === SESSION_DISPLAY_ENABLED_STORAGE_KEY) {
        setSessionDisplayEnabled(loadSessionDisplayEnabled());
      }
    };
    window.addEventListener("storage", onStorage);
    return () => window.removeEventListener("storage", onStorage);
  }, []);

  const refreshButtonConfigFromBoard = useCallback(async () => {
    if (!usb.connected || !hasKnownRuntime || !buttonConfigTargetBoardDeviceId) return null;
    const queryToken = ++buttonConfigQueryTokenRef.current;
    const startingRevision = buttonConfigRevisionRef.current;
    setVoiceConfigOtaState({
      pending: true,
      tone: "",
      message: "正在从板端读取按钮配置...",
    });
    try {
      const boardConfig = await invoke("usb_get_button_config", {
        expectedBoardDeviceId: buttonConfigTargetBoardDeviceId,
      });
      if (
        queryToken !== buttonConfigQueryTokenRef.current
        || startingRevision !== buttonConfigRevisionRef.current
      ) {
        return null;
      }
      setVoiceConfig((current) => {
        const next = mergeBoardButtonConfig(current, boardConfig, usb.runtime);
        saveVoiceConfigToStorage(next);
        return next;
      });
      setVoiceConfigDirty(false);
      setVoiceConfigOtaState({
        pending: false,
        tone: "success",
        message: "已从板端读取按钮配置并更新客户端缓存。",
      });
      return boardConfig;
    } catch (error) {
      if (queryToken !== buttonConfigQueryTokenRef.current) return null;
      setVoiceConfigOtaState({
        pending: false,
        tone: "warning",
        message: `读取板端按钮配置失败，暂时显示客户端缓存：${error}`,
      });
      return null;
    } finally {
      if (queryToken === buttonConfigQueryTokenRef.current) {
        setButtonConfigHydratedFor(buttonConfigTargetBoardDeviceId);
      }
    }
  }, [
    buttonConfigTargetBoardDeviceId,
    hasKnownRuntime,
    usb.connected,
    usb.runtime,
  ]);

  useEffect(() => {
    if (!usb.connected || !hasKnownRuntime || !buttonConfigTargetBoardDeviceId) {
      buttonConfigQueryTokenRef.current += 1;
      setButtonConfigHydratedFor("");
      return undefined;
    }
    refreshButtonConfigFromBoard();
    return () => {
      buttonConfigQueryTokenRef.current += 1;
    };
  }, [
    buttonConfigTargetBoardDeviceId,
    hasKnownRuntime,
    refreshButtonConfigFromBoard,
    usb.connected,
  ]);

  // Auto-open the device-guide modal the first time the user lands here.
  useEffect(() => {
    if (!binding || !hasKnownRuntime) return;
    if (shouldAutoOpenOnboarding(ONBOARDING_PAGE_IDS.DEVICE)) setGuideOpen(true);
  }, [binding, hasKnownRuntime]);

  const selectedAgentId = currentDisplay.agentId;
  const p4TargetBoardDeviceId = isP4Runtime
    ? (usb.boardDeviceId || onlineBoardDeviceId || binding?.boardDeviceId || "")
    : "";

  const mergeDismissedP4Sessions = useCallback((dismissed) => {
    setDismissedP4Sessions((current) => ({ ...current, ...dismissed }));
  }, []);
  const sessionFeed = useAgentSessionFeed({
    agentId: selectedAgentId,
    displayEnabled: sessionDisplayEnabled,
    dismissedSessions: dismissedP4Sessions,
  });
  const p4SessionSync = useP4SessionSync({
    enabled: isP4Runtime,
    boardDeviceId: p4TargetBoardDeviceId,
    agentId: selectedAgentId,
    usbConnected: usb.connected,
    displayEnabled: sessionDisplayEnabled,
    sessions: sessionFeed.sessions,
    routingSessions: sessionFeed.routingSessions,
    sessionsLoaded: sessionFeed.loaded,
    dismissedSessions: dismissedP4Sessions,
    onDismissSessions: mergeDismissedP4Sessions,
    formatSessionOption: formatVoiceSessionOption,
    push,
  });
  const selectedAgentBusSessions = sessionFeed.sessions;
  const selectedAgentRoutingSessions = sessionFeed.routingSessions;
  const selectedAgentSessionsLoaded = sessionFeed.loaded;

  const onSessionDisplayEnabledChange = useCallback((value) => {
    setSessionDisplayEnabled(saveSessionDisplayEnabled(value));
  }, []);

  // Poll voice-bus status.
  useEffect(() => {
    let cancelled = false;
    let inFlight = false;
    const ctl = new AbortController();
    const run = async () => {
      if (inFlight) return;
      inFlight = true;
      try {
        let body;
        try {
          body = await fetchAgentBusStatus(ctl.signal);
        } catch {
          if (cancelled || ctl.signal.aborted) return;
          const runtime = await ensureBridgeRuntime();
          if (!runtime?.running) {
            throw new Error(runtime?.message || "本地 Bridge 未启动");
          }
          body = await fetchAgentBusStatus(ctl.signal);
        }
        if (!cancelled) voiceDispatch({ type: "set_bus_status", value: body });
      } catch {
        if (!cancelled) {
          voiceDispatch({ type: "set_bus_status", value: { ok: false, agents: [] } });
        }
      } finally {
        inFlight = false;
      }
    };
    run();
    const id = setInterval(run, 5000);
    return () => {
      cancelled = true;
      ctl.abort();
      clearInterval(id);
    };
  }, []);

  // Poll voice-runtime.
  useEffect(() => {
    let cancelled = false;
    let inFlight = false;
    const run = async () => {
      if (cancelled || inFlight) return;
      inFlight = true;
      try {
        const res = isP4Runtime
          ? await invoke("ensure_device_voice_runtime", { input: { interactive: false } })
          : await invoke("ensure_voice_runtime");
        if (cancelled) return;
        voiceDispatch({
          type: "set_voice_runtime",
          value: {
            mode: res?.mode || null,
            message: res?.message || "",
            running: !!res?.running,
            agentId: res?.selectedAgentId || res?.profile?.selectedAgentId || "",
          },
        });
      } catch (err) {
        if (cancelled) return;
        voiceDispatch({ type: "set_voice_runtime", value: { mode: "error", message: String(err), running: false, agentId: "" } });
      } finally {
        inFlight = false;
      }
    };
    run();
    const id = setInterval(run, 5000);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, [isP4Runtime, selectedAgentId]);

  // ---------- Button config update + OTA dispatch ----------
  const updateVoiceConfig = useCallback((patch) => {
    buttonConfigRevisionRef.current += 1;
    setVoiceConfig((prev) => {
      let next = normalizeVoiceConfig({ ...prev, ...patch });
      if (isP4Runtime) {
        const nextVoiceRow = P4_BUTTON_CONTROL_ROWS.find(
          (row) => row.voiceTriggerId && next.buttonActions[row.id] === "voice_ptt",
        );
        next = {
          ...next,
          enabled: next.enabled && Boolean(nextVoiceRow),
          trigger: nextVoiceRow?.voiceTriggerId || P4_DEFAULT_VOICE_TRIGGER,
        };
      }
      saveVoiceConfigToStorage(next);
      setVoiceConfigDirty(true);
      setVoiceConfigOtaState({ pending: false, tone: "warning", message: "已保存到客户端；按钮配置需要通过 USB OTA 下发到板端后才会生效。" });
      return next;
    });
  }, [isP4Runtime]);

  const applyVoiceConfigOverUsb = useCallback(async (configOverride = null) => {
    const requestedConfig = configOverride?.buttonActions
      ? normalizeVoiceConfig(configOverride)
      : voiceConfig;
    const requestedP4VoiceRow = isP4Runtime
      ? P4_BUTTON_CONTROL_ROWS.find(
        (row) => row.voiceTriggerId && requestedConfig.buttonActions[row.id] === "voice_ptt",
      )
      : null;
    const requestedRuntimeVoiceEnabled = isP4Runtime
      ? requestedConfig.enabled && Boolean(requestedP4VoiceRow)
      : requestedConfig.enabled;
    const requestedVoiceTriggerId = isP4Runtime
      ? requestedP4VoiceRow?.voiceTriggerId || P4_DEFAULT_VOICE_TRIGGER
      : requestedConfig.trigger;
    if (!usb.connected) {
      setVoiceConfigOtaState({ pending: false, tone: "warning", message: "需要先通过 USB 连接设备，才能把按钮配置 OTA 到板端。" });
      return;
    }
    const targetBoardDeviceId = onlineBoardDeviceId || usb.boardDeviceId || binding.boardDeviceId;
    if (!targetBoardDeviceId) {
      setVoiceConfigOtaState({ pending: false, tone: "error", message: "未找到可用的板子 ID，请先完成设备绑定。" });
      return;
    }
    setVoiceConfigOtaState({ pending: true, tone: "", message: "正在通过 USB OTA 下发完整按钮配置到板端..." });
    try {
      const ack = await dispatchBoardButtonConfig({
        boardDeviceId: targetBoardDeviceId,
        buttonActions: requestedConfig.buttonActions,
        buttonValues: requestedConfig.buttonValues,
        runtime: usb.runtime,
        voiceButton: requestedVoiceTriggerId,
        voiceEnabled: requestedRuntimeVoiceEnabled,
      });
      let audioTransport = "";
      let voiceSetupWarning = "";
      try {
        if (isP4Runtime && requestedRuntimeVoiceEnabled) {
          setVoiceConfigOtaState({ pending: true, tone: "", message: "按钮配置已写入，正在准备设备麦克风识别通道..." });
          const bridgeRuntime = await ensureBridgeRuntime();
          if (!bridgeRuntime?.running) {
            throw new Error(bridgeRuntime?.message || "本地 Bridge 未启动");
          }
          const voiceRuntime = await invoke("ensure_device_voice_runtime");
          if (!voiceRuntime?.running) {
            throw new Error(voiceRuntime?.message || "设备录音识别通道未就绪");
          }
          const audio = await invoke("audio_bridge_signal", {
            boardDeviceId: targetBoardDeviceId,
            action: "start",
            voiceButton: requestedVoiceTriggerId,
          });
          if (!audio?.usbSent) throw new Error(audio?.usbError || "设备麦克风信令未通过 USB 送达");
          audioTransport = "，设备麦克风已启用";
          voiceDispatch({ type: "set_audio_bridge_state", enabled: true, ok: true, message: "设备麦克风已通过 USB 接入" });
        } else if (voiceState.audioBridgeEnabled) {
          await invoke("audio_bridge_signal", {
            boardDeviceId: targetBoardDeviceId,
            action: "stop",
            voiceButton: requestedVoiceTriggerId,
          });
          voiceDispatch({ type: "set_audio_bridge_state", enabled: false, ok: true, message: "设备麦克风监听已停止" });
        }
      } catch (voiceError) {
        voiceSetupWarning = String(voiceError);
        voiceDispatch({
          type: "set_audio_bridge_state",
          enabled: requestedRuntimeVoiceEnabled ? false : voiceState.audioBridgeEnabled,
          ok: false,
          message: `设备麦克风通道未就绪: ${voiceSetupWarning}`,
        });
      }
      saveVoiceConfigToStorage(requestedConfig);
      setVoiceConfig(requestedConfig);
      const bindingCount = Number(ack?.bindingCount ?? ack?.ack?.bindingCount ?? 0);
      setVoiceConfigDirty(false);
      setVoiceConfigOtaState({
        pending: false,
        tone: voiceSetupWarning ? "warning" : "success",
        message: voiceSetupWarning
          ? `按钮配置已写入板端（${Number.isFinite(bindingCount) ? bindingCount : 0} 项）；语音通道未就绪：${voiceSetupWarning}`
          : `按钮配置已写入板端（${Number.isFinite(bindingCount) ? bindingCount : 0} 项${audioTransport}）。`,
      });
    } catch (err) {
      setVoiceConfigOtaState({ pending: false, tone: "error", message: `按钮配置下发失败: ${err}` });
    }
  }, [binding.boardDeviceId, isP4Runtime, onlineBoardDeviceId, usb.boardDeviceId, usb.connected, usb.runtime, voiceConfig, voiceState.audioBridgeEnabled]);

  const updateVoiceTrigger = useCallback(async (trigger) => {
    buttonConfigRevisionRef.current += 1;
    const next = applyVoiceTriggerForRuntime(voiceConfig, trigger, usb.runtime);
    saveVoiceConfigToStorage(next);
    setVoiceConfig(next);
    setVoiceConfigDirty(true);
    setVoiceConfigOtaState({
      pending: false,
      tone: usb.connected ? "" : "warning",
      message: usb.connected
        ? "正在切换语音输入的长按按钮并同步到设备..."
        : "语音输入按键已保存到客户端；连接 USB 后同步到板端才会生效。",
    });
    if (usb.connected) {
      await applyVoiceConfigOverUsb(next);
    }
  }, [applyVoiceConfigOverUsb, usb.connected, usb.runtime, voiceConfig]);

  const updateVoiceEnabled = useCallback(async (enabled) => {
    buttonConfigRevisionRef.current += 1;
    const next = applyVoiceEnabledForRuntime(voiceConfig, enabled, usb.runtime);
    saveVoiceConfigToStorage(next);
    setVoiceConfig(next);
    setVoiceConfigDirty(true);
    setVoiceConfigOtaState({
      pending: false,
      tone: usb.connected ? "" : "warning",
      message: usb.connected
        ? enabled
          ? "正在启用语音输入并同步到设备..."
          : "正在关闭语音输入并同步到设备..."
        : "已保存到客户端；连接 USB 后同步到板端才会生效。",
    });
    if (usb.connected) {
      await applyVoiceConfigOverUsb(next);
    }
  }, [applyVoiceConfigOverUsb, usb.connected, usb.runtime, voiceConfig]);

  useEffect(() => {
    if (
      !isP4Runtime
      || !usb.connected
      || !p4TargetBoardDeviceId
      || buttonConfigHydratedFor !== p4TargetBoardDeviceId
      || !runtimeVoiceEnabled
    ) return undefined;
    let cancelled = false;

    const rearmDeviceVoice = async () => {
      try {
        const bridgeRuntime = await ensureBridgeRuntime();
        if (cancelled) return;
        if (bridgeRuntime?.running === false) {
          throw new Error(bridgeRuntime?.message || "本地 Bridge 未启动");
        }
        const voiceRuntime = await invoke("ensure_device_voice_runtime", {
          input: { interactive: false },
        });
        if (cancelled) return;
        if (voiceRuntime?.deferred) {
          if (!cancelled) {
            voiceDispatch({
              type: "set_audio_bridge_state",
              enabled: false,
              ok: null,
              deferred: true,
              message: voiceRuntime?.message || "打开语音输入区域后将继续恢复设备麦克风",
            });
          }
          return;
        }
        if (!voiceRuntime?.running) {
          throw new Error(voiceRuntime?.message || "设备录音识别通道未就绪");
        }
        const audio = await invoke("audio_bridge_signal", {
          boardDeviceId: p4TargetBoardDeviceId,
          action: "start",
          voiceButton: activeVoiceTriggerId,
        });
        if (!audio?.usbSent) throw new Error(audio?.usbError || "设备麦克风信令未通过 USB 送达");
        if (!cancelled) {
          voiceDispatch({ type: "set_audio_bridge_state", enabled: true, ok: true, message: "设备麦克风已自动恢复" });
        }
      } catch (error) {
        if (!cancelled) {
          voiceDispatch({ type: "set_audio_bridge_state", enabled: false, ok: false, message: `设备麦克风自动恢复失败: ${error}` });
        }
      }
    };

    rearmDeviceVoice();
    return () => {
      cancelled = true;
    };
  }, [
    activeVoiceTriggerId,
    buttonConfigHydratedFor,
    isP4Runtime,
    p4TargetBoardDeviceId,
    runtimeVoiceEnabled,
    usb.connected,
  ]);

  // ---------- Audio bridge toggle ----------
  const toggleAudioBridge = useCallback(async (action) => {
    const requestedEnabled = action === "start";
    const targetBoardDeviceId = onlineBoardDeviceId || usb.boardDeviceId || binding.boardDeviceId;
    voiceDispatch({ type: "set_audio_bridge_pending", value: true });
    try {
      if (!targetBoardDeviceId) throw new Error("未找到可用的板子 ID，请先完成设备绑定。");
      if (action === "start") {
        const bridgeRuntime = await ensureBridgeRuntime();
        if (bridgeRuntime?.running === false) throw new Error(bridgeRuntime?.message || "本地 Bridge 未启动，无法下发板子音频信令。");
        const voiceRuntime = await invoke(
          isP4Runtime ? "ensure_device_voice_runtime" : "ensure_voice_runtime",
        );
        if (!voiceRuntime?.running) throw new Error(voiceRuntime?.message || "voice-service 未启动，无法接入板子音频。");
      }
      const res = await invoke("audio_bridge_signal", {
        boardDeviceId: targetBoardDeviceId,
        action,
        voiceButton: activeVoiceTriggerId,
      });
      const transports = [res?.usbSent ? "USB" : "", res?.mqttSent ? "MQTT" : ""].filter(Boolean).join(" / ");
      voiceDispatch({ type: "set_audio_bridge_state", enabled: requestedEnabled, ok: true, message: `已通过 ${transports || "USB / MQTT"} 下发到板端` });
    } catch (err) {
      voiceDispatch({ type: "set_audio_bridge_state", enabled: !requestedEnabled, ok: false, message: `${action === "start" ? "启动" : "关闭"}板子音频失败: ${err}` });
    }
  }, [activeVoiceTriggerId, binding.boardDeviceId, isP4Runtime, onlineBoardDeviceId, usb.boardDeviceId]);

  const resumeDeviceVoiceAfterCredentialAccess = useCallback(() => {
    if (!isP4Runtime || !runtimeVoiceEnabled || !usb.connected || !p4TargetBoardDeviceId) {
      return undefined;
    }
    return toggleAudioBridge("start");
  }, [
    isP4Runtime,
    p4TargetBoardDeviceId,
    runtimeVoiceEnabled,
    toggleAudioBridge,
    usb.connected,
  ]);

  const sendMockButtonInject = useCallback(() => {
    const text = (voiceState.mockInjectInput || "").trim();
    if (!text || !selectedAgentId) return;
    voiceDispatch({ type: "set_mock_inject_pending", value: true });
    postMockButtonInject({
      agentId: selectedAgentId,
      sessionId: p4SessionSync.sessionId || "auto",
      text,
    })
      .then((response) => {
        const sessionId =
          response?.sessionId
          || response?.done?.sessionId
          || response?.ready?.sessionId
          || p4SessionSync.sessionId
          || "auto";
        voiceDispatch({
          type: "set_mock_inject_result",
          ok: true,
          message: `已发送到当前会话 · ${sessionId}`,
          reply: response?.tokenPreview || "",
        });
      })
      .catch((err) => {
        voiceDispatch({
          type: "set_mock_inject_result",
          ok: false,
          message: err?.message || String(err),
          reply: "",
        });
      });
  }, [p4SessionSync.sessionId, selectedAgentId, voiceState.mockInjectInput]);

  // ---------- Action-menu callbacks ----------
  const onSendTest = useCallback(() => {
    const sendPromise = usb.connected
      ? invoke("usb_send_speech", { text: "hello from pet-manager" }).then(() => ({ ok: true })).catch((err) => ({ ok: false, error: String(err) }))
      : invoke("send_test_message", { desktopDeviceId: binding.desktopDeviceId, namespace: null, text: null });
    sendPromise.then((res) => {
      push(res.ok
        ? { tone: "success", title: "测试消息已发送" }
        : { tone: "error", title: "测试消息发送失败", message: res.error });
    });
  }, [binding.desktopDeviceId, push, usb.connected]);

  const onCopyDesktopId = useCallback(() => {
    try {
      navigator.clipboard?.writeText(binding.desktopDeviceId || "");
      push({ tone: "success", title: "已复制桌面设备 ID" });
    } catch {
      push({ tone: "error", title: "复制失败" });
    }
  }, [binding.desktopDeviceId, push]);

  const onUnbindClick = useCallback(() => {
    invoke("remove_device_binding", { boardDeviceId: binding.boardDeviceId })
      .then(() => onUnbind?.())
      .catch((err) => push({ tone: "error", title: "解绑失败", message: String(err) }));
  }, [binding.boardDeviceId, onUnbind, push]);

  /* 应急：负一屏物理按键失灵切不回主屏时，远程把设备 .screen-page 写成 main */
  const onDeviceReturnHome = useCallback(async () => {
    try {
      await invoke("usb_set_screen_page", { page: "main" });
      push({ tone: "success", title: "已请求设备切回主屏" });
    } catch (err) {
      push({ tone: "error", title: "切回主屏失败", message: typeof err === "string" ? err : String(err) });
    }
  }, [push]);

  /* 应急：客户端 UI 形象 vs 设备实际脱节时，绕过 desktop-pet-assignment.js 的
   * appearanceChanged 缓存判断，直接重新推送当前 active appearance 到设备 */
  const onForceSyncAppearance = useCallback(async () => {
    const appearanceId =
      (typeof window !== "undefined" && window.localStorage
        ? window.localStorage.getItem(ACTIVE_APPEARANCE_KEY)
        : null) || BUILTIN_TERRIER_APPEARANCE_ID;
    try {
      const result = await invoke("usb_sync_appearance", {
        appearanceId,
        boardDeviceId: p4TargetBoardDeviceId,
      });
      if (result?.ok) {
        push({
          tone: "success",
          title: `已强制重推形象到设备（${appearanceId}）`,
          message: `${result.fileCount || 0} 个素材，${result.byteCount || 0} bytes`,
        });
      } else {
        push({ tone: "error", title: "强制同步失败", message: result?.error || "未知错误" });
      }
    } catch (err) {
      push({ tone: "error", title: "强制同步失败", message: typeof err === "string" ? err : String(err) });
    }
  }, [p4TargetBoardDeviceId, push]);

  const onInputConfigReset = useCallback(async () => {
    buttonConfigRevisionRef.current += 1;
    await refreshButtonConfigFromBoard();
  }, [refreshButtonConfigFromBoard]);

  return (
    <PageShell
      title="桌搭控制台"
      help={hasKnownRuntime ? () => setGuideOpen(true) : undefined}
      actions={
        <DashboardActionsMenu
          onSendTest={onSendTest}
          onCopyDesktopId={onCopyDesktopId}
          onUnbind={onUnbindClick}
          onDeviceReturnHome={onDeviceReturnHome}
          onForceSyncAppearance={onForceSyncAppearance}
          onUpdateFirmware={usb.connected && usb.runtime === P4_RUNTIME_ID
            ? () => {
              setFirmwareTargetBoardDeviceId(usb.boardDeviceId || "");
              setFirmwareModalOpen(true);
            }
            : undefined}
          onDiagnostics={usb.connected && usb.runtime === P4_RUNTIME_ID
            ? () => {
              setDiagnosticsTargetBoardDeviceId(usb.boardDeviceId || "");
              setDiagnosticsModalOpen(true);
            }
            : undefined}
        />
      }
    >
      <Card>
        <DeviceStatusBar />
      </Card>

      <Card
        title="Agent与形象"
        subtitle={agentScanSummary}
        actions={(
          <Button
            variant="ghost"
            size="small"
            loading={agentScan?.pending === true}
            loadingLabel="扫描中…"
            onClick={handleRefreshAgents}
            aria-label="重新扫描本机 CLI Agent"
          >
            <RefreshCw size={14} />
            重新扫描
          </Button>
        )}
      >
        <ChannelMatrixCard
          showSessionDisplaySetting={isP4Runtime}
          sessionDisplayEnabled={sessionDisplayEnabled}
          onSessionDisplayEnabledChange={onSessionDisplayEnabledChange}
        />
      </Card>

      <Card>
        <BoardButtonPanel
          voiceConfig={runtimeVoiceConfig}
          buttonActions={voiceConfig.buttonActions}
          buttonValues={voiceConfig.buttonValues}
          buttonLabels={voiceConfig.buttonLabels}
          runtime={usb.runtime}
          voiceConfigDirty={voiceConfigDirty}
          voiceConfigOtaState={voiceConfigOtaState}
          usbConnected={Boolean(usb.connected)}
          selectedTrigger={selectedVoiceTrigger}
          onVoiceConfigChange={updateVoiceConfig}
          onApplyVoiceConfig={applyVoiceConfigOverUsb}
        />
      </Card>

      <Card.Collapsible
        title="语音输入"
        summary={buildVoiceSummary(runtimeVoiceConfig, selectedVoiceTrigger)}
        defaultOpen
      >
        <VoiceAssistantPanel
          state={{
            ...voiceState,
            deviceVoiceFlow,
            busSessions: selectedAgentBusSessions,
            busRoutingSessions: selectedAgentRoutingSessions,
            busSessionsLoaded: selectedAgentSessionsLoaded,
            busSessionId: p4SessionSync.sessionId,
            selectedAgentId,
            deviceOnline,
          }}
          dispatch={voiceDispatch}
          toggleAudioBridge={toggleAudioBridge}
          sendMockButtonInject={sendMockButtonInject}
          voiceConfig={runtimeVoiceConfig}
          selectedTrigger={selectedVoiceTrigger}
          voiceTriggerOptions={runtimeVoiceTriggerOptions}
          onVoiceConfigChange={updateVoiceConfig}
          onVoiceTriggerChange={updateVoiceTrigger}
          onVoiceEnabledChange={updateVoiceEnabled}
          onCredentialReady={resumeDeviceVoiceAfterCredentialAccess}
          onOpenApiSettings={onOpenApiSettings}
        />
      </Card.Collapsible>

      <DeviceGuideModal
        isOpen={guideOpen}
        onClose={() => setGuideOpen(false)}
        runtime={usb.runtime}
      />
      <FirmwareUpdateModal
        open={firmwareModalOpen}
        onClose={() => {
          setFirmwareModalOpen(false);
          setFirmwareTargetBoardDeviceId("");
        }}
        currentFirmware={usb.firmware}
        expectedBoardDeviceId={firmwareTargetBoardDeviceId}
      />
      <DeviceDiagnosticsModal
        open={diagnosticsModalOpen}
        onClose={() => {
          setDiagnosticsModalOpen(false);
          setDiagnosticsTargetBoardDeviceId("");
        }}
        onInputConfigReset={onInputConfigReset}
        expectedBoardDeviceId={diagnosticsTargetBoardDeviceId}
      />
    </PageShell>
  );
}
