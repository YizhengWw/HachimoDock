/**
 * [Input] Current desktop-pet channel map, active appearance, selected appearance, Tauri invoke/listen adapters, verified USB status, and agent labels.
 * [Output] One fresh follow intent first observes old unknown state read-only, then performs one board/connection/boot-bound native appearance operation; native content-cache confirmation owns reuse, unknown results stop without replay, matching USB binding submission precedes profile persistence, and bridge-start failure remains a partial outcome.
 * [Pos] lib node in pc/src/lib
 * [Sync] If this file changes, update `pc/src/.folder.md` and UI callers that set desktop pets.
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
  "切换跟随需要 USB 连接，请连接设备后重试。";
export const APPEARANCE_SYNC_CANCELLED_MESSAGE = "形象素材传输已中断";
export const APPEARANCE_CONFIRMATION_REQUIRED_MESSAGE =
  "暂时无法确认本次切换结果，请核对设备当前状态；请勿重复提交。";
export const APPEARANCE_NOT_SUBMITTED_MESSAGE = "本次切换尚未提交，请检查连接后再试。";
export const FOLLOW_ASSIGNMENT_BUSY_MESSAGE = "已有形象或跟随切换正在处理，请等待本次结果，不要重复提交。";

export function appearanceConfirmationRequired(notSubmitted = false) {
  const error = new Error(notSubmitted ? APPEARANCE_NOT_SUBMITTED_MESSAGE : APPEARANCE_CONFIRMATION_REQUIRED_MESSAGE);
  error.outcome = notSubmitted ? "not_submitted" : "unknown";
  error.code = "appearance-confirmation-required";
  return error;
}

export function createDesktopPetIntentId() {
  const crypto = globalThis.crypto;
  if (typeof crypto?.randomUUID !== "function") {
    throw appearanceConfirmationRequired();
  }
  return crypto.randomUUID();
}

export async function reconcileDesktopPetAppearance({ invoke, boardDeviceId, isCurrent = () => true, usbScope, onProgress,
  wait = (ms) => new Promise(resolve => setTimeout(resolve, ms)),
}) {
  if (!boardDeviceId || !isCurrent()) throw appearanceConfirmationRequired();
  const boot = status => status?.bootId || status?.capabilities?.debugBootId || "";
  let scope = usbScope;
  const started = performance.now();
  const checkScope = async () => {
    if (!isCurrent()) throw appearanceConfirmationRequired();
    const current = await invoke("usb_get_status");
    if (!isCurrent() || !current?.connected || current.boardDeviceId !== boardDeviceId
      || (scope?.connectionId && current.connectionId !== scope.connectionId)
      || (boot(scope) && boot(current) !== boot(scope))) throw appearanceConfirmationRequired();
    scope ||= current;
  };
  // Retry only explicit read-only busy observations, never unknown/malformed
  // results or a mutation. Each query has a fresh nonce and the same USB scope.
  for (let attempt = 0; attempt < 24; attempt += 1) {
  if (attempt || !scope) await checkScope();
  // An observation has its own UUID, distinct from the current follow intent
  // and every old mutation. Never call this as a retry from a mutation catch.
  const userIntentId = createDesktopPetIntentId();
  let result;
  try {
    result = await invoke("usb_reconcile_appearance_unknown", {
      expectedBoardDeviceId: boardDeviceId,
      userIntentId,
    });
  } catch (error) {
    if (!isCurrent()) throw appearanceConfirmationRequired();
    const code = typeof error === "string" ? error : error?.message || "";
    if (code === "appearance_recovery_requires_firmware_update") {
      throw new Error("当前连接未提供安全状态确认能力。若刚完成升级，请等待设备重新连接；仍未恢复时再检查固件版本。");
    }
    if (code === "appearance_raw_stream_quarantined_no_probe") {
      throw new Error("设备连接暂时无法确认，本次未切换。请检查设备连接。");
    }
    if (code === "appearance_recovery_no_pending") {
      return { observed: false, noPending: true, message: "设备已就绪。" };
    }
    if (["appearance_recovery_busy", "appearance_recovery_waiting_quiescence"].includes(code)) {
      await checkScope();
      if (attempt === 23 || performance.now() - started >= 6_000) {
        throw new Error("设备尚未完成当前操作，本次未切换；连接保持后可再次切换。");
      }
      onProgress?.({ stage: "waiting_quiescence", text: "正在等待设备完成当前操作…", indeterminate: true, followOnly: true });
      // Short slices make user cancellation/connection retirement prompt.
      for (let slice = 0; slice < 3; slice += 1) {
        await wait(50);
        if (!isCurrent()) throw appearanceConfirmationRequired();
      }
      continue;
    }
    if (code === "appearance_recovery_connection_changed") {
      throw new Error("设备连接已变化，本次未切换。请重新选择设备后再试。");
    }
    throw appearanceConfirmationRequired();
  }
  if (!isCurrent() || result?.status !== "observed_current_state_quiescent"
    || result.oldResult !== "unknown" || result.mutationSent !== false || result.requiresFreshUserIntent !== true
    || ![0, 1].includes(result.activeSlot) || typeof result.activePackId !== "string"
    || !result.activePackId || result.activePackId.length > 64 || /[^\x21-\x7e]/.test(result.activePackId)) {
    throw appearanceConfirmationRequired();
  }
  return { observed: true, noPending: false, message: "设备已就绪。" };
  }
}

export const APPEARANCE_SYNC_STAGE_TEXT = {
  checking_cache: "正在检查设备是否已缓存该形象…",
  comparing_assets: "正在对比设备已有素材…",
  preparing_slot: "正在准备设备形象存储槽…",
  preparing_storage: "设备正在擦除旧素材、准备存储空间…",
  transferring: "开始通过 USB 传输素材…",
};

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
  followIntentId = createDesktopPetIntentId(),
  onProgress,
  isCurrent = () => true,
}) {
  if (!appearance?.id || !agentId) {
    throw new Error("请先选择要展示的渠道和形象。");
  }

  const nextMap = assignAppearanceToAgent(agentAppearanceMap, agentId, appearance.id);
  const enabledAgents = assignedAgentIds(nextMap, agentId);
  const channelLabel = channelLabelForId(agentOptions, agentId);
  const storedAppearanceId = readActiveAppearanceId();
  const activeAppearanceId = currentAppearanceId || storedAppearanceId;
  // Local IDs choose only explanatory USB-required text. Device content/cache
  // confirmation remains native-owned on every explicit follow request.
  const appearanceChanged = activeAppearanceId !== appearance.id;
  const requireCurrent = () => {
    if (!followIntentId || !isCurrent(followIntentId)) throw appearanceConfirmationRequired();
  };

  let usbStatus = null;
  try {
    usbStatus = await invoke("usb_get_status");
  } catch {
    usbStatus = null;
  }
  const shouldSyncOverUsb = Boolean(usbStatus?.connected);
  const targetBoardDeviceId = boardDeviceId || usbStatus?.boardDeviceId || "";

  if (appearanceChanged && !shouldSyncOverUsb) {
    throw new Error(APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE);
  }

  if (!shouldSyncOverUsb) {
    throw new Error(CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE);
  }
  requireCurrent();
  if (!targetBoardDeviceId || (usbStatus?.boardDeviceId && usbStatus.boardDeviceId !== targetBoardDeviceId)) {
    throw appearanceConfirmationRequired();
  }

  // This explicit follow click owns a NEW intent. First observe any old
  // unresolved transfer without resending it; only then begin this click's
  // normal native sync, which creates its own fresh mutation Attempt UUID.
  onProgress?.({ stage: "confirming", text: "正在确认…", indeterminate: true, followOnly: true });
  await reconcileDesktopPetAppearance({ invoke, boardDeviceId: targetBoardDeviceId, usbScope: usbStatus,
    onProgress, isCurrent: () => isCurrent(followIntentId) });
  requireCurrent();
  const confirmedUsb = await invoke("usb_get_status");
  const bootId = (status) => status?.bootId || status?.capabilities?.debugBootId || "";
  if (confirmedUsb?.connected !== true || confirmedUsb.boardDeviceId !== targetBoardDeviceId
    || (usbStatus.connectionId && confirmedUsb.connectionId !== usbStatus.connectionId)
    || (bootId(usbStatus) && bootId(confirmedUsb) !== bootId(usbStatus))) {
    throw appearanceConfirmationRequired();
  }
  requireCurrent();

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
  // One explicit native operation per user request. Native owns fresh content
  // fingerprint/cache selection and metadata ACK; never force an upload or use a
  // local appearance ID as evidence. Any new unknown stops this intent;
  // neither observation nor mutation is automatically repeated from catch.
  {
    const unlisten = listen
      ? await listen("usb-sync-progress", (event) => {
        if (!isCurrent(followIntentId)) return;
        const progress = event.payload || {};
        if (progress.stage) {
          // Pre-transfer steps have no byte progress; name the step instead
          // of showing a frozen 0%.
          const text = APPEARANCE_SYNC_STAGE_TEXT[progress.stage];
          if (text) onProgress?.({ type: "info", stage: progress.stage, text, percent: 0, indeterminate: true });
          return;
        }
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
      requireCurrent();
      const result = await invoke("usb_sync_appearance", {
        appearanceId: appearance.id,
        boardDeviceId: targetBoardDeviceId,
      });
      if (result?.ok !== true) {
        throw new Error(result?.error || "同步失败");
      }
      notice = result?.reusedSlot
        ? `已确认「${appearance.name}」从设备缓存切换，并提交跟随 ${channelLabel}，无需重新传输素材。`
        : `已确认「${appearance.name}」素材并提交跟随 ${channelLabel}，USB 同步 ${result?.fileCount || 0} 个素材 (${formatBytes(result?.byteCount || 0)})`;
    } catch (err) {
      const detail = err?.message || String(err);
      const unknown = detail.startsWith("APPEARANCE_RESULT_UNKNOWN:");
      const notSubmitted = detail.startsWith("APPEARANCE_NOT_SUBMITTED:");
      if (!unknown && detail.includes(APPEARANCE_SYNC_CANCELLED_MESSAGE)) {
        throw new Error(APPEARANCE_SYNC_CANCELLED_MESSAGE);
      }
      if (notSubmitted && /^APPEARANCE_NOT_SUBMITTED:\s*P4 appearance videos require \d{1,12} bytes but the custom slot only has \d{1,12} bytes$/.test(detail)) {
        const error = new Error("形象视频超出设备可用空间，尚未传输。请确认设备分区和形象素材大小。");
        error.code = "appearance-capacity-exceeded";
        error.outcome = "not_submitted";
        throw error;
      }
      // The durable unknown latch may predate this click. Do not claim this
      // request uploaded anything, expose native codes, or infer old image state.
      throw appearanceConfirmationRequired(notSubmitted);
    } finally {
      try { unlisten(); } catch { /* listener cleanup must not replace the device result */ }
    }
  }

  requireCurrent();
  // One explicit USB submission. Profile/runtime changes must not race ahead of
  // the device step. No rollback or replay after an uncertain write result.
  let dispatched;
  try {
    dispatched = await invoke("dispatch_remote_cli_binding", {
      input: {
        boardDeviceId: targetBoardDeviceId,
        expectedConnectionId: confirmedUsb.connectionId,
        expectedBootId: bootId(confirmedUsb),
        targetDeviceId: profile.desktopDeviceId,
        targetSource: agentId,
        previousSource,
        mqttNamespace: profile.mqttNamespace,
      },
    });
  } catch {
    throw new Error("本次切换结果尚未确认，请检查连接后再试。");
  }
  if (dispatched?.ok !== true || dispatched?.usbSent !== true
    || dispatched.targetSource !== agentId || dispatched.targetDeviceId !== profile.desktopDeviceId
    || dispatched.boardDeviceId !== targetBoardDeviceId) {
    throw new Error("本次切换结果尚未确认，请检查连接后再试。");
  }
  requireCurrent();

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
  }

  saveAgentAppearanceMap(nextMap);
  saveEnabledAgents(new Set(enabledAgents));
  try {
    localStorage.setItem(ACTIVE_APPEARANCE_KEY, appearance.id);
  } catch {}

  if (followChanged) {
    try {
      await invoke("ensure_bridge_runtime", { input: { forceRestart: true } });
    } catch {
      const error = new Error("USB 跟随指令已提交且配置已保存，但本地桥接服务未就绪；请检查桥接状态，不要重复发送设备指令。");
      error.committedAssignment = { nextMap, enabledAgents, agentId };
      throw error;
    }
  }

  return { nextMap, notice };
}

function readActiveAppearanceId() {
  try {
    return localStorage.getItem(ACTIVE_APPEARANCE_KEY) || "";
  } catch {
    return "";
  }
}
