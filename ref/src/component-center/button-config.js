/**
 * [Input] Per-component buttons.json bindings and install-time control-label overrides.
 * [Output] Shared screen/SW1-SW3/encoder option catalog, label resolution,
 *          button-config model versioning, and compatibility-only complete-map
 *          and snapshot helpers. Current installs keep bindings package-owned.
 * [Pos] component-center contract helper in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

export const COMPONENT_CONTROL_OPTIONS = [
  {
    label: "屏幕点击",
    shortLabel: "点击",
    control: "屏幕区域",
    event: "screen.region.tap",
    help: "点负一屏触发这个组件动作。",
  },
  {
    label: "屏幕长按",
    shortLabel: "长按",
    control: "屏幕区域",
    event: "screen.region.long_press",
    help: "长按负一屏触发这个组件动作。",
  },
  ...["SW1", "SW2", "SW3"].map((control, index) => {
    const sw = index + 1;
    return {
      label: `${control} 短按`,
      shortLabel: control,
      control,
      event: `button.sw${sw}.short_press`,
      help: `按下设备 ${control} 后触发这个组件动作。`,
    };
  }),
  {
    label: "旋钮短按",
    shortLabel: "旋钮按下",
    control: "前方旋钮",
    event: "button.encoder.short_press",
    help: "短按前方旋钮后触发这个组件动作。",
  },
  {
    label: "旋钮长按",
    shortLabel: "旋钮长按",
    control: "前方旋钮",
    event: "button.encoder.long_press",
    help: "长按前方旋钮后触发这个组件动作。",
  },
  {
    label: "旋钮顺时针",
    shortLabel: "右旋",
    control: "前方旋钮",
    event: "knob.rotate_cw",
    help: "顺时针旋转前方旋钮后触发这个组件动作。",
  },
  {
    label: "旋钮逆时针",
    shortLabel: "左旋",
    control: "前方旋钮",
    event: "knob.rotate_ccw",
    help: "逆时针旋转前方旋钮后触发这个组件动作。",
  },
  {
    label: "旋钮双向旋转",
    shortLabel: "旋转",
    control: "前方旋钮",
    event: "knob.rotate_cw / knob.rotate_ccw",
    help: "向任意方向旋转前方旋钮都触发同一个组件动作。",
  },
];

export const P4_COMPONENT_BUTTON_EVENTS = [
  "button.sw1.short_press",
  "button.sw2.short_press",
  "button.sw3.short_press",
  "button.encoder.short_press",
  "button.encoder.long_press",
  "knob.rotate_cw",
  "knob.rotate_ccw",
];

export const DEVICE_BUTTON_CONFIG_STORAGE_KEY = "pet-manager.board-voice-config";
export const DEVICE_BUTTON_CONFIG_MODEL_VERSION = 4;
export const COMPONENT_SYSTEM_ACTION_PAGE_MAIN = "page_main";

const P4_COMPONENT_DOWNLINK_EVENTS = [
  "button.sw1.short_press",
  "button.sw1.long_press",
  "button.sw1.hold",
  "button.sw2.short_press",
  "button.sw2.long_press",
  "button.sw2.hold",
  "button.sw3.short_press",
  "button.sw3.long_press",
  "button.sw3.hold",
  "button.encoder.short_press",
  "button.encoder.long_press",
  "button.encoder.hold",
  "knob.rotate_cw",
  "knob.rotate_ccw",
];

const P4_COMPONENT_EVENT_ROW_IDS = {
  "button.sw1.short_press": "p4_sw1_short",
  "button.sw2.short_press": "p4_sw2_short",
  "button.sw3.short_press": "p4_sw3_short",
  "button.encoder.short_press": "p4_encoder_press",
  "button.encoder.long_press": "p4_encoder_long",
  "knob.rotate_cw": "p4_encoder_cw",
  "knob.rotate_ccw": "p4_encoder_ccw",
};

export function defaultControlLabelForBinding(binding = {}) {
  const event = String(binding.event || "");
  const control = String(binding.control || "");
  const exactMatch = COMPONENT_CONTROL_OPTIONS.find(
    (option) => option.control === control && option.event === event,
  );
  if (exactMatch) return exactMatch.label;
  const eventMatch = COMPONENT_CONTROL_OPTIONS.find((option) => option.event === event);
  if (eventMatch) return eventMatch.label;
  const controlMatch = COMPONENT_CONTROL_OPTIONS.find((option) => option.control === control);
  return controlMatch?.label || control || COMPONENT_CONTROL_OPTIONS[0].label;
}

export function optionForControlLabel(label) {
  return COMPONENT_CONTROL_OPTIONS.find((option) => option.label === label) || null;
}

export function componentInputEventSlots(event) {
  if (event === "knob.rotate_cw / knob.rotate_ccw") {
    return ["knob.rotate_cw", "knob.rotate_ccw"];
  }
  return event ? [event] : [];
}

export function buildComponentButtonConfigBindings(bindings = []) {
  const desired = new Map();
  for (const binding of bindings) {
    for (const event of componentInputEventSlots(String(binding?.event || "").trim())) {
      if (!P4_COMPONENT_BUTTON_EVENTS.includes(event)) continue;
      desired.set(event, String(binding?.action || "").trim());
    }
  }
  return P4_COMPONENT_DOWNLINK_EVENTS.map((event) => {
    const action = desired.get(event);
    if (!action) return { event, action: "disabled", value: "" };
    if (action === COMPONENT_SYSTEM_ACTION_PAGE_MAIN) {
      return { event, action: COMPONENT_SYSTEM_ACTION_PAGE_MAIN, value: "" };
    }
    return { event, action: "miniapp_action", value: action };
  });
}

export function buildComponentButtonConfigSnapshot(bindings = [], currentConfig = {}) {
  const buttonActions = { ...(currentConfig?.buttonActions || {}) };
  const buttonValues = { ...(currentConfig?.buttonValues || {}) };
  const buttonLabels = { ...(currentConfig?.buttonLabels || {}) };

  for (const rowId of Object.values(P4_COMPONENT_EVENT_ROW_IDS)) {
    buttonActions[rowId] = "disabled";
    delete buttonValues[rowId];
    delete buttonLabels[rowId];
  }

  for (const binding of bindings) {
    const action = String(binding?.action || "").trim();
    if (!action) continue;
    for (const event of componentInputEventSlots(String(binding?.event || "").trim())) {
      const rowId = P4_COMPONENT_EVENT_ROW_IDS[event];
      if (!rowId) continue;
      if (action === COMPONENT_SYSTEM_ACTION_PAGE_MAIN) {
        buttonActions[rowId] = COMPONENT_SYSTEM_ACTION_PAGE_MAIN;
        delete buttonValues[rowId];
        delete buttonLabels[rowId];
      } else {
        buttonActions[rowId] = "miniapp_action";
        buttonValues[rowId] = action;
        buttonLabels[rowId] = String(binding?.label || action).trim() || action;
      }
    }
  }

  return {
    ...currentConfig,
    buttonModelVersion: DEVICE_BUTTON_CONFIG_MODEL_VERSION,
    enabled: false,
    buttonActions,
    buttonValues,
    buttonLabels,
  };
}
