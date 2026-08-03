/**
 * [Input] Shared desktop-pet assignment workflow with mocked Tauri commands and localStorage.
 * [Output] Node regression coverage that setting a desktop pet preserves per-channel shapes, syncs assets to the exact board only for real appearance changes, reports P4 slot-cache reuse, skips unchanged-appearance follow-switch re-pushes, requires USB for every follow change, idempotently dispatches the USB device binding even after profile persistence, and saves exactly one active channel.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { AGENT_APPEARANCE_MAP_STORAGE_KEY, ENABLED_AGENTS_STORAGE_KEY } from "./agent-appearance-config.js";
import {
  ACTIVE_APPEARANCE_KEY,
  APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE,
  APPEARANCE_SYNC_CANCELLED_MESSAGE,
  CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE,
  applyDesktopPetAssignment,
} from "./desktop-pet-assignment.js";

function installStorage() {
  const values = new Map();
  globalThis.localStorage = {
    getItem: (key) => values.get(key) || null,
    setItem: (key, value) => values.set(key, String(value)),
    removeItem: (key) => values.delete(key),
    clear: () => values.clear(),
  };
  return values;
}

test("applyDesktopPetAssignment saves one desktop channel and mirrors it to bridge selection", async () => {
  const storage = installStorage();
  const calls = [];
  const invoke = async (command, args) => {
    calls.push({ command, args });
    if (command === "load_bridge_profile") {
      return {
        desktopDeviceId: "desk-1",
        mqttUrl: "mqtt://example",
        mqttNamespace: "pet",
        mqttUsername: "u",
        mqttPassword: "p",
        transport: "mqtt",
        serialPort: "",
        serialBaud: 115200,
        petChannelId: "pet",
        enabledAgents: ["codex"],
      };
    }
    if (command === "ensure_bridge_runtime") return { running: true };
    if (command === "usb_get_status") return { connected: true };
    if (command === "usb_sync_appearance") return { ok: true, fileCount: 16, byteCount: 4096 };
    if (command === "dispatch_remote_cli_binding") return { ok: true };
    return {};
  };

  const result = await applyDesktopPetAssignment({
    invoke,
    listen: null,
    agentAppearanceMap: { codex: "old-avatar" },
    agentId: "claude-code",
    appearance: { id: "new-avatar", name: "新形象" },
    agentOptions: [{ id: "claude-code", label: "Claude Code" }],
    boardDeviceId: "board-1",
    currentAppearanceId: "old-avatar",
  });

  assert.deepEqual(result.nextMap, { codex: "old-avatar", "claude-code": "new-avatar" });
  assert.deepEqual(JSON.parse(storage.get(AGENT_APPEARANCE_MAP_STORAGE_KEY)), {
    codex: "old-avatar",
    "claude-code": "new-avatar",
  });
  assert.deepEqual(JSON.parse(storage.get(ENABLED_AGENTS_STORAGE_KEY)), ["claude-code"]);
  assert.equal(storage.get(ACTIVE_APPEARANCE_KEY), "new-avatar");

  const profileCall = calls.find((call) => call.command === "save_bridge_profile");
  assert.deepEqual(profileCall.args.input.enabledAgents, ["claude-code"]);
  assert.equal(profileCall.args.input.selectedAgentId, "claude-code");
  assert.deepEqual(
    calls.find((call) => call.command === "usb_sync_appearance").args,
    { appearanceId: "new-avatar", boardDeviceId: "board-1" },
  );
  assert.deepEqual(
    calls.find((call) => call.command === "dispatch_remote_cli_binding").args.input,
    {
      boardDeviceId: "board-1",
      targetDeviceId: "desk-1",
      targetSource: "claude-code",
      previousSource: "codex",
      mqttNamespace: "pet",
    },
  );
});

test("applyDesktopPetAssignment refuses an unchanged-appearance follow switch without USB", async () => {
  const storage = installStorage();
  const calls = [];
  const invoke = async (command, args) => {
    calls.push({ command, args });
    if (command === "load_bridge_profile") {
      return {
        desktopDeviceId: "desk-1",
        mqttUrl: "mqtt://example",
        mqttNamespace: "pet",
        mqttUsername: "u",
        mqttPassword: "p",
        transport: "mqtt",
        serialPort: "",
        serialBaud: 115200,
        petChannelId: "pet",
        enabledAgents: ["codex"],
      };
    }
    if (command === "usb_get_status") return { connected: false };
    return {};
  };

  await assert.rejects(
    () => applyDesktopPetAssignment({
      invoke,
      listen: null,
      agentAppearanceMap: { codex: "same-avatar", "claude-code": "same-avatar" },
      agentId: "claude-code",
      appearance: { id: "same-avatar", name: "同一形象" },
      agentOptions: [{ id: "claude-code", label: "Claude Code" }],
      boardDeviceId: "board-1",
      currentAppearanceId: "same-avatar",
    }),
    new RegExp(CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE),
  );

  assert.equal(calls.some((call) => call.command === "usb_sync_appearance"), false);
  assert.equal(calls.some((call) => call.command === "save_bridge_profile"), false);
  assert.equal(calls.some((call) => call.command === "dispatch_remote_cli_binding"), false);
  assert.equal(storage.get(ENABLED_AGENTS_STORAGE_KEY), undefined);
});

test("applyDesktopPetAssignment does not re-sync unchanged active appearance when USB is connected", async () => {
  installStorage();
  const calls = [];
  const invoke = async (command, args) => {
    calls.push({ command, args });
    if (command === "load_bridge_profile") {
      return {
        desktopDeviceId: "desk-1",
        mqttUrl: "mqtt://example",
        mqttNamespace: "pet",
        mqttUsername: "u",
        mqttPassword: "p",
        transport: "mqtt",
        serialPort: "",
        serialBaud: 115200,
        petChannelId: "pet",
        enabledAgents: ["codex"],
      };
    }
    if (command === "ensure_bridge_runtime") return { running: true };
    if (command === "usb_get_status") return { connected: true };
    if (command === "usb_sync_appearance") throw new Error("unchanged appearance should not sync");
    if (command === "dispatch_remote_cli_binding") return { ok: true };
    return {};
  };

  const result = await applyDesktopPetAssignment({
    invoke,
    listen: null,
    agentAppearanceMap: { codex: "same-avatar" },
    agentId: "codex",
    appearance: { id: "same-avatar", name: "同一形象" },
    agentOptions: [{ id: "codex", label: "Codex" }],
    boardDeviceId: "board-1",
    currentAppearanceId: "same-avatar",
  });

  assert.equal(calls.some((call) => call.command === "usb_sync_appearance"), false);
  assert.equal(calls.some((call) => call.command === "dispatch_remote_cli_binding"), true);
  assert.match(result.notice, /无需重新传输素材/);
});

test("applyDesktopPetAssignment switches follow channel without syncing unchanged appearance", async () => {
  const storage = installStorage();
  const calls = [];
  const invoke = async (command, args) => {
    calls.push({ command, args });
    if (command === "load_bridge_profile") {
      return {
        desktopDeviceId: "desk-1",
        mqttUrl: "mqtt://example",
        mqttNamespace: "pet",
        mqttUsername: "u",
        mqttPassword: "p",
        transport: "mqtt",
        serialPort: "",
        serialBaud: 115200,
        petChannelId: "pet",
        enabledAgents: ["codex"],
        selectedAgentId: "codex",
      };
    }
    if (command === "ensure_bridge_runtime") return { running: true };
    if (command === "usb_get_status") return { connected: true };
    if (command === "usb_sync_appearance") throw new Error("unchanged appearance should not sync");
    if (command === "dispatch_remote_cli_binding") return { ok: true };
    return {};
  };

  const result = await applyDesktopPetAssignment({
    invoke,
    listen: null,
    agentAppearanceMap: { codex: "same-avatar", "claude-code": "same-avatar" },
    agentId: "claude-code",
    appearance: { id: "same-avatar", name: "同一形象" },
    agentOptions: [{ id: "claude-code", label: "Claude Code" }],
    boardDeviceId: "board-1",
    currentAppearanceId: "same-avatar",
  });

  assert.match(result.notice, /已切换设备跟随主体为 Claude Code/);
  assert.doesNotMatch(result.notice, /形象素材重推未完成/);
  assert.equal(calls.some((call) => call.command === "usb_sync_appearance"), false);
  assert.ok(calls.some((call) => call.command === "save_bridge_profile"));
  assert.ok(calls.some((call) => call.command === "dispatch_remote_cli_binding"));
  assert.deepEqual(JSON.parse(storage.get(ENABLED_AGENTS_STORAGE_KEY)), ["claude-code"]);
  assert.equal(storage.get(ACTIVE_APPEARANCE_KEY), "same-avatar");
});

test("applyDesktopPetAssignment forwards numeric USB sync progress", async () => {
  installStorage();
  const progressEvents = [];
  const invoke = async (command) => {
    if (command === "load_bridge_profile") {
      return {
        desktopDeviceId: "desk-1",
        mqttUrl: "mqtt://example",
        mqttNamespace: "pet",
        mqttUsername: "u",
        mqttPassword: "p",
        transport: "mqtt",
        serialPort: "",
        serialBaud: 115200,
        petChannelId: "pet",
        enabledAgents: ["codex"],
      };
    }
    if (command === "ensure_bridge_runtime") return { running: true };
    if (command === "usb_get_status") return { connected: true };
    if (command === "usb_sync_appearance") return { ok: true, fileCount: 4, byteCount: 4096 };
    if (command === "dispatch_remote_cli_binding") return { ok: true };
    return {};
  };
  const listen = async (eventName, callback) => {
    assert.equal(eventName, "usb-sync-progress");
    callback({
      payload: {
        currentFile: 2,
        totalFiles: 4,
        bytesSent: 2048,
        bytesTotal: 4096,
      },
    });
    return () => {};
  };

  await applyDesktopPetAssignment({
    invoke,
    listen,
    agentAppearanceMap: { codex: "old-avatar" },
    agentId: "codex",
    appearance: { id: "new-avatar", name: "新形象" },
    agentOptions: [{ id: "codex", label: "Codex" }],
    boardDeviceId: "board-1",
    currentAppearanceId: "old-avatar",
    onProgress: (progress) => progressEvents.push(progress),
  });

  assert.deepEqual(progressEvents[0], {
    type: "info",
    text: "USB 传输中… 2/4 个素材 (2.0 KB/4.0 KB) 50%",
    currentFile: 2,
    totalFiles: 4,
    bytesSent: 2048,
    bytesTotal: 4096,
    percent: 50,
  });
});

test("applyDesktopPetAssignment reports instant P4 cached-slot activation", async () => {
  installStorage();
  const invoke = async (command) => {
    if (command === "load_bridge_profile") {
      return {
        desktopDeviceId: "desk-1",
        mqttNamespace: "pet",
        enabledAgents: ["codex"],
        selectedAgentId: "codex",
      };
    }
    if (command === "usb_get_status") return { connected: true };
    if (command === "usb_sync_appearance") {
      return { ok: true, fileCount: 0, byteCount: 0, reusedSlot: true };
    }
    if (command === "dispatch_remote_cli_binding") return { ok: true };
    if (command === "ensure_bridge_runtime") return { running: true };
    return {};
  };

  const result = await applyDesktopPetAssignment({
    invoke,
    listen: null,
    agentAppearanceMap: { codex: "old-avatar" },
    agentId: "codex",
    appearance: { id: "cached-avatar", name: "缓存形象" },
    agentOptions: [{ id: "codex", label: "Codex" }],
    boardDeviceId: "board-1",
    currentAppearanceId: "old-avatar",
  });

  assert.match(result.notice, /从设备缓存即时切换/);
  assert.match(result.notice, /无需重新传输素材/);
});

test("applyDesktopPetAssignment does not switch followed channel when USB appearance sync fails", async () => {
  const storage = installStorage();
  const calls = [];
  const invoke = async (command, args) => {
    calls.push({ command, args });
    if (command === "load_bridge_profile") {
      return {
        desktopDeviceId: "desk-1",
        mqttUrl: "mqtt://example",
        mqttNamespace: "pet",
        mqttUsername: "u",
        mqttPassword: "p",
        transport: "mqtt",
        serialPort: "",
        serialBaud: 115200,
        petChannelId: "pet",
        enabledAgents: ["codex"],
      };
    }
    if (command === "usb_get_status") return { connected: true };
    if (command === "usb_sync_appearance") throw new Error("Connection lost");
    return {};
  };

  await assert.rejects(
    () => applyDesktopPetAssignment({
      invoke,
      listen: null,
      agentAppearanceMap: { codex: "old-avatar" },
      agentId: "claude-code",
      appearance: { id: "new-avatar", name: "新形象" },
      agentOptions: [{ id: "claude-code", label: "Claude Code" }],
      boardDeviceId: "board-1",
      currentAppearanceId: "old-avatar",
    }),
    /形象素材下发失败，已取消切换跟随[\s\S]*Connection lost/,
  );

  assert.equal(calls.some((call) => call.command === "save_bridge_profile"), false);
  assert.equal(calls.some((call) => call.command === "ensure_bridge_runtime"), false);
  assert.equal(calls.some((call) => call.command === "dispatch_remote_cli_binding"), false);
  assert.equal(storage.get(AGENT_APPEARANCE_MAP_STORAGE_KEY), undefined);
  assert.equal(storage.get(ENABLED_AGENTS_STORAGE_KEY), undefined);
  assert.equal(storage.get(ACTIVE_APPEARANCE_KEY), undefined);
});

test("applyDesktopPetAssignment preserves the explicit appearance cancellation result", async () => {
  const storage = installStorage();
  const calls = [];
  const invoke = async (command, args) => {
    calls.push({ command, args });
    if (command === "load_bridge_profile") {
      return {
        desktopDeviceId: "desk-1",
        mqttNamespace: "pet",
        enabledAgents: ["codex"],
      };
    }
    if (command === "usb_get_status") return { connected: true };
    if (command === "usb_sync_appearance") {
      return { ok: false, error: APPEARANCE_SYNC_CANCELLED_MESSAGE };
    }
    return {};
  };

  await assert.rejects(
    () => applyDesktopPetAssignment({
      invoke,
      listen: null,
      agentAppearanceMap: { codex: "old-avatar" },
      agentId: "codex",
      appearance: { id: "new-avatar", name: "新形象" },
      agentOptions: [{ id: "codex", label: "Codex" }],
      boardDeviceId: "board-1",
      currentAppearanceId: "old-avatar",
    }),
    new RegExp(APPEARANCE_SYNC_CANCELLED_MESSAGE),
  );

  assert.equal(calls.some((call) => call.command === "save_bridge_profile"), false);
  assert.equal(storage.get(ACTIVE_APPEARANCE_KEY), undefined);
});

test("applyDesktopPetAssignment refuses appearance changes without USB before saving", async () => {
  const storage = installStorage();
  const calls = [];
  const invoke = async (command, args) => {
    calls.push({ command, args });
    if (command === "usb_get_status") return { connected: false };
    return {};
  };

  await assert.rejects(
    () => applyDesktopPetAssignment({
      invoke,
      listen: null,
      agentAppearanceMap: { codex: "old-avatar" },
      agentId: "claude-code",
      appearance: { id: "new-avatar", name: "新形象" },
      agentOptions: [{ id: "claude-code", label: "Claude Code" }],
      boardDeviceId: "board-1",
      currentAppearanceId: "old-avatar",
    }),
    new RegExp(APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE),
  );

  assert.equal(calls.some((call) => call.command === "save_bridge_profile"), false);
  assert.equal(storage.get(AGENT_APPEARANCE_MAP_STORAGE_KEY), undefined);
});
