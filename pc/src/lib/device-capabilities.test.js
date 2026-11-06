import test from "node:test";
import assert from "node:assert/strict";
import { supportsDeviceSessions, hasFixedDeviceAppearance } from "./device-capabilities.js";
import { normalizeUsbStatusPayload } from "../shell/DeviceContext.pure.js";

test("P4 keeps legacy sessions; S3 requires an explicit handshake capability", () => {
  assert.equal(supportsDeviceSessions({runtime:"esp-p4"}),true);
  assert.equal(supportsDeviceSessions({runtime:"linux"}),false);
  assert.equal(supportsDeviceSessions({runtime:"esp-s3"}),false);
  assert.equal(supportsDeviceSessions({runtime:"esp-s3",capabilities:{sessionSync:true}}),true);
});
test("StopWatch identity survives normalization without becoming a P4 media target", () => {
  const usb = normalizeUsbStatusPayload({connected:true,runtime:"esp32-s3",
    capabilities:{boardId:"m5stack-stopwatch",maxAppearanceSlots:0,sessionSync:true}});
  assert.equal(usb.runtime,"esp-s3");
  assert.equal(hasFixedDeviceAppearance(usb),true);
  assert.equal(hasFixedDeviceAppearance({runtime:"esp-p4",capabilities:{maxAppearanceSlots:0}}),false);
});
