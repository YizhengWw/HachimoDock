// Source contracts supplement the native pacing tests and Windows cross-build.
// They do not substitute for a Windows driver/device playback test.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
const rust = (name) => readFileSync(new URL(`../src-tauri/src/${name}`, import.meta.url), "utf8");

test("Windows opens an overlapped serial handle with independent non-inherited events", () => {
  const source = rust("usb_serial/windows_serial.rs");
  const handles = rust("usb_serial/connection_handle.rs");
  assert.match(handles, /WindowsSerialPort::open/);
  assert.match(source, /FILE_ATTRIBUTE_NORMAL \| FILE_FLAG_OVERLAPPED/);
  assert.match(source, /CreateEventW\(std::ptr::null\(\), 1, 0, std::ptr::null\(\)\)/);
  assert.match(source, /Self::wrap\(self.control.try_clone_native\(\)\?/);
  assert.match(source, /SetHandleInformation\(control.as_raw_handle\(\), HANDLE_FLAG_INHERIT, 0\)/);
  assert.match(source, /ReadFile\(self.handle\(\),[\s\S]*?&mut operation/);
  assert.match(source, /WriteFile\(self.handle\(\),[\s\S]*?&mut operation/);
  assert.doesNotMatch(source, /self\.control\.(read|write)\(/);
});

test("Windows cancellation joins only its own operation before releasing buffers", () => {
  const source = rust("usb_serial/windows_serial.rs");
  assert.match(source, /CancelIoEx\(self.handle\(\), &operation\);[\s\S]*?GetOverlappedResult\(self.handle\(\), &operation, &mut transferred, 1\)/);
  assert.doesNotMatch(source, /CancelIoEx\([^,]+, std::ptr::null/);
  assert.match(source, /if completed != 0 \{ return Self::transfer_result/);
  assert.match(source, /WriteTotalTimeoutConstant: ms.max\(500\)/);
});

test("CH343 live audio retains 64-byte bursts and never drains or pauses RX", () => {
  const source = rust("usb_serial.rs");
  assert.match(source, /P4_CH343_SERIAL_WRITE_SLICE_BYTES: usize = 64/);
  assert.match(source, /write_serial_live_frame\(conn.writer.as_mut\(\), bytes,\s*paced_slice_bytes.unwrap_or\(bytes.len\(\)\), live_audio_pacing::serial_frame_gap\(topic, P4_CH343_SERIAL_WRITE_GAP\)\)/);
  const live = rust("usb_serial/live_audio_pacing.rs").split("#[cfg(test)]\nmod tests")[0];
  assert.doesNotMatch(live, /writer\.flush\(/);
  assert.match(source, /let audio_frame = topic.starts_with\("audio\/"\) \|\| topic.starts_with\("media\/"\) \|\| topic == "ui\/conversation";/);
  assert.match(source, /let live_frame = audio_frame \|\| topic == "widget\/data" \|\| topic == "app\/snapshot" \|\| topic == "ui\/text-pixels"\s*\|\| \(!bulk_frame && conn.capture_active.load\(Ordering::Acquire\)\);/);
  assert.match(source, /let bounded_frame = serial_frame_is_bounded\(topic, live_frame\);/);
  const classify=source.split("fn serial_frame_is_bounded(")[1].split("fn flush_serial_writer")[0];
  assert.match(classify, /live_frame \|\| matches!\(topic, "system\/heartbeat" \| "diagnostics\/query"\)/);
  const write=source.split("let write_result = {")[1].split("if let Err(error) = write_result")[0];
  assert.match(write, /let _pause = \(!bounded_frame\).then\(\|\| SerialReaderPause::new\(&self.serial_body_write_active\)\);/);
  assert.equal((write.match(/SerialReaderPause/g)||[]).length,1);
  const bounded=write.split("if bounded_frame {")[1].split("if let Some(write_slice_bytes)")[0];
  assert.doesNotMatch(bounded, /flush_serial_writer|\.flush\(|SerialReaderPause|serial_body_write_active/);
  assert.match(bounded, /return result.map\(\|_\|\(\)\).map_err/);
  assert.doesNotMatch(source, /live_write_plan/);
  assert.match(live,/writer.write\(&chunk\[offset\.\.\]\)/);
  assert.doesNotMatch(live,/writer.write_all\(/);
  assert.match(live,/MAX_RECOVERY_RETRIES/);
  assert.match(live,/RECOVERY_DEADLINE/);
  assert.match(rust("usb_serial/windows_serial.rs"),/uncertain_write_error\(error\)/);
});

test("device rejection logs allowlisted topic and reason without provider payloads", () => {
  const source = rust("realtime_chat.rs");
  const rejected = source.slice(source.indexOf('if topic == "protocol/ack"'), source.indexOf('if !topic.starts_with("audio/")'));
  assert.match(rejected, /"protocol_rejected", json!\(\{"stage":stage,"reason":reason,"ok":false\}\)/);
  assert.match(rejected, /"audio_chunk_rejected" => "audio_chunk_rejected"/);
  assert.doesNotMatch(rejected, /diagnostics::record[^;]*payload\)/);
});

test("failed audio writes stop playback and do not advance the sent-byte counter", () => {
  const source = rust("realtime_chat.rs");
  const chunk = source.slice(source.indexOf("let flush_chunk ="), source.indexOf("while let Ok((expected, cmd))"));
  assert.match(chunk, /let result = send_session_board\(\s*usb, &session,/);
  assert.match(chunk, /"playback_tx"/);
  assert.match(chunk, /if let Err\(error\) = result \{[\s\S]*?request_failure\(&session, "playback_write_failed", Some\("[^"\n]+"\)\);\s+return;/);
  const failure = source.slice(source.indexOf("fn request_failure("), source.indexOf("fn start_guard("));
  assert.match(failure, /session\.cancelled\.swap\(true, Ordering::SeqCst\)/);
  assert.match(failure, /session\.stop_notify\.notify_one\(\)/);
  assert.ok(chunk.indexOf("if let Err(error) = result") < chunk.indexOf("*sent_bytes +="));
});
