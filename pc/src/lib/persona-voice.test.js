/**
 * [Input] `lib/persona-voice.js` pure helpers.
 * [Output] Unit coverage for defaults, factory Terrier persona, normalization/configured flag,
 *          validation, speaker labels, realtime system prompt assembly, and the pending-setup marker.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  BUILTIN_TERRIER_PERSONA_VOICE,
  DEFAULT_SPEAKER_ID,
  DOUBAO_SPEAKERS,
  PERSONA_TEMPLATES,
  TTS_PROVIDER_DOUBAO,
  buildRealtimeSystemPrompt,
  clearPersonaSetupPending,
  createDefaultPersonaVoice,
  effectiveSpeakerId,
  listPersonaSetupPending,
  markPersonaSetupPending,
  normalizePersonaVoice,
  personaSummaryLabel,
  validatePersonaVoice,
} from "./persona-voice.js";

test("built-in Terrier ships a factory persona and the default Doubao speaker", () => {
  const factory = normalizePersonaVoice(null, { name: "西高地小狗", isBuiltin: true });
  assert.equal(factory.configured, true);
  assert.equal(factory.persona.display_name, "小西");
  assert.match(factory.persona.greeting, /汪/);
  assert.equal(factory.voice.provider, TTS_PROVIDER_DOUBAO);
  assert.equal(factory.voice.speaker, DEFAULT_SPEAKER_ID);
  assert.equal(BUILTIN_TERRIER_PERSONA_VOICE.voice.speaker, "zh_female_vv_uranus_bigtts");
  assert.ok(DOUBAO_SPEAKERS.some((speaker) => speaker.id === DEFAULT_SPEAKER_ID));
});

test("a custom appearance without a document gets defaults but is reported as not configured", () => {
  const record = normalizePersonaVoice(null, { name: "Apex Nessie", description: "一只好奇又温柔的绿色长颈小水怪。" });
  assert.equal(record.configured, false);
  assert.equal(record.persona.display_name, "Apex Nessie");
  assert.equal(record.persona.intro, "一只好奇又温柔的绿色长颈小水怪。");
  assert.equal(record.persona.style, PERSONA_TEMPLATES[0].style);
  assert.equal(record.voice.speaker, DEFAULT_SPEAKER_ID);
  assert.equal(personaSummaryLabel(record), "未配置人设");
});

test("a saved document is normalized, clamped and reported as configured", () => {
  const record = normalizePersonaVoice(
    {
      persona: { display_name: " Torti ", style: "毒舌", language: "EN", forbidden: "" },
      voice: { provider: "something-else", speaker: "zh_female_gaolengyujie_moon_bigtts", speed: 9, volume: -1 },
    },
    { name: "Torti" },
  );
  assert.equal(record.configured, true);
  assert.equal(record.persona.display_name, "Torti");
  assert.equal(record.persona.language, "en");
  assert.equal(record.persona.forbidden, "");
  assert.equal(record.voice.provider, TTS_PROVIDER_DOUBAO);
  assert.equal(record.voice.speed, 2);
  assert.equal(record.voice.volume, 0.2);
  assert.equal(personaSummaryLabel(record), "Torti · Vivi 2.0");
  assert.match(record.voiceMigrationNotice, /旧版音色/);
});

test("validation requires name, style and a speaker; cloned voice satisfies the speaker rule", () => {
  const empty = validatePersonaVoice({ persona: { display_name: "", style: "" }, voice: { speaker: "" } });
  assert.deepEqual(Object.keys(empty).sort(), ["display_name", "speaker", "style"]);
  const cloned = validatePersonaVoice({ persona: { display_name: "A", style: "B" }, voice: { speaker: "", clone_speaker_id: "S_abc" } });
  assert.deepEqual(cloned, {});
  assert.equal(effectiveSpeakerId({ speaker: "x", clone_speaker_id: "S_abc" }), "S_abc");
  assert.equal(effectiveSpeakerId({ speaker: "x" }), "x");
});

test("realtime system prompt carries persona body, language rule and spoken-style rules", () => {
  const prompt = buildRealtimeSystemPrompt(createDefaultPersonaVoice({ name: "小黑", description: "一只黑猫" }).persona);
  assert.match(prompt, /^你是「小黑」。一只黑猫/);
  assert.match(prompt, /始终用中文回答/);
  assert.match(prompt, /不超过五十个字/);
  assert.match(prompt, /不要输出 Markdown/);
});

test("pending-setup marker is a no-op without localStorage and round-trips with it", () => {
  markPersonaSetupPending("a1");
  assert.deepEqual(listPersonaSetupPending(), []);
  const store = new Map();
  globalThis.localStorage = {
    getItem: (key) => (store.has(key) ? store.get(key) : null),
    setItem: (key, value) => store.set(key, String(value)),
  };
  try {
    markPersonaSetupPending("a1");
    markPersonaSetupPending("a2");
    assert.deepEqual(listPersonaSetupPending(), ["a1", "a2"]);
    clearPersonaSetupPending("a1");
    assert.deepEqual(listPersonaSetupPending(), ["a2"]);
  } finally {
    delete globalThis.localStorage;
  }
});
