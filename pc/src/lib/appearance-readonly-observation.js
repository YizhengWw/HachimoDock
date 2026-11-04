/** Explicit menu observation only; never sync/follow/activate/reset or clear RawQ. */
import { createDesktopPetIntentId } from "./desktop-pet-assignment.js";

export function appearanceObservationError(error) {
  const code = typeof error === "string" ? error : error?.message || "";
  if (code === "appearance_raw_stream_quarantined_no_probe") return "素材传输保护仍生效，本次未发送设备核对请求。需先完成真实复位证明的恢复流程；不会自动重发旧操作。";
  if (["appearance_recovery_busy", "appearance_recovery_waiting_quiescence"].includes(code)) return "设备操作尚未结束，本次未切换或重发。稍后可再次单独核对状态。";
  return "当前形象状态未能确认；旧同步结果仍未确认。本次未切换或重发。";
}

export async function observeAppearanceFromMenu({ invoke, boardDeviceId, isCurrent = () => true }) {
  const boot = status => status?.bootId || status?.capabilities?.debugBootId || "";
  const valid = status => isCurrent() && status?.connected === true
    && status.boardDeviceId === boardDeviceId && !!status.connectionId && !!boot(status);
  if (!boardDeviceId || !isCurrent()) throw new Error("appearance_recovery_connection_changed");
  const before = await invoke("usb_get_status"); // cached status only, not a probe
  if (!valid(before)) throw new Error("appearance_recovery_connection_changed");
  let result, noPending = false;
  try {
    result = await invoke("usb_reconcile_appearance_unknown", {
      expectedBoardDeviceId: boardDeviceId, userIntentId: createDesktopPetIntentId(),
    });
  } catch (error) {
    if ((typeof error === "string" ? error : error?.message) !== "appearance_recovery_no_pending") throw error;
    noPending = true;
  }
  const after = await invoke("usb_get_status"); // never starts connection or sync
  if (!valid(after) || before.connectionId !== after.connectionId || boot(before) !== boot(after)) {
    throw new Error("appearance_recovery_connection_changed");
  }
  if (!noPending && (result?.status !== "observed_current_state_quiescent"
    || result.oldResult !== "unknown" || result.mutationSent !== false || result.requiresFreshUserIntent !== true
    || ![0, 1].includes(result.activeSlot) || typeof result.activePackId !== "string"
    || !result.activePackId || result.activePackId.length > 64 || /[^\x21-\x7e]/.test(result.activePackId))) {
    throw new Error("appearance_recovery_not_quiescent_or_unproven");
  }
  return {
    title: noPending ? "当前待处理记录已核对" : "当前形象状态已核对",
    message: noPending
      ? "未发现待核对的形象记录；旧同步结果仍未确认。本次未执行同步或跟随切换。"
      : "旧同步结果仍未确认。本次仅核对当前状态，未执行同步或跟随切换。",
  };
}
