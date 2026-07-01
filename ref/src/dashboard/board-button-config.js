/**
 * [Input] Raw stored voice config + per-button action selections.
 * [Output] Pure board-button / voice-PTT configuration model shared by the device
 *          dashboard orchestrator (DeviceDashboard.jsx) and BoardButtonPanel:
 *          canonical option lists, control rows, normalization, localStorage
 *          load/save and the USB-OTA binding builder. No React / JSX — kept as
 *          plain JS so the logic is unit-testable on bare node.
 * [Pos] lib helper for ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`. Board-side
 *        event names must stay in sync with board_rotary_input.c.
 */

export const VOICE_CONFIG_STORAGE_KEY = "pet-manager.board-voice-config";
export const DEFAULT_VOICE_CONFIG = { enabled: false, trigger: "encoder_button.hold" };

export const DEFAULT_BUTTON_ACTIONS = {
  encoder_button_short: "system_page",
  encoder_button: "system_reset",
  encoder_rotate: "volume_adjust",
};

export const VOICE_BUTTON_OPTIONS = [
  { id: "encoder_button.hold", label: "前方旋钮按压", detail: "启用后按住说话；旋钮旋转仍切屏。", event: "voice.ptt.encoder_button.hold" },
];

export const BUTTON_FUNCTION_OPTIONS = [
  { id: "voice_ptt", label: "语音按住说话", detail: "按住接入语音；同一时间只允许一个硬件按钮作为语音触发。" },
  { id: "system_page", label: "系统切页", detail: "保持 main / stats 页面切换，适合旋钮短按。" },
  { id: "system_reset", label: "系统重置", detail: "保留长按重启或重置配网等板端默认能力。" },
  { id: "volume_adjust", label: "音量调节", detail: "旋钮旋转调节系统总音量，屏幕顶部短暂显示音量条。切页可继续用屏幕滑动。" },
  { id: "disabled", label: "不绑定", detail: "下发 disabled，让新版板端忽略该输入；不会继续触发系统切页或负一屏操作。" },
];

export const BOARD_BUTTON_CONTROL_ROWS = [
  { id: "encoder_button_short", label: "前方旋钮短按", event: "button.encoder.short_press", defaultAction: "system_page", actionOptions: ["system_page", "disabled"] },
  { id: "encoder_button", label: "前方旋钮长按", event: "button.encoder.long_press", voiceTriggerId: "encoder_button.hold", defaultAction: "system_reset", actionOptions: ["voice_ptt", "system_reset", "disabled"] },
  { id: "encoder_rotate", label: "前方旋钮旋转", event: "knob.rotate_cw / knob.rotate_ccw", defaultAction: "volume_adjust", actionOptions: ["volume_adjust"] },
];

export function actionOptionById(actionId) {
  return BUTTON_FUNCTION_OPTIONS.find((option) => option.id === actionId) || BUTTON_FUNCTION_OPTIONS[0];
}

export function buildBoardButtonConfigBindings(buttonActions = {}) {
  return BOARD_BUTTON_CONTROL_ROWS.map((row) => ({
    event: row.otaEvent || row.event,
    action: row.actionOptions.includes(buttonActions[row.id]) ? buttonActions[row.id] : row.defaultAction,
  }));
}

export function createButtonConfigRequestId() {
  return `button-config-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
}

export function normalizeVoiceConfig(value = {}) {
  const triggerIds = new Set(VOICE_BUTTON_OPTIONS.map((o) => o.id));
  const trigger = triggerIds.has(value.trigger) ? value.trigger : DEFAULT_VOICE_CONFIG.trigger;
  const incoming = value.buttonActions && typeof value.buttonActions === "object" ? value.buttonActions : {};
  const buttonActions = BOARD_BUTTON_CONTROL_ROWS.reduce((next, row) => {
    next[row.id] = row.actionOptions.includes(incoming[row.id]) ? incoming[row.id] : DEFAULT_BUTTON_ACTIONS[row.id] || row.defaultAction;
    return next;
  }, {});
  return { enabled: value.enabled === true, trigger, buttonActions };
}

export function loadVoiceConfigFromStorage() {
  try {
    const raw = localStorage.getItem(VOICE_CONFIG_STORAGE_KEY);
    if (raw) return normalizeVoiceConfig(JSON.parse(raw));
  } catch {}
  return normalizeVoiceConfig({});
}

export function saveVoiceConfigToStorage(next) {
  try {
    localStorage.setItem(VOICE_CONFIG_STORAGE_KEY, JSON.stringify(normalizeVoiceConfig(next)));
  } catch {}
}
