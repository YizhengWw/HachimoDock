import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { normalizePersonaVoice, buildRealtimeChatStartInput } from "./lib/persona-voice.js";
const read = (name) => readFileSync(new URL(name, import.meta.url), "utf8");

test("saving persists first, refreshes consumers, then applies the saved voice to the live session", async () => {
  const source = read("./lib/appearance-store.js");
  const body = source.slice(source.indexOf("export async function savePersonaVoice"), source.indexOf("function appLocalAudioSrc"));
  const events = [];
  const deps = {
    BUILTIN_TERRIER_APPEARANCE_ID: "terrier", normalizePersonaVoice, PERSONA_VOICE_SCHEMA_VERSION: 1,
    ensureRoot: async () => {}, ROOT_DIR: "custom-appearances", exists: async () => true,
    BaseDirectory: { AppLocalData: 1 }, mkdir: async () => {}, PERSONA_VOICE_FILE: "persona-voice.json",
    writeTextFile: async (_, value) => events.push(["persist", JSON.parse(value)]),
    clearPersonaSetupPending: () => {}, invalidateAppearanceCache: () => events.push(["invalidate"]),
    emitPersonaVoiceUpdated: (id, value) => events.push(["refresh", id, value]),
    buildRealtimeChatStartInput,
    invoke: async (command, args) => { events.push(["apply", command, args]); },
  };
  const save = new Function(...Object.keys(deps), `${body.replace("export async", "async")}; return savePersonaVoice;`)(...Object.values(deps));
  const saved = await save("terrier", { persona: { display_name: "新名字", style: "简短回答" }, voice: { speaker: "new-speaker", speed: 1.2, volume: 0.8 } });
  assert.deepEqual(events.map(([event]) => event), ["persist", "invalidate", "refresh", "apply"]);
  assert.equal(events[3][1], "realtime_chat_update_persona");
  assert.equal(events[3][2].input.voice.speaker, "new-speaker");
  assert.equal(events[3][2].input.voice.speed, 1.2);
  assert.equal(events[3][2].input.voice.volume, 0.8);
  assert.equal(events[3][2].input.appearanceId, "terrier");
  assert.equal(saved.configured, true);
  assert.equal("volume" in events[0][1].voice, false, "global volume must not be saved per appearance");
});

test("the voice modal loads and saves a shared volume and factory persona reset preserves it", () => {
  const source = read("./PersonaVoiceModal.jsx");
  assert.match(source, /invoke\("load_realtime_voice_volume"\)/);
  assert.match(source, /invoke\("save_realtime_voice_volume", \{ volume: form.voice.volume \}\)/);
  assert.match(source, /全局音量/);
  assert.match(source, /所有形象共用/);
  assert.match(source, /voice: \{ \.\.\.defaults.voice, volume: current.voice.volume \}/);
  assert.match(source, /disabled=\{saving \|\| !volumeReady\}/);
  const runtime = read("../src-tauri/src/realtime_chat.rs");
  assert.match(runtime, /input.voice.volume = voice_chat_settings::realtime_volume\(\)\?/);
  assert.match(runtime, /playback_volume: AtomicU32::new\(input.voice.volume\(\).to_bits\(\)\)/);
  assert.match(runtime, /BASE64.encode\(&pcm\)/);
  const synthesis = runtime.slice(runtime.indexOf("async fn speak_one_logged"), runtime.indexOf("async fn wait_playback"));
  assert.doesNotMatch(synthesis, /scale_pcm16/);
  assert.match(synthesis, /PlayerCmd::Pcm\(pcm.to_vec\(\)\)/);
});

test("device context updates only the edited appearance and its immediate button target", () => {
  const source = read("./shell/DeviceContext.jsx");
  const body = source.slice(source.indexOf("    const onPersonaSaved ="), source.indexOf("    window.addEventListener(PERSONA_VOICE_UPDATED_EVENT"));
  let records = [{ id: "a", personaVoice: "old" }, { id: "b", personaVoice: "other" }];
  const target = { current: { appearance: records[0], boardDeviceId: "board" } };
  const handler = new Function("setAppearances", "realtimeTargetRef", `${body}; return onPersonaSaved;`)(fn => { records = fn(records); }, target);
  handler({ detail: { appearanceId: "a", personaVoice: "new" } });
  assert.equal(records[0].personaVoice, "new"); assert.equal(records[1].personaVoice, "other");
  assert.equal(target.current.appearance.personaVoice, "new");
  handler({ detail: { appearanceId: "b", personaVoice: "other-new" } });
  assert.equal(target.current.appearance.id, "a"); assert.equal(target.current.appearance.personaVoice, "new");
  assert.equal(target.current.boardDeviceId, "board");
});
