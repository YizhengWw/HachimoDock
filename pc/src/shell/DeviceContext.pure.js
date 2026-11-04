/**
 * [Input] agentAppearanceMap, enabledAgents, appearances, agentOptions, and a verified P4 USB snapshot.
 * [Output] Pure helpers that preserve verified USB runtime/capability/baud metadata, compare normalized snapshots without forcing Context churn, and stay importable in unit tests without pulling in @tauri-apps/api transitively.
 * [Pos] lib node in pc/src/shell
 * [Sync] If this file changes, update `pc/src/shell/.folder.md`.
 */

import {
  activeDesktopAssignment,
  appearanceById,
  channelLabelForId,
} from "../lib/agent-appearance-config.js";

export function deriveCurrentDisplay(agentAppearanceMap, enabledAgents, appearances, agentOptions) {
  const active = activeDesktopAssignment(agentAppearanceMap, enabledAgents);
  return {
    agentId: active.agentId,
    appearance: appearanceById(appearances, active.appearanceId),
    channelLabel: active.agentId ? channelLabelForId(agentOptions, active.agentId) : "",
  };
}

export function resolveUsbRuntime(status) {
  const explicit = String(status?.runtime || "").trim().toLowerCase();
  if (["esp-p4", "esp32-p4", "esp32_p4", "p4"].includes(explicit)) return "esp-p4";
  if (["linux", "linux-board", "raspberry-pi", "raspberry", "radxa"].includes(explicit)) return "linux";
  if (explicit) return explicit;

  const model = String(status?.deviceModel || "").trim().toLowerCase();
  if (model.includes("esp32-p4") || model.includes("esp-p4")) return "esp-p4";

  const capabilities = status?.capabilities && typeof status.capabilities === "object"
    ? status.capabilities
    : null;
  const assetFormats = Array.isArray(capabilities?.assetFormats)
    ? capabilities.assetFormats
    : Array.isArray(capabilities?.appearance?.formats)
      ? capabilities.appearance.formats
      : [];
  const nativeProtocol = String(
    capabilities?.nativeProtocol || capabilities?.transport?.nativeProtocol || "",
  ).toLowerCase();
  if (
    assetFormats.some((format) => String(format).toLowerCase().startsWith("p4-"))
    || nativeProtocol === "pet-usb-native-v1"
  ) {
    return "esp-p4";
  }
  return "";
}

export function normalizeUsbStatusPayload(status) {
  const capabilities = status?.capabilities && typeof status.capabilities === "object"
    ? status.capabilities
    : null;
  // Connection IDs are native u64 values serialized as decimal strings. Never
  // round them through Number, or substitute a local timestamp/generation.
  const rawConnectionId = status?.connectionId;
  const connectionId = status?.connected && typeof rawConnectionId === "string"
    && /^[1-9][0-9]{0,19}$/.test(rawConnectionId)
    && BigInt(rawConnectionId) <= 18446744073709551615n ? rawConnectionId : "";
  const rawBootId = typeof status?.bootId === "string" && status.bootId
    ? status.bootId : capabilities?.debugBootId;
  const bootId = status?.connected && typeof rawBootId === "string"
    && /^[0-9a-f]{16}$/.test(rawBootId) ? rawBootId : "";
  return {
    connectionId,
    bootId,
    connected: !!status?.connected,
    portName: status?.portName || "",
    baudRate: Number(status?.baudRate) || 0,
    boardDeviceId: status?.connected ? status?.boardDeviceId || "" : "",
    runtime: resolveUsbRuntime({ ...status, capabilities }),
    deviceModel: status?.deviceModel || "",
    firmware: status?.firmware || "",
    buildId: status?.buildId || "",
    gitSha: status?.gitSha || "",
    buildDirty: !!status?.buildDirty,
    protocolSchema: Number(status?.protocolSchema) || 0,
    wireProtocol: status?.wireProtocol || "",
    capabilities,
  };
}

export function usbStatusSnapshotsEqual(left, right) {
  if (left === right) return true;
  if (!left || !right) return false;
  return left.connected === right.connected
    && left.portName === right.portName
    && left.baudRate === right.baudRate
    && left.boardDeviceId === right.boardDeviceId
    && left.connectionId === right.connectionId
    && left.bootId === right.bootId
    && left.runtime === right.runtime
    && left.deviceModel === right.deviceModel
    && left.firmware === right.firmware
    && left.buildId === right.buildId
    && left.gitSha === right.gitSha
    && left.buildDirty === right.buildDirty
    && left.protocolSchema === right.protocolSchema
    && left.wireProtocol === right.wireProtocol
    && JSON.stringify(left.capabilities ?? null) === JSON.stringify(right.capabilities ?? null);
}

export function deriveDeviceReachability({ usb }) {
  const deviceOnline = Boolean(usb?.connected);
  const onlineBoardDeviceId = deviceOnline ? String(usb?.boardDeviceId || "").trim() : "";
  return { deviceOnline, onlineBoardDeviceId };
}
