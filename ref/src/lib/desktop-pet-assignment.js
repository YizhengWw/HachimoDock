/**
 * [Input] Current desktop-pet channel map, active appearance, selected appearance, Tauri invoke/listen adapters, device reachability, and agent labels.
 * [Output] Shared "set as desktop pet" workflow that syncs appearance assets only when the selected appearance changes, explains USB sync failures before cancelling real appearance changes, skips asset re-pushes for pure follow-channel switches, clears the previous followed source, requires USB for appearance changes, updates device follow-source binding, and keeps one active channel.
 * [Pos] lib node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md` and UI callers that set desktop pets.
 */

import {
  assignAppearanceToAgent,
  assignedAgentIds,
  channelLabelForId,
  saveAgentAppearanceMap,
  saveEnabledAgents,
} from "./agent-appearance-config.js";

export const ACTIVE_APPEARANCE_KEY = "pet-manager:active-appearance-id";
export const APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE =
  "当前渠道配置的形象与设备端当前形象不一致，请先连接 USB 线后再切换渠道和形象。";
export const CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE =
  "请先连接设备（USB 直连或设备在线）后再切换设备跟随渠道。";

export function formatBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

export async function applyDesktopPetAssignment({
  invoke,
  listen,
  agentAppearanceMap,
  agentId,
  appearance,
  agentOptions,
  boardDeviceId,
  currentAppearanceId = "",
  deviceOnline = false,
  onProgress,
}) {
  if (!appearance?.id || !agentId) {
    throw new Error("请先选择要展示的渠道和形象。");
  }

  const nextMap = assignAppearanceToAgent(agentAppearanceMap, agentId, appearance.id);
  const enabledAgents = assignedAgentIds(nextMap, agentId);
  const channelLabel = channelLabelForId(agentOptions, agentId);
  const storedAppearanceId = readActiveAppearanceId();
  const activeAppearanceId = currentAppearanceId || storedAppearanceId;
  /* `appearanceChanged` 只反映**客户端本地缓存** vs 目标的差异。它不能保证
   * 设备端真的就是 activeAppearanceId——前一次切换可能 OTA 失败、设备被
   * 别的客户端动过、设备被重启过 (`.desktop-pet-current` 丢失)，都会让
   * localStorage 和设备实际状态脱节。真实换形象时必须 USB sync 成功后才
   * 保存跟随渠道；纯跟随渠道切换且形象 ID 一致时不重推素材，避免把
   * 跟随切换卡在不必要的 OTA ACK 上。 */
  const appearanceChanged = activeAppearanceId !== appearance.id;

  let usbStatus = null;
  try {
    usbStatus = await invoke("usb_get_status");
  } catch {
    usbStatus = null;
  }
  const shouldSyncOverUsb = Boolean(usbStatus?.connected);

  if (appearanceChanged && !shouldSyncOverUsb) {
    throw new Error(APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE);
  }

  if (!appearanceChanged && !shouldSyncOverUsb && !deviceOnline) {
    throw new Error(CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE);
  }

  const profile = await invoke("load_bridge_profile");
  const profileEnabledAgents = Array.isArray(profile.enabledAgents)
    ? profile.enabledAgents.filter(Boolean)
    : [];
  const selectedAgentId = profile.selectedAgentId || profileEnabledAgents[0] || "";
  const previousSource = profileEnabledAgents.find((id) => id && id !== agentId)
    || (selectedAgentId && selectedAgentId !== agentId ? selectedAgentId : "");
  const followChanged =
    selectedAgentId !== agentId ||
    !profileEnabledAgents.includes(agentId) ||
    profileEnabledAgents.some((id) => id && id !== agentId);
  let notice = appearanceChanged
    ? `已将「${appearance.name}」设为 ${channelLabel} 渠道桌宠。`
    : `已切换设备跟随主体为 ${channelLabel}，沿用「${appearance.name}」，无需重新传输素材。`;
  /* 只有真正换形象才下发素材；纯跟随渠道切换且形象一致时不重推动画。 */
  if (appearanceChanged && shouldSyncOverUsb) {
    const unlisten = listen
      ? await listen("usb-sync-progress", (event) => {
        const progress = event.payload || {};
        const currentFile = Number(progress.currentFile || 0);
        const totalFiles = Number(progress.totalFiles || 0);
        const bytesSent = Number(progress.bytesSent || 0);
        const bytesTotal = Number(progress.bytesTotal || 0);
        const percent = bytesTotal > 0
          ? Math.round((bytesSent / bytesTotal) * 100)
          : 0;
        onProgress?.({
          type: "info",
          text: `USB 传输中… ${currentFile}/${totalFiles} 个素材 (${formatBytes(bytesSent)}/${formatBytes(bytesTotal)}) ${percent}%`,
          currentFile,
          totalFiles,
          bytesSent,
          bytesTotal,
          percent,
        });
      })
      : () => {};
    try {
      const result = await invoke("usb_sync_appearance", { appearanceId: appearance.id });
      if (!result?.ok) {
        throw new Error(result?.error || "同步失败");
      }
      notice = `已将「${appearance.name}」设为 ${channelLabel} 渠道桌宠，并通过 USB 同步 ${result?.fileCount || 0} 个素材 (${formatBytes(result?.byteCount || 0)})`;
    } catch (err) {
      const detail = err?.message || String(err);
      throw new Error(`形象素材下发失败，已取消切换跟随；设备仍保持原跟随主体。原始错误：${detail}`);
    } finally {
      unlisten();
    }
  }

  if (followChanged) {
    await invoke("save_bridge_profile", {
      input: {
        desktopDeviceId: profile.desktopDeviceId,
        mqttUrl: profile.mqttUrl,
        mqttNamespace: profile.mqttNamespace,
        mqttUsername: profile.mqttUsername,
        mqttPassword: profile.mqttPassword,
        transport: profile.transport,
        serialPort: profile.serialPort,
        serialBaud: profile.serialBaud,
        petChannelId: profile.petChannelId,
        enabledAgents,
        selectedAgentId: agentId,
      },
    });
    await invoke("ensure_bridge_runtime", { input: { forceRestart: true } });

    await invoke("dispatch_remote_cli_binding", {
      input: {
        boardDeviceId: boardDeviceId || "",
        targetDeviceId: profile.desktopDeviceId,
        targetSource: agentId,
        previousSource,
        mqttNamespace: profile.mqttNamespace,
      },
    });
  }

  saveAgentAppearanceMap(nextMap);
  saveEnabledAgents(new Set(enabledAgents));
  try {
    localStorage.setItem(ACTIVE_APPEARANCE_KEY, appearance.id);
  } catch {}

  return { nextMap, notice };
}

function readActiveAppearanceId() {
  try {
    return localStorage.getItem(ACTIVE_APPEARANCE_KEY) || "";
  } catch {
    return "";
  }
}
