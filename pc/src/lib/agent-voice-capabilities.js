export function deviceVoicePlatformWarning(agentId, platform) {
  return agentId === "mimocode" && platform !== "macos"
    ? "MiMoCode 的按键语音输入目前仅支持 macOS 终端；当前系统可使用状态跟随与设备实时对话。"
    : "";
}
