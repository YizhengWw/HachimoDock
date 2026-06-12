/**
 * [Input] Device availability helper.
 * [Output] Node regression coverage that wireless device actions can use the current online board even when the saved board id is stale.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  isBoundDeviceOnline,
  resolveOnlineBoardDeviceId,
} from "./device-availability.js";

test("resolveOnlineBoardDeviceId prefers the saved board when it is online", () => {
  const id = resolveOnlineBoardDeviceId(
    {
      "board-old": { online: true, targetDeviceId: "desktop-1" },
      "board-new": { online: true, targetDeviceId: "desktop-1" },
    },
    { boardDeviceId: "board-old", desktopDeviceId: "desktop-1" },
  );

  assert.equal(id, "board-old");
});

test("resolveOnlineBoardDeviceId falls back to an online board targeting the same desktop", () => {
  const id = resolveOnlineBoardDeviceId(
    {
      "board-old": { online: false, targetDeviceId: "desktop-1" },
      "board-new": { online: true, targetDeviceId: "desktop-1" },
      "board-other": { online: true, targetDeviceId: "desktop-2" },
    },
    { boardDeviceId: "board-old", desktopDeviceId: "desktop-1" },
  );

  assert.equal(id, "board-new");
  assert.equal(isBoundDeviceOnline({ "board-new": { online: true, desktopDeviceId: "desktop-1" } }, {
    boardDeviceId: "board-old",
    desktopDeviceId: "desktop-1",
  }), true);
});

test("resolveOnlineBoardDeviceId can use the only online board during dev stale-binding walkthroughs", () => {
  const id = resolveOnlineBoardDeviceId(
    {
      "board-new": { online: true, targetDeviceId: "desktop-formal" },
    },
    { boardDeviceId: "board-old", desktopDeviceId: "desktop-temp" },
  );

  assert.equal(id, "board-new");
});

test("resolveOnlineBoardDeviceId does not pick unrelated online boards when there are multiple candidates", () => {
  const id = resolveOnlineBoardDeviceId(
    {
      "board-other": { online: true, targetDeviceId: "desktop-2" },
      "board-third": { online: true, targetDeviceId: "desktop-3" },
    },
    { boardDeviceId: "board-old", desktopDeviceId: "desktop-1" },
  );

  assert.equal(id, "");
});
