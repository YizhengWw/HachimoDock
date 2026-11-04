import test from "node:test";
import assert from "node:assert/strict";

import { DEVICE_VOLUME_MAX, normalizeDeviceControls } from "./device-controls.js";

test("device controls clamp output volume to 0..100", () => {
  assert.deepEqual(normalizeDeviceControls({ volume: 200 }), { volume: DEVICE_VOLUME_MAX });
  assert.deepEqual(normalizeDeviceControls({ volume: -1 }), { volume: 0 });
  assert.deepEqual(normalizeDeviceControls({}), { volume: 70 });
});

test("100 percent is accepted", () => {
  assert.deepEqual(normalizeDeviceControls({ volume: 100 }), { volume: 100 });
});
