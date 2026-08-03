/*
 * [Input] board-runtime/start-rpi.sh USB/MQTT transport selection policy.
 * [Output] Prevent stale /dev/ttyGS0 detection from forcing USB when the UDC is not configured.
 */

const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const { resolve } = require("node:path");
const test = require("node:test");

const scriptPath = resolve(__dirname, "../start-rpi.sh");
const script = readFileSync(scriptPath, "utf8");

function autoDetectionBlock() {
  const start = script.indexOf('if [ -z "${BOARD_TRANSPORT_FORCE:-}" ]; then');
  const end = script.indexOf('else\n    BOARD_TRANSPORT="$BOARD_TRANSPORT_FORCE"', start);
  assert.notEqual(start, -1, "auto-detection branch should exist");
  assert.notEqual(end, -1, "forced transport branch should follow auto-detection");
  return script.slice(start, end);
}

test("auto transport only selects USB when the UDC is configured", () => {
  const block = autoDetectionBlock();
  assert.match(
    block,
    /\[\s+"\$UDC_STATE"\s+=\s+"configured"\s+\]\s+&&\s+\[\s+-c \/dev\/ttyGS0\s+\]/,
  );
  assert.doesNotMatch(block, /if \[ -c \/dev\/ttyGS0 \]; then\s*\n\s*BOARD_TRANSPORT="usb"/);
});

test("auto transport falls back to MQTT when ttyGS0 exists but USB is stale", () => {
  const block = autoDetectionBlock();
  assert.match(
    block,
    /USB serial device exists but host is not configured after .+; using MQTT/,
  );
});
