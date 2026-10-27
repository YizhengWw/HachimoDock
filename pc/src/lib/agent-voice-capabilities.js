export function deviceVoicePlatformWarning(agentId, platform) {
  return agentId === "mimocode" && platform !== "macos"
    ? "MiMoCode 的按键语音输入目前仅支持 macOS 终端；当前系统可使用状态跟随与设备实时对话。"
    : "";
}
export function deviceVoiceTransportReady(agentId, platform, cliReady) {
  // macOS Codex drafts use the native desktop input path. CLI readiness
  // describes a different transport; native permission/window checks run at use.
  return (platform === "macos" && agentId === "codex") || cliReady === true;
}

