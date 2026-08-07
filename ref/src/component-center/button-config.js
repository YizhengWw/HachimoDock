/**
 * [Input] Per-component buttons.json bindings and install-time control-label overrides.
 * [Output] Shared screen/SW1-SW3/joystick option catalog, label resolution,
 *          exact shipped-v7 board-default migration, authoritative optional
 *          global-exit event resolution, and button-config model-v9 signaling.
 *          Current installs keep bindings package-owned.
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
    label: "摇杆中按短按",
    shortLabel: "中按",
    control: "前方摇杆",
    event: "button.encoder.short_press",
    help: "短按摇杆中键后触发这个组件动作；旧旋钮组件包继续兼容。",
  },
  {
    label: "摇杆中按长按",
    shortLabel: "中按长按",
    control: "前方摇杆",
    event: "button.encoder.long_press",
    help: "长按摇杆中键后触发这个组件动作；旧旋钮组件包继续兼容。",
  },
  {
    label: "摇杆向上",
    shortLabel: "向上",
    control: "前方摇杆",
    event: "joystick.up",
    help: "向上推动摇杆后触发这个组件动作。",
  },
  {
    label: "摇杆向下",
    shortLabel: "向下",
    control: "前方摇杆",
    event: "joystick.down",
    help: "向下推动摇杆后触发这个组件动作。",
  },
  {
    label: "摇杆向右",
    shortLabel: "向右",
    control: "前方摇杆",
    event: "knob.rotate_cw",
    help: "向右推动摇杆后触发这个组件动作；旧包中的顺时针事件继续兼容。",
  },
  {
    label: "摇杆向左",
    shortLabel: "向左",
    control: "前方摇杆",
    event: "knob.rotate_ccw",
    help: "向左推动摇杆后触发这个组件动作；旧包中的逆时针事件继续兼容。",
  },
  {
    label: "摇杆左右方向",
    shortLabel: "左右",
    control: "前方摇杆",
    event: "knob.rotate_cw / knob.rotate_ccw",
    help: "向左或向右推动摇杆都触发同一个组件动作；旧旋钮包继续兼容。",
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
  "joystick.up",
  "joystick.down",
];

const P4_GLOBAL_BUTTON_EVENTS = new Set([
  ...P4_COMPONENT_BUTTON_EVENTS,
  "button.sw1.long_press",
  "button.sw1.hold",
  "button.sw2.long_press",
  "button.sw2.hold",
  "button.sw3.long_press",
  "button.sw3.hold",
  "button.encoder.hold",
]);

const GLOBAL_EXIT_CONTROL_LABELS = {
  "button.sw1.long_press": "SW1 长按",
  "button.sw1.hold": "SW1 按住",
  "button.sw2.long_press": "SW2 长按",
  "button.sw2.hold": "SW2 按住",
  "button.sw3.long_press": "SW3 长按",
  "button.sw3.hold": "SW3 按住",
  "button.encoder.hold": "中键按住",
};

export const DEVICE_BUTTON_CONFIG_STORAGE_KEY = "pet-manager.board-voice-config";
export const DEVICE_BUTTON_CONFIG_MODEL_VERSION = 9;

export function resolveGlobalExitEvents(response = {}) {
  const boardConfig = response?.config && typeof response.config === "object"
    ? response.config
    : response;
  const bindings = Array.isArray(boardConfig?.bindings) ? boardConfig.bindings : [];
  return [...new Set(bindings
    .filter((binding) => String(binding?.action || "").trim() === "page_back")
    .map((binding) => String(binding?.event || "").trim())
    .filter((event) => P4_GLOBAL_BUTTON_EVENTS.has(event)))];
}

export function globalExitControlLabel(event) {
  const normalized = String(event || "").trim();
  return COMPONENT_CONTROL_OPTIONS.find((option) => option.event === normalized)?.shortLabel
    || GLOBAL_EXIT_CONTROL_LABELS[normalized]
    || normalized;
}

const P4_V7_SHIPPED_DEFAULT_BINDINGS = {
  "button.sw1.short_press": "page_back",
  "button.sw1.long_press": "disabled",
  "button.sw1.hold": "voice_ptt",
  "button.sw2.short_press": "component_center",
  "button.sw2.long_press": "disabled",
  "button.sw2.hold": "disabled",
  "button.sw3.short_press": "page_enter",
  "button.sw3.long_press": "disabled",
  "button.sw3.hold": "disabled",
  "button.encoder.short_press": "page_enter",
  "button.encoder.long_press": "disabled",
  "button.encoder.hold": "disabled",
  "knob.rotate_cw": "session_next",
  "knob.rotate_ccw": "session_previous",
  "joystick.up": "disabled",
  "joystick.down": "disabled",
};

export function migrateP4V7ShippedBoardDefaults(response = {}, runtime = "") {
  if (String(runtime || "").trim().toLowerCase() !== "esp-p4") {
    return { response, migrated: false };
  }
  const wrapped = response?.config && typeof response.config === "object";
  const boardConfig = wrapped ? response.config : response;
  const bindings = Array.isArray(boardConfig?.bindings) ? boardConfig.bindings : [];
  const actionsByEvent = new Map(bindings.map((binding) => [
    String(binding?.event || "").trim(),
    String(binding?.action || "").trim(),
  ]));
  const matchesShippedV7Defaults = Object.entries(P4_V7_SHIPPED_DEFAULT_BINDINGS)
    .every(([event, action]) => actionsByEvent.get(event) === action);
  if (!matchesShippedV7Defaults) return { response, migrated: false };

  const migratedBindings = bindings.map((binding) => {
    if (binding?.event === "button.sw1.short_press") {
      return { ...binding, action: "page_enter", value: "" };
    }
    if (binding?.event === "button.sw3.short_press") {
      return { ...binding, action: "page_back", value: "" };
    }
    return binding;
  });
  const migratedConfig = {
    ...boardConfig,
    version: DEVICE_BUTTON_CONFIG_MODEL_VERSION,
    bindings: migratedBindings,
  };
  return {
    response: wrapped ? { ...response, config: migratedConfig } : migratedConfig,
    migrated: true,
  };
}

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
