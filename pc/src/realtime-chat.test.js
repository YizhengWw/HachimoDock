/**
 * [Input] Desktop sources (DeviceContext, DeviceDashboard, RealtimeChatPanel, ApiSettings,
 *         PersonaVoiceModal), the Tauri Rust modules, the ESP32-P4 firmware sources and protocol doc.
 * [Output] Cross-module contract coverage for 实时对话: the board key round-trips through Rust to
 *          the UI, the persona/voice start payload, credential storage, the streamed playback and
 *          conversation topics, the HUD/animation override, and the docs that declare them.
 * [Pos] test node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
import { buildRealtimeChatStartInput, voiceSpecFor, previewTextFor } from "./lib/persona-voice.js";

const srcDir = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(srcDir, "..", "..");
const read = (relative) => readFileSync(join(repoRoot, relative), "utf8");

test("start payload carries the assembled system prompt, greeting and camelCase voice spec", () => {
  const input = buildRealtimeChatStartInput(
    { id: "a1", name: "Torti", description: "一只乌龟", personaVoice: undefined },
    "p4-board",
  );
  assert.equal(input.boardDeviceId, "p4-board");
  assert.equal(input.appearanceId, "a1");
  assert.equal(input.displayName, "Torti");
  assert.match(input.systemPrompt, /你是「Torti」/);
  assert.ok(input.greeting.length > 0);
  assert.deepEqual(Object.keys(input.voice).sort(), ["cloneSpeakerId", "speaker", "speed", "volume"]);
  assert.equal(voiceSpecFor({ voice: { clone_speaker_id: " S_x " } }).cloneSpeakerId, "S_x");
  assert.equal(previewTextFor({ greeting: "汪！" }), "汪！");
});

test("Rust owns the conversation loop and exposes the commands the UI calls", () => {
  const lib = read("pc/src-tauri/src/lib.rs");
  const rtc = read("pc/src-tauri/src/realtime_chat.rs");
  assert.match(lib, /realtime_chat::route_device_audio\(&topic, &payload\)/);
  assert.match(lib, /realtime_chat::handle_input_event\(emitter, &payload\)/);
  assert.match(lib, /\| realtime_chat::ACTION_ID/);
  for (const command of ["realtime_chat_start", "realtime_chat_stop", "realtime_chat_status", "persona_voice_preview", "load_voice_chat_settings", "save_voice_chat_settings"]) {
    assert.match(lib, new RegExp(`realtime_chat::${command},`), `command ${command} registered`);
  }
  assert.match(rtc, /pub const ACTION_ID: &str = "realtime_chat";/);
  assert.match(rtc, /"audio\/conversation"[\s\S]*"halfDuplex": !duplex/);
  assert.match(rtc, /listen_during_reply/);
  assert.match(rtc, /"audioAec","audioVad","audioFullDuplex"/);
  assert.match(rtc, /epoch\.fetch_add\(1, Ordering::SeqCst\)/);
  assert.match(rtc, /"audio\/play_begin"/);
  assert.match(rtc, /"audio\/play_chunk"/);
  assert.match(rtc, /"audio\/play_end"/);
  assert.match(rtc, /"ui\/conversation"/);
  assert.match(rtc, /IDLE_TIMEOUT: Duration = Duration::from_secs\(60\)/);
  assert.match(rtc, /REQUEST_EVENT: &str = "realtime-chat-request"/);
  const tts = read("pc/src-tauri/src/doubao_tts.rs");
  assert.match(tts, /wss:\/\/openspeech\.bytedance\.com\/api\/v3\/tts\/bidirection/);
  assert.match(tts, /VOICE_CLONE_RESOURCE_ID: &str = "seed-icl-2\.0"/);
});

test("DeviceContext resolves the board key into the current appearance and mirrors Rust state", () => {
  const context = read("pc/src/shell/DeviceContext.jsx");
  assert.match(context, /listen\("realtime-chat-request"/);
  assert.match(context, /listen\("realtime-chat-state"/);
  assert.match(context, /invoke\("realtime_chat_start", \{ input: buildRealtimeChatStartInput\(appearance, boardDeviceId\) \}\)/);
  assert.match(context, /还没有设置人设与声音/);
  assert.match(context, /startRealtimeChat,\n\s+stopRealtimeChat,/);
});

test("dashboard offers the realtime_chat action and the conversation card", () => {
  const dashboard = read("pc/src/DeviceDashboard.jsx");
  assert.match(dashboard, /id: "realtime_chat", label: "实时对话"/);
  assert.match(dashboard, /const P4_CUSTOM_ACTION_OPTIONS = \[\n\s+"agent_prompt",\n\s+"realtime_chat",/);
  assert.match(dashboard, /<RealtimeChatPanel keyBound=\{realtimeChatKeyBound\}[^\n]*onOpenPersona=/);
  const panel = read("pc/src/dashboard/RealtimeChatPanel.jsx");
  assert.match(panel, /开始聊天/);
  assert.match(panel, /<UsageHelp/);
  assert.match(read("pc/src/UsageHelp.jsx"), /等它说完再说/);
  assert.doesNotMatch(panel, /半双工|选一个短按绑定/);
});

test("SW2 long press defaults to one realtime toggle and respects explicit overrides", () => {
  const dashboard = read("pc/src/DeviceDashboard.jsx");
  const rowSource = dashboard.match(/\{ id: "p4_sw2_long",[^\n]+/)[0].replace(/,$/, "");
  const row = new Function("P4_CUSTOM_ACTION_OPTIONS", `return (${rowSource});`)(["realtime_chat", "disabled"]);
  const builderSource = dashboard.slice(dashboard.indexOf("export function buildBoardButtonConfigBindings"), dashboard.indexOf("function createButtonConfigRequestId"));
  const build = new Function("buttonControlRowsForRuntime", "clampButtonActionValue", `${builderSource.replace("export function", "function")}; return buildBoardButtonConfigBindings;`)(() => [row], (v) => v);
  assert.deepEqual(build({}, {}, "esp-p4"), [
    { event: "button.sw2.long_press", action: "realtime_chat" },
    { event: "button.sw2.hold", action: "disabled" },
  ]);
  assert.deepEqual(build({ p4_sw2_long: "disabled" }, {}, "esp-p4"), [
    { event: "button.sw2.long_press", action: "disabled" },
    { event: "button.sw2.hold", action: "disabled" },
  ]);
  assert.deepEqual(build({ p4_sw2_long: "voice_ptt" }, {}, "esp-p4", true), [
    { event: "button.sw2.long_press", action: "disabled" },
    { event: "button.sw2.hold", action: "voice_ptt" },
  ]);
  const firmware = read("firmware/main/pet_p4_input.c");
  assert.ok(firmware.includes('add_default_binding(config, "button.sw2.long_press", "realtime_chat", "")'));
  assert.ok(firmware.includes('add_default_binding(config, "button.sw2.hold", "disabled", "")'));
});

test("API settings persist the LLM and Doubao TTS credentials and the modal previews through Rust", () => {
  const settings = read("pc/src/ApiSettings.jsx");
  assert.match(settings, /invoke\("load_voice_chat_settings"\)/);
  assert.match(settings, /invoke\("save_voice_chat_settings"/);
  assert.match(settings, /语音识别与合成/);
  assert.match(settings, /语音 API Key（识别与合成共用）/);
  assert.doesNotMatch(settings, /api-settings-tts-key|api-settings-tts-resource|api-settings-asr-resource/);
  assert.match(settings, /对话大模型/);
  assert.match(settings, /LLM_PRESETS/);
  assert.match(read("pc/src/lib/api-configuration.js"), /id: "deepseek"[\s\S]*id: "doubao"[\s\S]*id: "mimo"/);
  const modal = read("pc/src/PersonaVoiceModal.jsx");
  assert.match(modal, /invoke\("persona_voice_preview"/);
  assert.match(modal, /status\?\.ttsConfigured === true/);
});

test("saved persona refreshes the device target and the active session without switching appearances", () => {
  const store = read("pc/src/lib/appearance-store.js");
  const context = read("pc/src/shell/DeviceContext.jsx");
  const rtc = read("pc/src-tauri/src/realtime_chat.rs");
  assert.match(store, /emitPersonaVoiceUpdated\(appearanceId, saved\)/);
  assert.match(store, /invoke\("realtime_chat_update_persona"/);
  assert.match(context, /addEventListener\(PERSONA_VOICE_UPDATED_EVENT/);
  assert.match(context, /await getAppearance\(selectedAppearance.id\)/);
  assert.match(rtc, /pending_persona: Mutex<Option<RealtimeChatStartInput>>/);
  assert.match(rtc, /status.appearance_id != input.appearance_id/);
  assert.match(rtc, /current.system_prompt = next.system_prompt/);
});

test("firmware implements conversation capture, streamed playback, the key action and the HUD", () => {
  const audio = read("firmware/main/pet_p4_audio.c");
  assert.match(audio, /esp_err_t pet_p4_audio_conversation_set\(bool enabled, bool half_duplex\)/);
  assert.match(audio, /esp_err_t pet_p4_audio_stream_push\(const uint8_t \*pcm, size_t length\)/);
  assert.match(audio, /PET_P4_AUDIO_STREAM_START_BYTES 6400/);
  assert.match(audio, /if \(playback_gates_capture\(now_ms\)\) \{/);
  assert.match(audio, /if \(pet_p4_audio_conversation_active\(\)\s*\|\| atomic_load_explicit\(&g_stream_active, memory_order_acquire\)\) return;/);
  assert.match(audio, /!conversation && now_ms - started_ms >= PET_P4_AUDIO_MAX_CAPTURE_MS/);
  const protocol = read("firmware/main/pet_p4_protocol.c");
  for (const topic of ["audio/conversation", "audio/play_begin", "audio/play_chunk", "audio/play_end", "audio/play_flush", "ui/conversation"]) {
    assert.match(protocol, new RegExp(`strcmp\\(topic, "${topic.replace("/", "\\/")}"\\) == 0`), topic);
  }
  assert.match(protocol, /if \(!strcmp\(state->conversation_state, "listening"\)\) return "notification";/);
  const input = read("firmware/main/pet_p4_input.c");
  assert.match(input, /"voice_ptt",\n\s+"realtime_chat",/);
  const view = read("firmware/main/pet_p4_view.c");
  assert.match(view, /conversation_active\(state\)/);
  const doc = read("firmware/protocol.md");
  assert.match(doc, /## Realtime Conversation Audio/);
  assert.match(doc, /audio\/play_chunk/);
  const ini = read("firmware/platformio.ini");
  assert.match(ini, /custom_p4_project_version = 0\.7\.59-p4/);
});

test("failure status survives fast exit, cancellation bypasses audio queues, device logs are forwarded", () => {
  const rtc = read("pc/src-tauri/src/realtime_chat.rs");
  assert.match(rtc, /stop_notify\.notify_one\(\)/);
  assert.match(rtc, /session\.stop_notify\.notified\(\)/);
  assert.doesNotMatch(rtc, /\*slot = None/);
  assert.match(rtc, /topic == "audio\/diagnostic"/);
  assert.match(read("pc/src-tauri/src/realtime_chat_log.rs"), /MAX_BYTES: u64 = 1024 \* 1024/);
  const firmware = read("firmware/main/pet_p4_protocol.c");
  assert.match(firmware, /conversation_error_until_ms/);
  assert.match(firmware, /pet_p4_conversation_update\(state, json_string\(payload, "sessionId"\)/);
  assert.match(read("firmware/main/pet_p4_conversation.c"), /now_ms \+ 8000ULL/);
});

test("live captions are isolated from Agent state and follow PCM playback", () => {
  const rtc = read("pc/src-tauri/src/realtime_chat.rs");
  assert.match(rtc, /pointer\("\/features\/conversationSubtitles"\)/);
  assert.match(rtc, /"offsetBytes":sent_bytes \+ pending.len\(\) as u64/);
  assert.match(rtc, /"role":"user","final":final_text/);
  assert.match(rtc, /Some\("playback_completed"\)/);
  assert.match(rtc, /completed_playback\.lock\(\).map\(\|id\| \*id == turn_id\)/);
  const view = read("firmware/main/pet_p4_view.c");
  assert.match(view, /out->agent = ""/);
  assert.match(view, /out->stats_json = ""/);
  const renderer = read("firmware/main/pet_p4_renderer.c");
  assert.match(renderer, /show_session_queue = !realtime/);
  assert.match(renderer, /if \(!realtime\) draw_session_queue/);
});
