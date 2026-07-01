/**
 * [Input] Wizard actions dispatched by DeviceSetup.jsx.
 * [Output] Pure (no-JSX) onboarding state machine: stepIndex(phase) → progress
 *          index, INITIAL_STATE, and the reducer. No React / Tauri — kept as
 *          plain JS so the binding flow is unit-testable on bare node.
 * [Pos] lib helper for ref/src/DeviceSetup.jsx
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

export function stepIndex(phase) {
  if (phase === "idle") return 0;
  if (phase === "connecting_ap" || phase === "fetching_info" || phase === "scanning_wifi") return 0;
  if (
    phase === "wait_user_input" ||
    phase === "applying_config" ||
    phase === "polling_result" ||
    phase === "ethernet_detecting"
  ) return 1;
  if (phase === "ethernet_binding" || phase === "restoring_wifi") return 2;
  if (phase === "choose_agent_appearance" || phase === "completed") return 3;
  if (phase === "error") return 0;
  return 0;
}

export const INITIAL_STATE = {
  phase: "idle",
  originalSsid: null,
  boardDeviceId: "",
  pairingState: null,
  wifiNetworks: [],
  selectedSsid: "",
  manualSsid: false,
  password: "",
  showPassword: false,
  connectionMode: "wifi",
  desktopDeviceId: "",
  pollCount: 0,
  lastAttempt: null,
  resultIp: "",
  mqttVerified: null, // null = not checked, true = online, false = timeout
  testSent: null, // null = not sent, true = sent ok, false = failed
  testMessage: "",
  agents: [],
  agentScanLoading: false,
  agentScanError: "",
  appearances: [],
  appearanceLoadError: "",
  agentAppearanceDrafts: {},
  savingAgentAppearance: false,
  error: null,
  message: "",
};

export function reducer(state, action) {
  switch (action.type) {
    case "set_phase":
      return { ...state, phase: action.phase, error: null, message: action.message || "" };
    case "set_connection_mode":
      return { ...state, connectionMode: action.value };
    case "set_original_ssid":
      return { ...state, originalSsid: action.ssid };
    case "set_device_info":
      return {
        ...state,
        boardDeviceId: action.boardDeviceId,
        pairingState: action.pairingState,
        desktopDeviceId: action.desktopDeviceId || state.desktopDeviceId,
      };
    case "set_wifi_networks":
      return { ...state, wifiNetworks: action.networks };
    case "set_selected_ssid":
      return { ...state, selectedSsid: action.ssid };
    case "set_manual_ssid":
      return { ...state, manualSsid: action.value };
    case "set_password":
      return { ...state, password: action.value };
    case "toggle_show_password":
      return { ...state, showPassword: !state.showPassword };
    case "set_poll_count":
      return { ...state, pollCount: action.count };
    case "set_last_attempt":
      return { ...state, lastAttempt: action.attempt };
    case "set_mqtt_verified":
      return { ...state, mqttVerified: action.value };
    case "set_test_result":
      return { ...state, testSent: action.ok, testMessage: action.message || "" };
    case "set_agent_scan_loading":
      return { ...state, agentScanLoading: action.value, agentScanError: action.value ? "" : state.agentScanError };
    case "set_agent_setup_data":
      return {
        ...state,
        agentScanLoading: false,
        agentScanError: "",
        appearanceLoadError: action.appearanceLoadError || "",
        agents: action.agents,
        appearances: action.appearances,
        agentAppearanceDrafts: action.agentAppearanceDrafts,
      };
    case "set_agent_scan_error":
      return { ...state, agentScanLoading: false, agentScanError: action.error || "" };
    case "set_agent_appearance_drafts":
      return { ...state, agentAppearanceDrafts: action.value };
    case "set_saving_agent_appearance":
      return { ...state, savingAgentAppearance: action.value };
    case "set_result":
      return {
        ...state,
        phase: "choose_agent_appearance",
        resultIp: action.ip || "",
        lastAttempt: action.attempt,
        connectionMode: action.connectionMode || state.connectionMode,
        error: null,
      };
    case "set_completed":
      return { ...state, phase: "completed", savingAgentAppearance: false, error: null, message: "" };
    case "set_error":
      return { ...state, phase: "error", error: action.error, message: action.message || "" };
    case "reset":
      return { ...INITIAL_STATE };
    default:
      return state;
  }
}
