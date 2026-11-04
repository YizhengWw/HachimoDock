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
  const ack=source.slice(source.indexOf("if hello_ack_due(last_hello_ack"),source.indexOf("\n                on_message(msg.topic"));
  assert.match(ack,/^if hello_ack_due\(last_hello_ack, now, &desktop_id\)\s*\{\s*if let Ok\(_wire\)=self.wire_guard.try_lock\(\)\s*\{\s*if self.ensure_wire_available\(&c.port_name,Some\(&c.board_device_id\)\).is_ok\(\)\s*&&\s*c.writer.write_all\(ack.as_bytes\(\)\).is_ok\(\)\s*\{\s*last_hello_ack\s*=\s*Some\(now\);/);
  const due=source.split("fn hello_ack_due(")[1].split("fn serial_probe_handshake_messages")[0];
  assert.match(due,/!desktop_device_id.trim\(\).is_empty\(\)\s*&& last_sent.is_none_or\(\|last\| now.saturating_duration_since\(last\) >= Duration::from_secs\(2\)\)/);
  assert.match(source,/filter\(\|c\| c.connection_id == connection_id\)[\s\S]*refresh_pending_hello\(c\)/);
  const refresh=source.split("fn refresh_pending_hello(")[1].split("pub fn set_desktop_device_id")[0];
  assert.match(refresh,/generation != c.connection_id/);
  assert.match(refresh,/inbound_owner.is_current/);
  assert.match(refresh,/apply_hello_payload_to_connection\(c, &payload\)/);
});
