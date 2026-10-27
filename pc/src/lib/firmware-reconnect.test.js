import test from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";

test("OTA validation preserves a working serial handle and bounds reopen after boot grace", () => {
  const source=readFileSync(new URL("../../src-tauri/src/usb_serial.rs",import.meta.url),"utf8");
  const body=source.slice(source.indexOf("fn wait_for_firmware_validation"),source.indexOf("fn best_effort_firmware_abort"));
  assert.match(body,/let mut force_reconnect = false;/);
  assert.match(body,/started\.elapsed\(\) < Duration::from_secs\(20\)/);
  assert.ok(body.indexOf("Duration::from_secs(20)") < body.indexOf("reconnect()"));
  assert.ok(body.indexOf("Duration::from_secs(20)") < body.indexOf("self.query_diagnostics"));
  assert.match(body,/validation_query_failed[\s\S]*force_reconnect = true;/);
  assert.match(body,/evaluate_firmware_validation/);
});

test("serial reader updates hello identity without unbounded acknowledgement echoes", () => {
  const source=readFileSync(new URL("../../src-tauri/src/usb_serial.rs",import.meta.url),"utf8");
  assert.match(source,/let mut last_hello_ack = None;/);
  assert.match(source,/hello_ack_due\(last_hello_ack, now, &desktop_id\)[\s\S]*?write_all\(ack.as_bytes\(\)\).is_ok\(\)[\s\S]*?last_hello_ack = Some\(now\)/);
  assert.match(source,/apply_hello_payload_to_connection\(c, &msg.payload\)/);
});
