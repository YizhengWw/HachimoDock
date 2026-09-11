import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const native = readFileSync(new URL("../../src-tauri/src/lib.rs", import.meta.url), "utf8");

for (const command of [
  "ensure_bridge_runtime",
  "ensure_voice_runtime",
  "set_p4_session_binding",
  "audio_bridge_signal",
]) {
  test(`${command} delegates blocking work off the Tauri window thread`, () => {
    const start = native.indexOf(`async fn ${command}(`);
    assert.ok(start >= 0, `${command} must be an async command`);
    const end = native.indexOf(`fn ${command}_blocking(`, start);
    assert.ok(end > start, "blocking implementation must stay separate");
    const wrapper = native.slice(start, end);
    assert.match(wrapper, /tauri::async_runtime::spawn_blocking\(move \|\|/);
    assert.ok(wrapper.includes(`${command}_blocking(`));
    assert.match(wrapper, /\.await\s*\.map_err/);
    assert.doesNotMatch(wrapper, /\.lock\(|thread::sleep|Command::new/);
  });
}

test("startup runs the same serialized Bridge implementation on its own worker", () => {
  assert.match(native, /thread::spawn\(move \|\| \{\s*thread::sleep\(Duration::from_secs\(3\)\);\s*if let Err\(error\) = ensure_bridge_runtime_blocking\(/);
  assert.match(native, /fn ensure_bridge_runtime_blocking\([\s\S]*?let _lifecycle_guard = bridge_runtime_lifecycle_lock\(\)/);
});

test("asynchronous session updates retain the frontend ordering queue", () => {
  const hook = readFileSync(new URL("../dashboard/useP4SessionSync.js", import.meta.url), "utf8");
  assert.match(hook, /const bindingPromise = bindingQueueRef.current[\s\S]*?\.then\(\(\) => invoke\("set_p4_session_binding"/);
  assert.match(hook, /bindingQueueRef.current = bindingPromise.catch/);
});

test("asynchronous microphone signals retain start and stop ordering", () => {
  const dashboard = readFileSync(new URL("../DeviceDashboard.jsx", import.meta.url), "utf8");
  assert.match(dashboard, /audioSignalQueueRef.current[\s\S]*?\.then\(\(\) => invoke\("audio_bridge_signal", input\)\)/);
  assert.equal((dashboard.match(/invoke\("audio_bridge_signal"/g) || []).length, 1);
  assert.equal((dashboard.match(/await sendAudioBridgeSignal\(/g) || []).length, 4);
});
