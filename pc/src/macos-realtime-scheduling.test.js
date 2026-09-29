// Wiring contracts complement the native QoS test and opt-in pacing benchmark.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
const rust = (name) => readFileSync(new URL(`../src-tauri/src/${name}`, import.meta.url), "utf8");

test("Mac scheduling protection lives on the dedicated player thread, not a shared worker", () => {
  const player = rust("realtime_chat.rs").split("fn spawn_player(")[1].split("async fn ")[0];
  assert.match(player, /thread::spawn\(move \|\| \{\s*#\[cfg\(target_os = "macos"\)\]\s*let _audio_activity =/);
  assert.match(player, /AudioActivity::begin_on_player_thread\(\)/);
  assert.match(player, /"playback_scheduling"/);
  assert.match(player, /"qosApplied":activity.qos_error == 0/);
  assert.ok(player.indexOf("let _audio_activity") < player.indexOf("while let Ok((expected, cmd))"));
});

test("activity ends through RAII and cannot migrate to an async worker", () => {
  const source = rust("realtime_audio_macos.rs");
  assert.match(source, /PhantomData<Rc<\(\)>>/);
  assert.match(source, /impl Drop for AudioActivity[\s\S]*endActivity\(&self.token\)/);
  assert.match(source, /UserInitiatedAllowingIdleSystemSleep \| NSActivityOptions::LatencyCritical/);
  assert.doesNotMatch(source, /Command::new|mem::forget|into_raw/);
});

test("Mac protection does not introduce busy spinning or change Windows transport", () => {
  const source = rust("usb_serial/live_audio_pacing.rs").split("#[cfg(test)]")[0];
  assert.match(source, /#\[cfg\(windows\)\][\s\S]*spin_loop/);
  assert.match(source, /#\[cfg\(not\(windows\)\)\]\s*std::thread::sleep\(gap\)/);
  assert.match(rust("lib.rs"), /#\[cfg\(target_os = "macos"\)\]\s*mod realtime_audio_macos;/);
});
