/**
 * [Input] Device link status (`usb_get_status`) and the board's `network/status` snapshot.
 * [Output] Link labels, WiFi state text, stable error-code messages and credential validation that
 *          mirror the firmware and Rust rules, so the panel never invents capabilities.
 * [Pos] pure helper node in pc/src/lib; no Tauri or React imports.
 * [Sync] If this file changes, update `pc/src/.folder.md`, `wifi-link.test.js`, and
 *        `firmware/main/pet_p4_net_core.c` (credential rules, error codes).
 */

export const WIFI_SSID_MAX_BYTES = 32;

const utf8Length = (text) => new TextEncoder().encode(String(text ?? "")).length;

/** Same rules as pet_p4_net_credentials_valid and the Rust command: returns "" when acceptable. */
export function validateWifiCredentials(ssid, password) {
  const name = String(ssid ?? "");
  const secret = String(password ?? "");
  const nameBytes = utf8Length(name);
  if (nameBytes < 1 || nameBytes > WIFI_SSID_MAX_BYTES) return "WiFi 名称需为 1–32 字节";
  const bytes = utf8Length(secret);
  const hexKey = secret.length === 64 && /^[0-9a-fA-F]{64}$/.test(secret);
  if (bytes !== 0 && !hexKey && (bytes < 8 || bytes > 63)) {
    return "WiFi 密码需为 8–63 个字符（开放网络可留空）";
  }
  return "";
}

export function isWifiLink(usb) {
  return Boolean(usb?.connected) && usb?.transport === "wifi";
}

export function linkLabel(usb) {
  if (!usb?.connected) return "未连接";
  return isWifiLink(usb) ? "WiFi 无线" : "USB 直连";
}

export function wifiAddressFromPortName(portName) {
  const text = String(portName || "");
  return text.startsWith("wifi://") ? text.slice("wifi://".length) : "";
}

/** Firmware must advertise `capabilities.network.wifi`; provisioning also needs the USB cable. */
export function wifiSupported(usb) {
  return usb?.capabilities?.network?.wifi === true;
}

export function canProvisionWifi(usb) {
  return Boolean(usb?.connected) && usb?.transport !== "wifi" && wifiSupported(usb);
}

const ERROR_TEXT = {
  ap_not_found: "找不到该 WiFi：请确认名称，并注意设备只支持 2.4G / 5G 中路由器已开启的频段",
  wrong_password: "WiFi 密码错误，或路由器加密方式不兼容",
  disconnected: "与路由器的连接已断开，正在重试",
  connect_failed: "无法向路由器发起连接",
  c6_unavailable: "无线协处理器没有响应：请断电重启设备，仍失败时可能需要更新协处理器固件",
  crash_loop: "无线模块连续异常已自动暂停，重新保存 WiFi 即可重试",
  usb_required: "此操作需要 USB 连接",
  invalid_credentials: "设备拒绝了 WiFi 名称或密码",
  invalid_secret: "设备拒绝了配对密钥",
  storage_failed: "设备保存配置失败",
  auth_failed: "配对凭据不匹配，请通过 USB 重新配置 WiFi",
};

export function wifiErrorMessage(code) {
  const key = String(code || "");
  if (!key) return "";
  return ERROR_TEXT[key] || `无线错误：${key}`;
}

const STATE_TEXT = {
  unconfigured: { tone: "muted", text: "尚未配置 WiFi" },
  idle: { tone: "muted", text: "已配置，等待设备启动无线模块" },
  starting: { tone: "pending", text: "正在启动无线模块…" },
  connecting: { tone: "pending", text: "正在连接路由器…" },
  failed: { tone: "error", text: "连接失败" },
};

/** Maps a `network/status` snapshot to a tone + sentence for the panel. */
export function describeWifiStatus(status) {
  if (!status) return { tone: "muted", text: "尚未读取设备 WiFi 状态" };
  if (status.supported === false) {
    return { tone: "warning", text: "当前固件不支持 WiFi，请先升级设备固件" };
  }
  const state = String(status.state || "");
  if (state === "connected") {
    const rssi = Number.isFinite(status.rssi) ? `，信号 ${status.rssi} dBm` : "";
    const client = status.client ? "，电脑已通过 WiFi 连接" : "";
    return { tone: "success", text: `已连接 ${status.ssid || "路由器"}（${status.ip || "获取地址中"}${rssi}）${client}` };
  }
  if (state === "failed") {
    return { tone: "error", text: wifiErrorMessage(status.lastError) || STATE_TEXT.failed.text };
  }
  return STATE_TEXT[state] || { tone: "muted", text: state || "状态未知" };
}

/** What each channel carries, shown next to the controls so the limits are never a surprise. */
export const WIFI_SCOPE_NOTE = "WiFi 同步状态、会话和按键事件；形象、组件、语音与固件更新仍需 USB 连接。";
